/**
 * @file net_mgr_wifi.c
 * @brief netmgr 仲裁层内部：WiFi 链路实现（STA 连热点 / AP 发射热点）
 * @details 本文件只做**业务编排**，不认底层进程与配置格式：
 *          STA 侧控制与进程交 sys 层 net_mgr_wpa（连接/断开/状态/扫描），
 *          AP 侧配置与进程交 sys 层 net_mgr_ap（hostapd + udhcpd），
 *          本层负责模式分派（STA/AP 互斥，同一射频不并发）、接口地址配置、
 *          取址方式选择与失败路径回收。
 */
#include "net_mgr_core_int.h"

#include "net_mgr_dhcp.h"
#include "net_mgr_err.h"
#include "net_mgr_iface.h"
#include "net_mgr_proc.h"
#include "net_mgr_util.h"
#include "net_mgr_wpa.h"
#include "net_mgr_ap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/** 默认 WiFi 接口名 */
#define NETMGR_DEV_WIFI     "wlan0"
/** wpa_supplicant 启动配置文件路径 */
#define NETMGR_WPA_CONF     "/tmp/netmgr_wpa.conf"
/** AP 模式默认本机地址 */
#define NETMGR_AP_IP        "192.168.4.1"
/** AP 模式默认掩码 */
#define NETMGR_AP_MASK      "255.255.255.0"
/** AP 模式默认分配起始地址 */
#define NETMGR_AP_START     "192.168.4.100"
/** AP 模式默认分配结束地址 */
#define NETMGR_AP_END       "192.168.4.200"
/** WPA 预共享密钥最小长度 */
#define WPA_PSK_MIN    8

/** WiFi 链路私有数据 */
typedef struct {
    NETMGR_DHCP_BASE dhcp; /**< DHCP 公共头（链路类型 + 取址状态），须为首成员供公共回调使用 */
    NETMGR_WIFI_CONF conf; /**< 配置副本，生命周期与链路上下文一致 */
} NETMGR_WIFI_PRIV;

static int wifi_open(NETMGR_CTX* ctx, const void* conf);
static int wifi_close(NETMGR_CTX* ctx);
static int wifi_refresh(NETMGR_CTX* ctx);
static int sta_start(NETMGR_WIFI_PRIV* priv, NETMGR_CTX* ctx);
static int ap_start(NETMGR_WIFI_PRIV* priv, NETMGR_CTX* ctx);

/** WiFi 链路操作表 */
const NETMGR_OPS core_wifi_ops = {wifi_open, wifi_close, wifi_refresh};

/**
 * @brief STA 模式拉起：启动 wpa_supplicant、下发网络参数、连接并取址
 * @param priv 私有数据，不可为空
 * @param ctx  链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int sta_start(NETMGR_WIFI_PRIV* priv, NETMGR_CTX* ctx)
{
    int ret = 0; /* 子步骤返回值 */

    /* 第 1 步：STA 与 AP 互斥（同一射频不并发）：AP 侧进程由 ap 模块统一结束 */
    ret = netmgr_ap_stop();
    if (ret < 0)
        return ret;

    /* 第 2 步：wpa_supplicant 的拉起、写配置与控制通道建立由 sys 层 wpa 模块负责；
       国家码暂不指定（NULL），由系统 regulatory 配置决定，需要时经配置字段下发 */
    ret = netmgr_wpa_start(priv->conf.ifname, NETMGR_WPA_CONF, NULL, 0);
    if (ret < 0)
        return ret;

    /* 第 3 步：下发网络参数并等待关联完成，协议细节全部在 sys 层 */
    ret = netmgr_wpa_connect(priv->conf.ssid, priv->conf.passwd, 0);
    if (ret < 0)
        return ret;

    ret = netmgr_iface_enable(priv->conf.ifname, 1);
    if (ret < 0)
    {
        return ret;
    }

    snprintf(ctx->ifname, sizeof(ctx->ifname), "%s", priv->conf.ifname);
    snprintf(ctx->dns, sizeof(ctx->dns), "%s", priv->conf.dns);
    ctx->priv = priv;

    /* 第 5 步：按取址方式配置，DHCP 异步取址，静态地址在此同步完成 */
    if (!priv->conf.use_dhcp)
    {
        ret = netmgr_iface_setip(priv->conf.ifname, priv->conf.ip);
        if (ret < 0)
            return ret;
        ret = netmgr_iface_setmask(priv->conf.ifname, priv->conf.mask);
        if (ret < 0)
            return ret;
        snprintf(ctx->gate, sizeof(ctx->gate), "%s", priv->conf.gate);
        ctx->state = NETMGR_ST_READY;
        return 0;
    }

    ret = netmgr_dhcp_start(priv->conf.ifname, core_dhcp_cb, priv);
    if (ret < 0)
    {
        return ret;
    }
    return 0;
}

/**
 * @brief AP 模式拉起：配置本机地址后交由 ap 模块拉起 hostapd 与 udhcpd
 * @details 顺序固定为「先地址、后服务」：hostapd 起信标前接口必须已启用并有本机地址，
 *          udhcpd 的 router/subnet 选项也取自该地址
 * @param priv 私有数据，不可为空
 * @param ctx  链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int ap_start(NETMGR_WIFI_PRIV* priv, NETMGR_CTX* ctx)
{
    NETMGR_APCONF apconf = {0}; /* AP 侧配置（sys 层只认自己的结构） */
    int ret = 0;                /* 子步骤返回值 */

    /* 第 1 步：STA 与 AP 互斥（同一射频不并发），先停 wpa_supplicant */
    ret = netmgr_wpa_stop();
    if (ret < 0)
        return ret;

    /* 第 2 步：启用接口并配置本机地址，AP 自身不承担默认路由 */
    ret = netmgr_iface_enable(priv->conf.ifname, 1);
    if (ret < 0)
        return ret;
    ret = netmgr_iface_setip(priv->conf.ifname, priv->conf.ap_ip);
    if (ret < 0)
        return ret;
    ret = netmgr_iface_setmask(priv->conf.ifname, priv->conf.ap_mask);
    if (ret < 0)
        return ret;

    /* 第 3 步：配置写盘与两个常驻进程的起停交给 sys 层 ap 模块 */
    (void)snprintf(apconf.ifname, sizeof(apconf.ifname), "%s", priv->conf.ifname);
    (void)snprintf(apconf.ssid, sizeof(apconf.ssid), "%s", priv->conf.ssid);
    (void)snprintf(apconf.passwd, sizeof(apconf.passwd), "%s", priv->conf.passwd);
    (void)snprintf(apconf.ap_ip, sizeof(apconf.ap_ip), "%s", priv->conf.ap_ip);
    (void)snprintf(apconf.ap_mask, sizeof(apconf.ap_mask), "%s", priv->conf.ap_mask);
    (void)snprintf(apconf.ap_start, sizeof(apconf.ap_start), "%s", priv->conf.ap_start);
    (void)snprintf(apconf.ap_end, sizeof(apconf.ap_end), "%s", priv->conf.ap_end);
    (void)snprintf(apconf.dns, sizeof(apconf.dns), "%s", priv->conf.dns);
    ret = netmgr_ap_start(&apconf);
    if (ret < 0)
        return ret;

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
        return -NETMGRERR_PARAM;
    }
    if ('\0' == cfg->ssid[0])
    {
        return -NETMGRERR_CONF;
    }
    if ('\0' != cfg->passwd[0] && (int)strlen(cfg->passwd) < WPA_PSK_MIN)
    {
        return -NETMGRERR_CONF;
    }
    /* STA 静态模式：地址三要素缺一不可，否则无法取址或无法参与默认路由 */
    if (NETMGR_MODE_STA == cfg->mode && !cfg->use_dhcp)
    {
        if (!netmgr_ip_valid(cfg->ip) || !netmgr_ip_valid(cfg->mask) ||
            !netmgr_ip_valid(cfg->gate))
        {
            return -NETMGRERR_CONF;
        }
    }

    priv = (NETMGR_WIFI_PRIV*)calloc(1, sizeof(NETMGR_WIFI_PRIV));
    if (NULL == priv)
    {
        return -NETMGRERR_MEM;
    }
    memcpy(&priv->conf, cfg, sizeof(priv->conf));
    priv->dhcp.type = NETMGR_WIFI; /* 公共回调经它上报事件 */

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

    if (!netmgr_iface_exist(priv->conf.ifname))
    {
        free(priv);
        return -NETMGRERR_NOTFOUND;
    }
    if (!netmgr_ip_valid(priv->conf.ap_ip) || !netmgr_ip_valid(priv->conf.ap_mask))
    {
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
        /* 清理过程中的次生失败不覆盖主错误码；priv 可能已挂到上下文，需一并复位 */
        netmgr_wpa_close();
        (void)netmgr_dhcp_stop(priv->conf.ifname);
        (void)netmgr_ap_stop();
        free(priv);
        ctx->priv = NULL;
        ctx->state = NETMGR_ST_DOWN;
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
    int ret = 0;                   /* 停止 DHCP 的返回值 */

    if (NULL == ctx)
    {
        return -NETMGRERR_PARAM;
    }

    priv = (NETMGR_WIFI_PRIV*)ctx->priv;
    /* 停止取址失败不阻断资源释放，错误码在末尾上抛 */
    if ('\0' != ctx->ifname[0])
        ret = netmgr_dhcp_stop(ctx->ifname);

    /* wpa_supplicant 的停止（断开 → 关通道 → 结束进程）由 sys 层 wpa 模块负责；
       AP 相关进程属跨进程编排，仍在本层回收 */
    if (NULL != priv)
        (void)netmgr_wpa_stop();
    (void)netmgr_ap_stop();

    free(ctx->priv);
    ctx->priv = NULL;
    ctx->state = NETMGR_ST_DOWN;
    ctx->route_on = 0;
    ctx->err = 0;
    return ret;
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
        return -NETMGRERR_PARAM;
    }
    priv = (NETMGR_WIFI_PRIV*)ctx->priv;

    /* 接口已消失（USB 网卡拔出、驱动卸载等）：直接判 DOWN 并给出原因，跳过后续查询 */
    if (!netmgr_iface_exist(ctx->ifname))
    {
        ctx->state = NETMGR_ST_DOWN;
        ctx->err = -NETMGRERR_NOTFOUND;
        return 0;
    }

    /* STA 模式先刷新关联信息：wpa 状态不产生 netlink 事件，由仲裁层比对前后差异生成事件 */
    if (NETMGR_MODE_STA == priv->conf.mode)
    {
        NETMGR_WPA_STATE wstate = NETMGR_WPA_UNKNOWN; /* 当前 wpa 工作状态 */

        if (0 == netmgr_wpa_state(&wstate) && NETMGR_WPA_COMPLETED == wstate)
        {
            (void)netmgr_wpa_status(&ctx->wifi);
            ctx->wifi.connected = ('\0' != ctx->wifi.ssid[0]) ? 1 : 0;
        }
        else
        {
            memset(&ctx->wifi, 0, sizeof(ctx->wifi));
        }
    }

    /* AP 模式：进程在且地址在即视为就绪，网关恒为空不参与路由仲裁 */
    if (NETMGR_MODE_AP == priv->conf.mode)
    {
        ctx->ip[0] = '\0';
        ctx->mask[0] = '\0';
        ctx->gate[0] = '\0';
        ctx->err = 0; /* AP 模式不参与 DHCP，无取址异常码 */
        memset(&ctx->wifi, 0, sizeof(ctx->wifi)); /* AP 模式不作为站点连接热点 */
        if (!netmgr_ap_running() || !netmgr_iface_ready(priv->conf.ifname))
        {
            ctx->state = NETMGR_ST_DOWN;
            return 0;
        }
        ret = netmgr_iface_ip(priv->conf.ifname, ctx->ip, (int)sizeof(ctx->ip));
        if (ret < 0)
            return ret;
        ret = netmgr_iface_mask(priv->conf.ifname, ctx->mask, (int)sizeof(ctx->mask));
        if (ret < 0)
            return ret;
        ctx->state = ('\0' != ctx->ip[0]) ? NETMGR_ST_READY : NETMGR_ST_DOWN;
        return 0;
    }

    /* STA 静态模式：只看连通与地址在位，网关取配置值 */
    if (!priv->conf.use_dhcp)
    {
        ret = core_link_ready_static(ctx, priv->conf.gate);
        ctx->err = 0;
        return ret;
    }

    /* STA DHCP 模式：连通、地址、网关齐全才算就绪，本函数只做状态投影 */
    ret = core_link_ready(ctx);
    if (ret < 0)
        return ret;
    /* 未就绪原因：取址失败给出具体错误码；已持有租约却掉线置 -NETMGRERR_DHCP */
    if (NETMGR_ST_DOWN == ctx->state && NETMGR_DHCP_FAIL == priv->dhcp.dhcpState)
        ctx->err = (int)priv->dhcp.dhcpErr;
    else if (NETMGR_ST_DOWN == ctx->state && NETMGR_DHCP_SUCC == priv->dhcp.dhcpState)
        ctx->err = -NETMGRERR_DHCP;
    else
        ctx->err = 0;
    return 0;
}


