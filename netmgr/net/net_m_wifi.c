/**
 * @file net_m_wifi.c
 * @brief net WiFi 链路实现：STA 多热点择优 / AP 热点（net 层）
 * @details STA 侧支持「候选热点记忆 + 自动择优」：
 *          1) 候选按 prio 升序尝试，prio 为 0 的候选用扫描信号强度降序排列；
 *          2) 最近一次连接成功的候选（lastOk）在下一次启动时排到最前；
 *          3) 全部候选失败后由管理线程按退避策略调用 heal 重新尝试。
 *          本文件只做业务编排，wpa_supplicant / hostapd / udhcpd 的起停与协议细节
 *          全部在 sys 层；同一射频 STA 与 AP 互斥，切换前先停对侧进程。
 * @note 连接与扫描均为同步阻塞操作（单候选最长 10 秒、扫描最长 8 秒），
 *       期间管理线程被占用：查询接口读快照不受影响，配置命令需排队。
 */
#include "net_m_ctx.h"

#include "net_mgr_ap.h"
#include "net_mgr_dhcp.h"
#include "net_mgr_err.h"
#include "net_mgr_iface.h"
#include "net_mgr_util.h"
#include "net_mgr_wpa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** 默认 WiFi 接口名 */
#define NET_M_DEV_WIFI   "wlan0"
/** wpa_supplicant 启动配置文件路径 */
#define NET_M_WPA_CONF   "/tmp/net_m_wpa.conf"
/** AP 模式默认本机地址 */
#define NET_M_AP_IP      "192.168.4.1"
/** AP 模式默认掩码 */
#define NET_M_AP_MASK    "255.255.255.0"
/** AP 模式默认分配起始地址 */
#define NET_M_AP_START   "192.168.4.100"
/** AP 模式默认分配结束地址 */
#define NET_M_AP_END     "192.168.4.200"
/** WPA 预共享密钥最小长度 */
#define NET_M_PSK_MIN    8
/** 候选热点数量上限（与 NET_M_WIFI_CONF.cand 数组长度一致） */
#define NET_M_CAND_MAX   4
/** 未扫描到该 SSID 时的信号占位值 */
#define NET_M_SIGNAL_NONE (-127)

/** WiFi 链路私有数据 */
typedef struct {
    NET_M_HEAD head;        /**< 公共头（链路类型），须为首成员供 DHCP 回调使用 */
    NET_M_WIFI_CONF conf;   /**< 配置副本，生命周期与链路上下文一致 */
    int activeCand;         /**< 当前使用（或最后尝试）的候选下标，-1 表示未定 */
    int lastOk;             /**< 最近一次连接成功的候选下标，-1 表示无记忆 */
} NET_M_WIFI_PRIV;

static int wifi_open(NET_M_LINK_RT* link, const void* conf);
static int wifi_close(NET_M_LINK_RT* link);
static int wifi_refresh(NET_M_LINK_RT* link);
static int wifi_heal(NET_M_LINK_RT* link);

/** WiFi 链路操作表 */
const NET_M_OPS net_m_wifi_ops = {wifi_open, wifi_close, wifi_refresh, wifi_heal};

/**
 * @brief 校验单个候选热点
 * @param cand 候选配置，不可为空
 * @return 0 合法；-NETMGRERR_CONF 非法
 */
static int cand_check(const NET_M_WIFI_CAND* cand)
{
    if (NULL == cand)
    {
        return -NETMGRERR_PARAM;
    }
    if ('\0' == cand->ssid[0])
    {
        return -NETMGRERR_CONF;
    }
    if ('\0' != cand->passwd[0] && (int)strlen(cand->passwd) < NET_M_PSK_MIN)
    {
        return -NETMGRERR_CONF;
    }
    return 0;
}

/**
 * @brief 在扫描结果中查找指定 SSID 的最强信号
 * @param scan     扫描结果，可为空
 * @param scanNum  扫描条数
 * @param ssid     目标 SSID，不可为空
 * @return 信号强度（dBm）；未找到返回 NET_M_SIGNAL_NONE
 */
static int cand_signal(const NETMGR_WIFI_AP* scan, int scanNum, const char* ssid)
{
    int best = NET_M_SIGNAL_NONE; /* 最强信号 */
    int i = 0;                    /* 扫描结果下标，从 0 开始 */

    if (NULL == scan || NULL == ssid)
    {
        return NET_M_SIGNAL_NONE;
    }

    for (i = 0; i < scanNum; ++i)
    {
        if (0 != strcmp(scan[i].ssid, ssid))
        {
            continue;
        }
        if (scan[i].signal > best)
        {
            best = scan[i].signal;
        }
    }
    return best;
}

/**
 * @brief 生成候选尝试顺序
 * @details 排序规则：最近连接成功者最前 → prio 非 0 者按 prio 升序 →
 *          prio 为 0 者按扫描信号降序（未扫描到时排最后）
 * @param priv    私有数据，不可为空
 * @param scan    扫描结果，可为空
 * @param scanNum 扫描条数
 * @param order   输出顺序数组，容量不少于 NET_M_CAND_MAX，不可为空
 */
static void cand_order(const NET_M_WIFI_PRIV* priv, const NETMGR_WIFI_AP* scan, int scanNum,
                       int* order)
{
    const NET_M_WIFI_CAND* cur = NULL;  /* 待插入的候选 */
    const NET_M_WIFI_CAND* prev = NULL; /* 比较对象 */
    int prevIdx = 0;                    /* 已排序元素下标 */
    int curBetter = 0;                  /* 1=新元素应排前面 */
    int count = 0;                      /* 已排序数量 */
    int i = 0;                          /* 待插入元素下标 */
    int j = 0;                          /* 已排序区间游标 */

    if (NULL == priv || NULL == order)
    {
        return;
    }

    for (i = 0; i < priv->conf.candNum && i < NET_M_CAND_MAX; ++i)
    {
        /* 插入排序：新元素向前找到第一个「不优于它」的位置，保证稳定性 */
        j = count;
        cur = &priv->conf.cand[i];
        while (j > 0)
        {
            prevIdx = order[j - 1];
            prev = &priv->conf.cand[prevIdx];
            curBetter = 0;

            if (i == priv->lastOk)
            {
                curBetter = 1; /* 有成功记忆的候选永远优先 */
            }
            else if (prevIdx == priv->lastOk)
            {
                curBetter = 0;
            }
            else if (0 != cur->prio && 0 == prev->prio)
            {
                curBetter = 1; /* 固定优先序优于自动择优 */
            }
            else if (0 == cur->prio && 0 != prev->prio)
            {
                curBetter = 0;
            }
            else if (0 != cur->prio && 0 != prev->prio)
            {
                curBetter = (cur->prio < prev->prio) ? 1 : 0;
            }
            else
            {
                curBetter = (cand_signal(scan, scanNum, cur->ssid) >
                             cand_signal(scan, scanNum, prev->ssid)) ? 1 : 0;
            }

            if (0 == curBetter)
            {
                break;
            }
            order[j] = order[j - 1];
            --j;
        }
        order[j] = i;
        ++count;
    }
}

/**
 * @brief 依次尝试候选热点，成功即返回
 * @details 单个候选的关联等待由 sys 层默认值决定（10 秒）；全部失败返回最后一个错误码
 * @param priv  私有数据，不可为空
 * @param order 候选尝试顺序，不可为空
 * @param count 顺序表长度
 * @return 0 已关联；负值为错误码
 */
static int cand_connect(NET_M_WIFI_PRIV* priv, const int* order, int count)
{
    NET_M_WIFI_CAND* cand = NULL; /* 候选配置 */
    int idx = 0;                  /* 候选下标 */
    int ret = -NETMGRERR_CONF;     /* 最近一次失败码 */
    int i = 0;                    /* 顺序表下标，从 0 开始 */

    for (i = 0; i < count; ++i)
    {
        idx = order[i];
        cand = &priv->conf.cand[idx];

        ret = netmgr_wpa_connect(cand->ssid, cand->passwd, 0);
        if (0 == ret)
        {
            priv->activeCand = idx;
            priv->lastOk = idx;
            return 0;
        }
    }
    return ret;
}

/**
 * @brief 建立 wpa 控制通道并择优连接（STA 模式）
 * @param priv 私有数据，不可为空
 * @return 0 已关联；负值为错误码
 */
static int sta_connect(NET_M_WIFI_PRIV* priv)
{
    NETMGR_WIFI_AP scan[NETMGR_SCAN_MAX]; /* 扫描结果列表 */
    int order[NET_M_CAND_MAX];            /* 候选尝试顺序 */
    int scanNum = (int)(sizeof(scan) / sizeof(scan[0])); /* 扫描容量 */
    int ret = 0;                          /* 子步骤返回值 */
    int i = 0;                            /* 循环下标，从 0 开始 */

    /* 第 1 步：STA 与 AP 互斥，先停 AP 侧进程 */
    ret = netmgr_ap_stop();
    if (ret < 0)
    {
        return ret;
    }

    /* 第 2 步：控制通道未建立时才拉起 wpa_supplicant；
       已建立时直接复用，避免自愈路径反复重建进程 */
    if (0 == netmgr_wpa_is_open())
    {
        ret = netmgr_wpa_start(priv->conf.ifname, NET_M_WPA_CONF, NULL, 0);
        if (ret < 0)
        {
            return ret;
        }
    }

    /* 第 3 步：扫描一次为自动择优提供信号依据；失败不阻断（退化为按配置顺序尝试） */
    if (0 != netmgr_wpa_scan(0))
    {
        scanNum = 0;
    }
    else if (0 != netmgr_wpa_scan_result(scan, &scanNum))
    {
        scanNum = 0;
    }

    /* 第 4 步：按顺序尝试候选 */
    for (i = 0; i < NET_M_CAND_MAX; ++i)
    {
        order[i] = 0;
    }
    cand_order(priv, scan, scanNum, order);
    return cand_connect(priv, order, priv->conf.candNum);
}

/**
 * @brief 启动 AP 模式：配置本机地址后交由 sys 层拉起 hostapd 与 udhcpd
 * @details 顺序固定为「先地址、后服务」：hostapd 起信标前必须已启用并有本机地址
 * @param priv 私有数据，不可为空
 * @param link 链路运行时上下文，不可为空
 * @return 0 成功；负值为错误码
 */
static int ap_start(NET_M_WIFI_PRIV* priv, NET_M_LINK_RT* link)
{
    NETMGR_APCONF apconf = {0}; /* sys 层 AP 配置 */
    int ret = 0;                /* 子步骤返回值 */

    /* 第 1 步：STA 与 AP 互斥，先停 wpa_supplicant */
    ret = netmgr_wpa_stop();
    if (ret < 0)
    {
        return ret;
    }

    /* 第 2 步：启用接口并配置本机地址 */
    ret = netmgr_iface_enable(priv->conf.ifname, 1);
    if (ret < 0)
    {
        return ret;
    }
    ret = netmgr_iface_setip(priv->conf.ifname, priv->conf.apIp);
    if (ret < 0)
    {
        return ret;
    }
    ret = netmgr_iface_setmask(priv->conf.ifname, priv->conf.apMask);
    if (ret < 0)
    {
        return ret;
    }

    /* 第 3 步：写盘与进程起停交给 sys 层 ap 模块 */
    (void)snprintf(apconf.ifname, sizeof(apconf.ifname), "%s", priv->conf.ifname);
    (void)snprintf(apconf.ssid, sizeof(apconf.ssid), "%s", priv->conf.apSsid);
    (void)snprintf(apconf.passwd, sizeof(apconf.passwd), "%s", priv->conf.apPasswd);
    (void)snprintf(apconf.ap_ip, sizeof(apconf.ap_ip), "%s", priv->conf.apIp);
    (void)snprintf(apconf.ap_mask, sizeof(apconf.ap_mask), "%s", priv->conf.apMask);
    (void)snprintf(apconf.ap_start, sizeof(apconf.ap_start), "%s", priv->conf.apStart);
    (void)snprintf(apconf.ap_end, sizeof(apconf.ap_end), "%s", priv->conf.apEnd);
    (void)snprintf(apconf.dns, sizeof(apconf.dns), "%s", priv->conf.dns);
    ret = netmgr_ap_start(&apconf);
    if (ret < 0)
    {
        return ret;
    }

    /* 第 4 步：网关留空表示不参与默认路由仲裁 */
    (void)snprintf(link->ifname, sizeof(link->ifname), "%s", priv->conf.ifname);
    (void)snprintf(link->ip, sizeof(link->ip), "%s", priv->conf.apIp);
    (void)snprintf(link->mask, sizeof(link->mask), "%s", priv->conf.apMask);
    (void)snprintf(link->dns, sizeof(link->dns), "%s", priv->conf.dns);
    link->gate[0] = '\0';
    link->priv = priv;
    link->state = NET_M_ST_READY;
    return 0;
}

/**
 * @brief STA 模式拉起：择优连接 → 启用接口 → 配置静态地址或发起 DHCP
 * @param link 链路运行时上下文，不可为空
 * @param priv 私有数据，不可为空
 * @return 0 成功；负值为错误码
 */
static int sta_open(NET_M_LINK_RT* link, NET_M_WIFI_PRIV* priv)
{
    int ret = 0; /* 子步骤返回值 */

    ret = sta_connect(priv);
    if (ret < 0)
    {
        return ret;
    }

    ret = netmgr_iface_enable(priv->conf.ifname, 1);
    if (ret < 0)
    {
        return ret;
    }

    (void)snprintf(link->ifname, sizeof(link->ifname), "%s", priv->conf.ifname);
    (void)snprintf(link->dns, sizeof(link->dns), "%s", priv->conf.dns);
    link->priv = priv;

    if (0 == priv->conf.useDhcp)
    {
        ret = netmgr_iface_setip(priv->conf.ifname, priv->conf.ip);
        if (ret < 0)
        {
            return ret;
        }
        ret = netmgr_iface_setmask(priv->conf.ifname, priv->conf.mask);
        if (ret < 0)
        {
            return ret;
        }
        (void)snprintf(link->gate, sizeof(link->gate), "%s", priv->conf.gate);
        link->state = NET_M_ST_READY;
        return 0;
    }

    return netmgr_dhcp_start(priv->conf.ifname, net_m_dhcp_notify, priv);
}

/**
 * @brief 补齐可选配置的默认值
 * @param conf 配置副本，不可为空
 */
static void conf_default(NET_M_WIFI_CONF* conf)
{
    if ('\0' == conf->ifname[0])
    {
        (void)snprintf(conf->ifname, sizeof(conf->ifname), "%s", NET_M_DEV_WIFI);
    }
    if ('\0' == conf->apIp[0])
    {
        (void)snprintf(conf->apIp, sizeof(conf->apIp), "%s", NET_M_AP_IP);
    }
    if ('\0' == conf->apMask[0])
    {
        (void)snprintf(conf->apMask, sizeof(conf->apMask), "%s", NET_M_AP_MASK);
    }
    if ('\0' == conf->apStart[0])
    {
        (void)snprintf(conf->apStart, sizeof(conf->apStart), "%s", NET_M_AP_START);
    }
    if ('\0' == conf->apEnd[0])
    {
        (void)snprintf(conf->apEnd, sizeof(conf->apEnd), "%s", NET_M_AP_END);
    }
}

/**
 * @brief 配置合法性校验（STA 候选 / AP 必填项 / 静态地址三要素）
 * @param conf 配置，不可为空
 * @return 0 合法；负值为错误码
 */
static int conf_check(const NET_M_WIFI_CONF* conf)
{
    int i = 0; /* 候选下标，从 0 开始 */

    if (NET_M_MODE_AP == conf->mode)
    {
        if ('\0' == conf->apSsid[0])
        {
            return -NETMGRERR_CONF;
        }
        if ('\0' != conf->apPasswd[0] && (int)strlen(conf->apPasswd) < NET_M_PSK_MIN)
        {
            return -NETMGRERR_CONF;
        }
        return 0;
    }

    /* STA：至少 1 个候选，且逐个校验 */
    if (conf->candNum <= 0 || conf->candNum > NET_M_CAND_MAX)
    {
        return -NETMGRERR_CONF;
    }
    for (i = 0; i < conf->candNum; ++i)
    {
        if (0 != cand_check(&conf->cand[i]))
        {
            return -NETMGRERR_CONF;
        }
    }

    /* 静态地址模式：三要素缺一不可 */
    if (0 == conf->useDhcp)
    {
        if (!netmgr_ip_valid(conf->ip) || !netmgr_ip_valid(conf->mask) ||
            !netmgr_ip_valid(conf->gate))
        {
            return -NETMGRERR_CONF;
        }
    }

    /* 候选 SSID 尺寸兜底：防超长配置被 strncpy 风格截断成另一个热点 */
    for (i = 0; i < conf->candNum; ++i)
    {
        if (strlen(conf->cand[i].ssid) >= sizeof(conf->cand[i].ssid))
        {
            return -NETMGRERR_CONF;
        }
    }
    return 0;
}

static int wifi_open(NET_M_LINK_RT* link, const void* conf)
{
    const NET_M_WIFI_CONF* cfg = (const NET_M_WIFI_CONF*)conf; /* 入参配置视图 */
    NET_M_WIFI_PRIV* priv = NULL;                              /* 私有数据 */
    int ret = 0;                                               /* 子步骤返回值 */

    if (NULL == link || NULL == cfg)
    {
        return -NETMGRERR_PARAM;
    }

    ret = conf_check(cfg);
    if (ret < 0)
    {
        return ret;
    }

    priv = (NET_M_WIFI_PRIV*)calloc(1, sizeof(NET_M_WIFI_PRIV));
    if (NULL == priv)
    {
        return -NETMGRERR_MEM;
    }
    memcpy(&priv->conf, cfg, sizeof(priv->conf));
    priv->head.type = NET_M_WIFI;
    priv->activeCand = -1;
    priv->lastOk = -1;
    conf_default(&priv->conf);

    if (!netmgr_iface_exist(priv->conf.ifname))
    {
        free(priv);
        return -NETMGRERR_NOTFOUND;
    }
    if (!netmgr_ip_valid(priv->conf.apIp) || !netmgr_ip_valid(priv->conf.apMask))
    {
        free(priv);
        return -NETMGRERR_CONF;
    }

    /* 按模式分派；失败时回收本层资源，避免半成品上下文 */
    if (NET_M_MODE_AP == priv->conf.mode)
    {
        ret = ap_start(priv, link);
    }
    else
    {
        ret = sta_open(link, priv);
    }

    if (ret < 0)
    {
        /* 清理过程中的次生失败不覆盖主错误码 */
        (void)netmgr_dhcp_stop(priv->conf.ifname);
        (void)netmgr_wpa_stop();
        (void)netmgr_ap_stop();
        free(priv);
        link->priv = NULL;
        link->ifname[0] = '\0';
        link->state = NET_M_ST_DOWN;
        return ret;
    }
    return 0;
}

static int wifi_close(NET_M_LINK_RT* link)
{
    int err = 0; /* 首个错误码 */
    int ret = 0; /* 子步骤返回值 */

    if (NULL == link)
    {
        return -NETMGRERR_PARAM;
    }

    /* 停止取址失败不阻断资源释放，错误码在末尾上抛 */
    if ('\0' != link->ifname[0])
    {
        ret = netmgr_dhcp_stop(link->ifname);
        if (ret < 0)
        {
            err = ret;
        }
    }

    /* STA 与 AP 两侧进程都回收：配置可能在两者间切换过 */
    (void)netmgr_wpa_stop();
    (void)netmgr_ap_stop();

    /* 关闭接口管理态：与 ip link set <ifname> down 等效，须在进程回收之后 */
    if ('\0' != link->ifname[0])
    {
        ret = netmgr_iface_enable(link->ifname, 0);
        if (ret < 0 && 0 == err)
        {
            err = ret;
        }
    }

    free(link->priv);
    link->priv = NULL;

    link->ifname[0] = '\0';
    link->ip[0] = '\0';
    link->mask[0] = '\0';
    link->gate[0] = '\0';
    link->dns[0] = '\0';
    memset(&link->wifi, 0, sizeof(link->wifi));
    link->state = NET_M_ST_DOWN;
    link->route_on = 0;
    link->err = 0;
    link->dhcpSeen = 0;
    link->dhcpErr = 0;
    return err;
}

/**
 * @brief STA 模式刷新：先投影关联信息，再判定就绪
 * @param link 链路运行时上下文，不可为空
 * @param priv 私有数据，不可为空
 * @return 0 成功；负值为错误码
 */
static int sta_refresh(NET_M_LINK_RT* link, NET_M_WIFI_PRIV* priv)
{
    NETMGR_WPA_STATE wstate = NETMGR_WPA_UNKNOWN; /* 当前 wpa 工作状态 */
    int ret = 0;                                  /* 子步骤返回值 */

    if (0 == netmgr_wpa_state(&wstate) && NETMGR_WPA_COMPLETED == wstate)
    {
        (void)netmgr_wpa_status(&link->wifi);
        link->wifi.connected = ('\0' != link->wifi.ssid[0]) ? 1 : 0;
    }
    else
    {
        memset(&link->wifi, 0, sizeof(link->wifi));
    }

    if (0 == priv->conf.useDhcp)
    {
        link->err = 0;
        return net_m_link_ready_static(link, priv->conf.gate);
    }

    ret = net_m_link_ready(link);
    if (ret < 0)
    {
        return ret;
    }
    net_m_link_attr_dhcp(link);
    return 0;
}

/**
 * @brief AP 模式刷新：进程存活 + 地址在位即视为就绪
 * @param link 链路运行时上下文，不可为空
 * @param priv 私有数据，不可为空
 * @return 0 成功；负值为错误码
 */
static int ap_refresh(NET_M_LINK_RT* link, NET_M_WIFI_PRIV* priv)
{
    int ret = 0; /* 子步骤返回值 */

    link->ip[0] = '\0';
    link->mask[0] = '\0';
    link->gate[0] = '\0';
    link->err = 0;
    memset(&link->wifi, 0, sizeof(link->wifi));

    if (!netmgr_ap_running() || !netmgr_iface_ready(priv->conf.ifname))
    {
        link->state = NET_M_ST_DOWN;
        return 0;
    }

    ret = netmgr_iface_ip(priv->conf.ifname, link->ip, (int)sizeof(link->ip));
    if (ret < 0)
    {
        return ret;
    }
    ret = netmgr_iface_mask(priv->conf.ifname, link->mask, (int)sizeof(link->mask));
    if (ret < 0)
    {
        return ret;
    }
    link->state = ('\0' != link->ip[0]) ? NET_M_ST_READY : NET_M_ST_DOWN;
    return 0;
}

static int wifi_refresh(NET_M_LINK_RT* link)
{
    NET_M_WIFI_PRIV* priv = NULL; /* 私有数据 */

    if (NULL == link || NULL == link->priv)
    {
        return -NETMGRERR_PARAM;
    }
    priv = (NET_M_WIFI_PRIV*)link->priv;

    /* 接口已消失（USB 网卡拔出、驱动卸载）：直接判 DOWN 并给出原因 */
    if (!netmgr_iface_exist(link->ifname))
    {
        link->state = NET_M_ST_DOWN;
        link->err = -NETMGRERR_NOTFOUND;
        return 0;
    }

    if (NET_M_MODE_AP == priv->conf.mode)
    {
        return ap_refresh(link, priv);
    }
    return sta_refresh(link, priv);
}

/**
 * @brief 自愈：STA 断开后重新择优连接；AP 进程退出后重启服务
 * @details 由管理线程按退避策略调用，refresh 保持纯投影语义
 * @param link 链路运行时上下文，不可为空
 * @return 0 自愈动作完成；负值为错误码（触发退避）
 */
static int wifi_heal(NET_M_LINK_RT* link)
{
    NET_M_WIFI_PRIV* priv = NULL; /* 私有数据 */
    int ret = 0;                  /* 子步骤返回值 */

    if (NULL == link || NULL == link->priv)
    {
        return -NETMGRERR_PARAM;
    }
    priv = (NET_M_WIFI_PRIV*)link->priv;

    if (NET_M_MODE_AP == priv->conf.mode)
    {
        /* AP：进程不在则按原配置重启（ap 模块幂等重建） */
        if (0 != netmgr_ap_running())
        {
            return 0;
        }
        return ap_start(priv, link);
    }

    /* STA：重新择优连接（sta_connect 内部按需重建控制通道） */
    ret = sta_connect(priv);
    if (ret < 0)
    {
        return ret;
    }

    /* 连接成功后按取址方式重新取址 */
    if (0 == priv->conf.useDhcp)
    {
        (void)netmgr_iface_setip(priv->conf.ifname, priv->conf.ip);
        (void)netmgr_iface_setmask(priv->conf.ifname, priv->conf.mask);
        (void)snprintf(link->gate, sizeof(link->gate), "%s", priv->conf.gate);
        return 0;
    }
    return netmgr_dhcp_start(priv->conf.ifname, net_m_dhcp_notify, priv);
}
