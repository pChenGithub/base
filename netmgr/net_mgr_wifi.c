/**
 * @file net_mgr_wifi.c
 * @brief WiFi 链路实现：STA 模式连接外部热点，AP 模式发射热点并分配地址
 * @details 直接与 wpa_supplicant 的 unix 控制接口通信（CTRL-REQ/CTRL-RSP 最小协议），
 *          不引用 base 内其它模块的 wpa 封装；AP 模式通过 hostapd + udhcpd 提供接入与 DNS。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "net_mgr_def.h"

/** 控制接口轮询间隔（微秒） */
#define WPA_POLL_US    100000
/** 控制接口就绪等待轮询次数，共 2 秒 */
#define WPA_OPEN_MAX   20
/** STA 连接完成等待轮询次数，共 10 秒 */
#define WPA_CONN_MAX   100
/** WPA 预共享密钥最小长度 */
#define WPA_PSK_MIN    8
/** wpa 回复前缀长度（"CTRL-RSP"） */
#define WPA_RSP_LEN    8
/** wpa 控制接口路径缓冲长度，与 sockaddr_un.sun_path 对齐 */
#define WPA_PATH_LEN   108
/** wpa 回复失败标记长度（"FAIL"） */
#define WPA_FAIL_LEN   4

/** WiFi 链路私有数据 */
typedef struct {
    NETMGR_WIFI_CONF conf; /**< 配置副本，生命周期与链路上下文一致 */
    int sock;              /**< wpa 控制 socket，-1 表示未连接 */
} NETMGR_WIFI_PRIV;

static int wifi_open(NETMGR_CTX* ctx, const void* conf);
static int wifi_close(NETMGR_CTX* ctx);
static int wifi_refresh(NETMGR_CTX* ctx);
static int sta_start(NETMGR_WIFI_PRIV* priv, NETMGR_CTX* ctx);
static int ap_start(NETMGR_WIFI_PRIV* priv, NETMGR_CTX* ctx);
static int wpa_open(NETMGR_WIFI_PRIV* priv);
static void wpa_close(NETMGR_WIFI_PRIV* priv);

/** WiFi 链路操作表 */
const NETMGR_OPS netmgr_wifi_ops = {wifi_open, wifi_close, wifi_refresh};

/**
 * @brief 连接 wpa_supplicant 控制接口
 * @param priv 私有数据，不可为空
 * @return 0 成功（已连接时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
static int wpa_open(NETMGR_WIFI_PRIV* priv)
{
    struct sockaddr_un addr; /* 控制接口地址 */
    struct timeval tv;       /* 收发超时 */
    char path[WPA_PATH_LEN]; /* 控制接口路径，长度与 sun_path 对齐避免截断 */
    int fd = -1;             /* 控制 socket */
    int err = 0;             /* 失败时的 errno 快照 */

    if (NULL == priv)
    {
        NETMGR_ERR("入参 priv 为空");
        return -NETMGRERR_PARAM;
    }
    if (priv->sock >= 0)
        return 0;

    if (snprintf(path, sizeof(path), "%s/%s", NETMGR_WPA_DIR, priv->conf.ifname) >=
        (int)sizeof(path))
    {
        NETMGR_ERR("控制接口路径超长, ifname=%s", priv->conf.ifname);
        return -NETMGRERR_BUFF;
    }

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        NETMGR_ERR("创建控制 socket 失败, errno=%d", errno);
        return -NETMGRERR_SOCKET;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (0 != connect(fd, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)))
    {
        err = errno;
        close(fd);
        NETMGR_ERR("连接 %s 失败, errno=%d", path, err);
        return -NETMGRERR_WPA;
    }

    /* 设置收发超时，避免 wpa 无响应时调用线程被永久阻塞 */
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    if (0 != setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ||
        0 != setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)))
        NETMGR_ERR("%s 设置超时失败, errno=%d", path, errno);

    priv->sock = fd;
    return 0;
}

/**
 * @brief 关闭 wpa 控制接口
 * @param priv 私有数据，不可为空
 */
static void wpa_close(NETMGR_WIFI_PRIV* priv)
{
    if (NULL == priv || priv->sock < 0)
        return;
    close(priv->sock);
    priv->sock = -1;
}

/**
 * @brief 发送一条 wpa 控制命令并读取回复正文
 * @param priv 私有数据，不可为空且已连接
 * @param cmd  命令文本，不可为空
 * @param out  回复正文输出缓冲，可为空
 * @param len  输出缓冲长度
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int wpa_cmd(NETMGR_WIFI_PRIV* priv, const char* cmd, char* out, int len)
{
    char buf[NETMGR_WPA_LEN]; /* 回复原始缓冲 */
    char* data = NULL;        /* 回复正文起点 */
    ssize_t n = 0;            /* 收发字节数 */

    if (NULL == priv || NULL == cmd || priv->sock < 0)
    {
        NETMGR_ERR("控制通道未就绪, cmd=%s", (NULL == cmd) ? "" : cmd);
        return -NETMGRERR_WPA;
    }

    if (0 > send(priv->sock, cmd, strlen(cmd), 0))
    {
        NETMGR_ERR("发送命令失败, cmd=%s, errno=%d", cmd, errno);
        return -NETMGRERR_WPA;
    }

    memset(buf, 0, sizeof(buf));
    n = recv(priv->sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
    {
        NETMGR_ERR("接收命令回复失败, cmd=%s, errno=%d", cmd, errno);
        return -NETMGRERR_WPA;
    }

    /* 回复格式为 CTRL-RSP-<序号>-<长度>\n<正文>，正文取首个换行之后 */
    if (0 != strncmp(buf, "CTRL-RSP", WPA_RSP_LEN))
    {
        NETMGR_ERR("回复格式异常, cmd=%s, head=%.32s", cmd, buf);
        return -NETMGRERR_WPA;
    }
    data = strchr(buf, '\n');
    data = (NULL == data) ? buf : (data + 1);

    if (0 == strncmp(data, "FAIL", WPA_FAIL_LEN))
    {
        NETMGR_ERR("命令执行失败, cmd=%s, rsp=%.64s", cmd, data);
        return -NETMGRERR_WPA;
    }

    if (NULL != out && len > 0)
        snprintf(out, (size_t)len, "%s", data);
    return 0;
}

/**
 * @brief 写 wpa_supplicant 最小配置（控制接口、配置回写、国家码）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int wpa_conf_write(void)
{
    FILE* fp = NULL; /* 配置文件句柄 */

    fp = fopen(NETMGR_WPA_CONF, "w");
    if (NULL == fp)
    {
        NETMGR_ERR("打开 %s 失败, errno=%d", NETMGR_WPA_CONF, errno);
        return -NETMGRERR_FILE;
    }

    if (fprintf(fp, "ctrl_interface=%s\nupdate_config=1\ncountry=CN\n", NETMGR_WPA_DIR) < 0)
    {
        NETMGR_ERR("写入 %s 失败, errno=%d", NETMGR_WPA_CONF, errno);
        fclose(fp);
        return -NETMGRERR_FILE;
    }
    fflush(fp);
    fclose(fp);
    return 0;
}

/**
 * @brief 等待 wpa_supplicant 控制接口就绪
 * @param ifname 接口名，不可为空
 * @return 0 就绪；负值为 NETMGRERR_* 错误码
 */
static int wpa_wait(const char* ifname)
{
    char path[WPA_PATH_LEN]; /* 控制接口路径，长度与 sun_path 对齐 */
    int i = 0;               /* 轮询次数，从 0 开始 */

    if (NULL == ifname)
    {
        NETMGR_ERR("入参 ifname 为空");
        return -NETMGRERR_PARAM;
    }
    if (snprintf(path, sizeof(path), "%s/%s", NETMGR_WPA_DIR, ifname) >= (int)sizeof(path))
    {
        NETMGR_ERR("控制接口路径超长, ifname=%s", ifname);
        return -NETMGRERR_BUFF;
    }

    for (i = 0; i < WPA_OPEN_MAX; ++i)
    {
        if (0 == access(path, F_OK))
            return 0;
        usleep(WPA_POLL_US);
    }

    NETMGR_ERR("等待 %s 就绪超时", path);
    return -NETMGRERR_TIMEOUT;
}

/**
 * @brief 等待 STA 进入连接完成状态
 * @param priv 私有数据，不可为空且已连接
 * @return 0 已连接；负值为 NETMGRERR_* 错误码
 */
static int wpa_wait_conn(NETMGR_WIFI_PRIV* priv)
{
    char out[NETMGR_WPA_LEN]; /* STATUS 回复 */
    int i = 0;                /* 轮询次数，从 0 开始 */

    if (NULL == priv)
    {
        NETMGR_ERR("入参 priv 为空");
        return -NETMGRERR_PARAM;
    }

    for (i = 0; i < WPA_CONN_MAX; ++i)
    {
        out[0] = '\0';
        if (0 == wpa_cmd(priv, "STATUS", out, (int)sizeof(out)) &&
            NULL != strstr(out, "wpa_state=COMPLETED"))
            return 0;
        usleep(WPA_POLL_US);
    }

    NETMGR_ERR("%s 连接热点超时", priv->conf.ifname);
    return -NETMGRERR_TIMEOUT;
}

/**
 * @brief STA 模式拉起：启动 wpa_supplicant、下发网络参数、连接并取址
 * @param priv 私有数据，不可为空
 * @param ctx  链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int sta_start(NETMGR_WIFI_PRIV* priv, NETMGR_CTX* ctx)
{
    char id[16] = {0};           /* wpa 网络配置编号 */
    char cmd[NETMGR_CMD_LEN];    /* 拼接后的控制命令 */
    int ret = 0;                 /* 子步骤返回值 */

    /* 第 1 步：保证 wpa_supplicant 由本模块独占启动，避免沿用未知配置 */
    (void)netmgr_proc_kill("hostapd");
    (void)netmgr_proc_kill("udhcpd");
    (void)netmgr_proc_kill("wpa_supplicant");

    ret = wpa_conf_write();
    if (ret < 0)
        return ret;

    ret = netmgr_proc_exec("wpa_supplicant -B -i %s -c %s", priv->conf.ifname, NETMGR_WPA_CONF);
    if (ret < 0)
    {
        NETMGR_ERR("%s 启动 wpa_supplicant 失败, ret=%d", priv->conf.ifname, ret);
        return -NETMGRERR_PROC;
    }

    /* 第 2 步：等待控制接口出现后建立连接 */
    ret = wpa_wait(priv->conf.ifname);
    if (ret < 0)
        return ret;
    ret = wpa_open(priv);
    if (ret < 0)
        return ret;

    /* 第 3 步：重建网络配置，编号由 wpa 分配 */
    (void)wpa_cmd(priv, "REMOVE_NETWORK all", NULL, 0);
    ret = wpa_cmd(priv, "ADD_NETWORK", id, (int)sizeof(id));
    if (ret < 0)
        return ret;
    id[strcspn(id, "\r\n")] = '\0';

    snprintf(cmd, sizeof(cmd), "SET_NETWORK %s ssid \"%s\"", id, priv->conf.ssid);
    ret = wpa_cmd(priv, cmd, NULL, 0);
    if (ret < 0)
        return ret;

    if ('\0' != priv->conf.passwd[0])
    {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %s psk \"%s\"", id, priv->conf.passwd);
        ret = wpa_cmd(priv, cmd, NULL, 0);
        if (ret < 0)
            return ret;
    }
    else
    {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %s key_mgmt NONE", id);
        ret = wpa_cmd(priv, cmd, NULL, 0);
        if (ret < 0)
            return ret;
    }

    snprintf(cmd, sizeof(cmd), "ENABLE_NETWORK %s", id);
    ret = wpa_cmd(priv, cmd, NULL, 0);
    if (ret < 0)
        return ret;
    snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %s", id);
    ret = wpa_cmd(priv, cmd, NULL, 0);
    if (ret < 0)
        return ret;

    /* 第 4 步：等待关联完成，再由 DHCP 取址 */
    ret = wpa_wait_conn(priv);
    if (ret < 0)
        return ret;

    ret = netmgr_link_enable(priv->conf.ifname, 1);
    if (ret < 0)
    {
        NETMGR_ERR("启用 %s 失败, ret=%d", priv->conf.ifname, ret);
        return ret;
    }

    snprintf(ctx->ifname, sizeof(ctx->ifname), "%s", priv->conf.ifname);
    snprintf(ctx->dns, sizeof(ctx->dns), "%s", priv->conf.dns);
    ctx->priv = priv;

    ret = netmgr_dhcp_start(priv->conf.ifname, 0);
    if (ret < 0)
    {
        NETMGR_ERR("%s 启动 DHCP 失败, ret=%d", priv->conf.ifname, ret);
        return ret;
    }
    return 0;
}

/**
 * @brief 写 hostapd 配置
 * @param conf WiFi 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int ap_conf_write(const NETMGR_WIFI_CONF* conf)
{
    FILE* fp = NULL; /* 配置文件句柄 */

    fp = fopen(NETMGR_AP_CONF, "w");
    if (NULL == fp)
    {
        NETMGR_ERR("打开 %s 失败, errno=%d", NETMGR_AP_CONF, errno);
        return -NETMGRERR_FILE;
    }

    /* 基础参数：接口、驱动、信道，密码为空时按开放网络输出 */
    fprintf(fp, "interface=%s\ndriver=nl80211\nssid=%s\nhw_mode=g\nchannel=6\n",
            conf->ifname, conf->ssid);
    if ('\0' != conf->passwd[0])
        fprintf(fp, "wpa=2\nwpa_passphrase=%s\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n",
                conf->passwd);

    if (0 != fflush(fp))
    {
        NETMGR_ERR("写入 %s 失败, errno=%d", NETMGR_AP_CONF, errno);
        fclose(fp);
        return -NETMGRERR_FILE;
    }
    fclose(fp);
    return 0;
}

/**
 * @brief 写 AP 模式 DHCP 服务配置，包含下发给客户端的 DNS
 * @param conf WiFi 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int ap_dhcp_conf(const NETMGR_WIFI_CONF* conf)
{
    FILE* fp = NULL; /* 配置文件句柄 */

    fp = fopen(NETMGR_UDHCP_CONF, "w");
    if (NULL == fp)
    {
        NETMGR_ERR("打开 %s 失败, errno=%d", NETMGR_UDHCP_CONF, errno);
        return -NETMGRERR_FILE;
    }

    fprintf(fp, "start %s\nend %s\ninterface %s\nopt router %s\nopt subnet %s\n",
            conf->ap_start, conf->ap_end, conf->ifname, conf->ap_ip, conf->ap_mask);
    /* DNS 仅在配置了服务器时下发，未配置则由客户端沿用自身设置 */
    if ('\0' != conf->dns[0])
        fprintf(fp, "opt dns %s\n", conf->dns);

    if (0 != fflush(fp))
    {
        NETMGR_ERR("写入 %s 失败, errno=%d", NETMGR_UDHCP_CONF, errno);
        fclose(fp);
        return -NETMGRERR_FILE;
    }
    fclose(fp);
    return 0;
}

/**
 * @brief AP 模式拉起：启动 hostapd、配置本机地址并启动 DHCP 服务
 * @param priv 私有数据，不可为空
 * @param ctx  链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int ap_start(NETMGR_WIFI_PRIV* priv, NETMGR_CTX* ctx)
{
    int ret = 0; /* 子步骤返回值 */

    /* 第 1 步：AP 与 STA 互斥，先停掉 STA 相关进程 */
    (void)netmgr_proc_kill("wpa_supplicant");
    (void)netmgr_proc_kill("udhcpd");
    (void)netmgr_proc_kill("hostapd");

    ret = ap_conf_write(&priv->conf);
    if (ret < 0)
        return ret;

    ret = netmgr_proc_exec("hostapd -B %s", NETMGR_AP_CONF);
    if (ret < 0)
    {
        NETMGR_ERR("%s 启动 hostapd 失败, ret=%d", priv->conf.ifname, ret);
        return -NETMGRERR_PROC;
    }

    /* 第 2 步：配置热点本机地址，AP 自身不承担默认路由 */
    ret = netmgr_link_enable(priv->conf.ifname, 1);
    if (ret < 0)
        return ret;
    ret = netmgr_link_setip(priv->conf.ifname, priv->conf.ap_ip);
    if (ret < 0)
        return ret;
    ret = netmgr_link_setmask(priv->conf.ifname, priv->conf.ap_mask);
    if (ret < 0)
        return ret;

    /* 第 3 步：启动 DHCP 服务，向接入的客户端分配地址与 DNS */
    ret = ap_dhcp_conf(&priv->conf);
    if (ret < 0)
        return ret;
    ret = netmgr_proc_exec("udhcpd %s", NETMGR_UDHCP_CONF);
    if (ret < 0)
    {
        NETMGR_ERR("%s 启动 udhcpd 失败, ret=%d", priv->conf.ifname, ret);
        return -NETMGRERR_PROC;
    }

    /* 第 4 步：挂载上下文，网关留空表示不参与默认路由仲裁 */
    snprintf(ctx->ifname, sizeof(ctx->ifname), "%s", priv->conf.ifname);
    snprintf(ctx->ip, sizeof(ctx->ip), "%s", priv->conf.ap_ip);
    ctx->gate[0] = '\0';
    ctx->priv = priv;
    ctx->state = NETMGR_ST_READY;
    return 0;
}

/**
 * @brief 拉起 WiFi 链路，按模式分派到 STA 或 AP 流程
 * @param ctx  链路上下文，不可为空
 * @param conf WiFi 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int wifi_open(NETMGR_CTX* ctx, const void* conf)
{
    const NETMGR_WIFI_CONF* cfg = (const NETMGR_WIFI_CONF*)conf; /* 入参配置视图 */
    NETMGR_WIFI_PRIV* priv = NULL;                               /* 私有数据 */
    int ret = 0;                                                 /* 子步骤返回值 */

    if (NULL == ctx || NULL == cfg)
    {
        NETMGR_ERR("入参非法");
        return -NETMGRERR_PARAM;
    }
    if ('\0' == cfg->ssid[0])
    {
        NETMGR_ERR("SSID 未配置");
        return -NETMGRERR_CONF;
    }
    if ('\0' != cfg->passwd[0] && (int)strlen(cfg->passwd) < WPA_PSK_MIN)
    {
        NETMGR_ERR("密码长度不足, 至少 %d 位", WPA_PSK_MIN);
        return -NETMGRERR_CONF;
    }

    priv = (NETMGR_WIFI_PRIV*)calloc(1, sizeof(NETMGR_WIFI_PRIV));
    if (NULL == priv)
    {
        NETMGR_ERR("申请 WiFi 链路上下文失败");
        return -NETMGRERR_MEM;
    }
    memcpy(&priv->conf, cfg, sizeof(priv->conf));
    priv->sock = -1;

    /* 补齐可选配置的默认值：接口名与 AP 参数 */
    if ('\0' == priv->conf.ifname[0])
        snprintf(priv->conf.ifname, sizeof(priv->conf.ifname), "%s", NETMGR_DEV_WIFI);
    if ('\0' == priv->conf.ap_ip[0])
        snprintf(priv->conf.ap_ip, sizeof(priv->conf.ap_ip), "%s", NETMGR_AP_IP);
    if ('\0' == priv->conf.ap_mask[0])
        snprintf(priv->conf.ap_mask, sizeof(priv->conf.ap_mask), "%s", NETMGR_AP_MASK);
    if ('\0' == priv->conf.ap_start[0])
        snprintf(priv->conf.ap_start, sizeof(priv->conf.ap_start), "%s", NETMGR_AP_START);
    if ('\0' == priv->conf.ap_end[0])
        snprintf(priv->conf.ap_end, sizeof(priv->conf.ap_end), "%s", NETMGR_AP_END);

    if (!netmgr_link_exist(priv->conf.ifname))
    {
        NETMGR_ERR("接口 %s 不存在", priv->conf.ifname);
        free(priv);
        return -NETMGRERR_NOTFOUND;
    }
    if (!netmgr_ip_valid(priv->conf.ap_ip) || !netmgr_ip_valid(priv->conf.ap_mask))
    {
        NETMGR_ERR("AP 地址配置非法, ip=%s, mask=%s", priv->conf.ap_ip, priv->conf.ap_mask);
        free(priv);
        return -NETMGRERR_CONF;
    }

    /* 按模式分派，失败时释放本层资源，避免半成品上下文 */
    if (NETMGR_MODE_AP == priv->conf.mode)
        ret = ap_start(priv, ctx);
    else
        ret = sta_start(priv, ctx);

    if (ret < 0)
    {
        wpa_close(priv);
        netmgr_dhcp_stop(priv->conf.ifname);
        (void)netmgr_proc_kill("hostapd");
        (void)netmgr_proc_kill("udhcpd");
        free(priv);
        return ret;
    }
    return 0;
}

/**
 * @brief 关闭 WiFi 链路并释放私有数据
 * @param ctx 链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int wifi_close(NETMGR_CTX* ctx)
{
    NETMGR_WIFI_PRIV* priv = NULL; /* 私有数据 */
    int ret = 0;                   /* 子步骤返回值 */

    if (NULL == ctx)
    {
        NETMGR_ERR("入参 ctx 为空");
        return -NETMGRERR_PARAM;
    }

    priv = (NETMGR_WIFI_PRIV*)ctx->priv;
    if ('\0' != ctx->ifname[0])
    {
        ret = netmgr_dhcp_stop(ctx->ifname);
        if (ret < 0)
            NETMGR_ERR("停止 %s 的 DHCP 失败, ret=%d", ctx->ifname, ret);
    }

    /* 先断开控制通道再结束进程，顺序保证控制命令能正常下发 */
    if (NULL != priv)
    {
        if (priv->sock >= 0)
            (void)wpa_cmd(priv, "DISCONNECT", NULL, 0);
        wpa_close(priv);
    }
    (void)netmgr_proc_kill("udhcpd");
    (void)netmgr_proc_kill("hostapd");
    (void)netmgr_proc_kill("wpa_supplicant");

    free(ctx->priv);
    ctx->priv = NULL;
    ctx->state = NETMGR_ST_DOWN;
    ctx->route_on = 0;
    return 0;
}

/**
 * @brief 刷新 WiFi 链路状态
 * @param ctx 链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int wifi_refresh(NETMGR_CTX* ctx)
{
    NETMGR_WIFI_PRIV* priv = NULL; /* 私有数据 */
    int ret = 0;                   /* 子步骤返回值 */

    if (NULL == ctx || NULL == ctx->priv)
    {
        NETMGR_ERR("链路上下文未初始化");
        return -NETMGRERR_PARAM;
    }
    priv = (NETMGR_WIFI_PRIV*)ctx->priv;

    /* AP 模式：进程在且地址在即视为就绪，网关恒为空不参与路由仲裁 */
    if (NETMGR_MODE_AP == priv->conf.mode)
    {
        ctx->ip[0] = '\0';
        ctx->gate[0] = '\0';
        if (!netmgr_proc_exist("hostapd") || !netmgr_link_up(priv->conf.ifname))
        {
            ctx->state = NETMGR_ST_DOWN;
            return 0;
        }
        ret = netmgr_link_ip(priv->conf.ifname, ctx->ip, (int)sizeof(ctx->ip));
        if (ret < 0)
            return ret;
        ctx->state = ('\0' != ctx->ip[0]) ? NETMGR_ST_READY : NETMGR_ST_DOWN;
        return 0;
    }

    /* STA 模式：连通、地址、网关齐全才算就绪 */
    ret = netmgr_link_ready(ctx);
    if (ret < 0)
        return ret;
    if (NETMGR_ST_DOWN == ctx->state && !netmgr_dhcp_busy(ctx->ifname))
    {
        ret = netmgr_dhcp_start(ctx->ifname, 0);
        if (ret < 0)
            NETMGR_ERR("%s 重新拉起 DHCP 失败, ret=%d", ctx->ifname, ret);
    }
    return 0;
}
