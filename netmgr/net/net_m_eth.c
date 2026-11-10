/**
 * @file net_m_eth.c
 * @brief net 以太网链路实现：DHCP 或静态地址（net 层）
 * @details 私有数据以 NET_M_HEAD 为首成员，供 DHCP 回调按固定偏移取链路类型；
 *          失败路径全部释放私有数据，不把半成品挂到链路槽上。
 */
#include "net_m_ctx.h"

#include "net_mgr_dhcp.h"
#include "net_mgr_err.h"
#include "net_mgr_iface.h"
#include "net_mgr_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** 默认以太网接口名 */
#define NET_M_DEV_ETH "eth0"

/** 以太网链路私有数据 */
typedef struct {
    NET_M_HEAD head;       /**< 公共头（链路类型），须为首成员供 DHCP 回调使用 */
    NET_M_ETH_CONF conf;   /**< 配置副本，生命周期与链路上下文一致 */
} NET_M_ETH_PRIV;

static int eth_open(NET_M_LINK_RT* link, const void* conf);
static int eth_close(NET_M_LINK_RT* link);
static int eth_refresh(NET_M_LINK_RT* link);
static int eth_heal(NET_M_LINK_RT* link);

/** 以太网链路操作表：heal 用于拔插后强制重新取址（udhcpc 不感知 carrier） */
const NET_M_OPS net_m_eth_ops = {eth_open, eth_close, eth_refresh, eth_heal};

static int eth_open(NET_M_LINK_RT* link, const void* conf)
{
    const NET_M_ETH_CONF* cfg = (const NET_M_ETH_CONF*)conf; /* 入参配置视图 */
    NET_M_ETH_PRIV* priv = NULL;                             /* 私有数据 */
    int ret = 0;                                             /* 子步骤返回值 */

    if (NULL == link || NULL == cfg)
    {
        return -NETMGRERR_PARAM;
    }

    priv = (NET_M_ETH_PRIV*)calloc(1, sizeof(NET_M_ETH_PRIV));
    if (NULL == priv)
    {
        return -NETMGRERR_MEM;
    }
    memcpy(&priv->conf, cfg, sizeof(priv->conf));
    priv->head.type = NET_M_ETH;

    /* 第 1 步：确定接口名，未指定时使用默认 eth0 */
    if ('\0' == priv->conf.ifname[0])
    {
        (void)snprintf(priv->conf.ifname, sizeof(priv->conf.ifname), "%s", NET_M_DEV_ETH);
    }
    if (!netmgr_iface_exist(priv->conf.ifname))
    {
        free(priv);
        return -NETMGRERR_NOTFOUND;
    }

    /* 第 2 步：启用接口，地址配置前必须处于可操作状态 */
    ret = netmgr_iface_enable(priv->conf.ifname, 1);
    if (ret < 0)
    {
        free(priv);
        return ret;
    }

    /* 第 3 步：按取址方式配置；DHCP 异步取址，静态配置在此同步完成 */
    if (0 != priv->conf.useDhcp)
    {
        ret = netmgr_dhcp_start(priv->conf.ifname, net_m_dhcp_notify, priv);
        if (ret < 0)
        {
            free(priv);
            return ret;
        }
    }
    else
    {
        if (!netmgr_ip_valid(priv->conf.ip) || !netmgr_ip_valid(priv->conf.mask))
        {
            free(priv);
            return -NETMGRERR_CONF;
        }
        ret = netmgr_iface_setip(priv->conf.ifname, priv->conf.ip);
        if (ret < 0)
        {
            free(priv);
            return ret;
        }
        ret = netmgr_iface_setmask(priv->conf.ifname, priv->conf.mask);
        if (ret < 0)
        {
            free(priv);
            return ret;
        }
    }

    /* 第 4 步：全部成功后才挂载上下文，失败路径不留半成品 */
    (void)snprintf(link->ifname, sizeof(link->ifname), "%s", priv->conf.ifname);
    (void)snprintf(link->dns, sizeof(link->dns), "%s", priv->conf.dns);
    link->priv = priv;
    return 0;
}

static int eth_close(NET_M_LINK_RT* link)
{
    int err = 0; /* 首个错误码 */
    int ret = 0; /* 子步骤返回值 */

    if (NULL == link)
    {
        return -NETMGRERR_PARAM;
    }

    if ('\0' != link->ifname[0])
    {
        /* 停止取址失败不阻断资源释放，错误码在末尾上抛 */
        ret = netmgr_dhcp_stop(link->ifname);
        if (ret < 0)
        {
            err = ret;
        }

        /* 关闭接口管理态：与 ip link set <ifname> down 等效（地址保留但不再收发），
           保证「禁用链路」与系统命令效果一致 */
        ret = netmgr_iface_enable(link->ifname, 0);
        if (ret < 0 && 0 == err)
        {
            err = ret;
        }
    }

    free(link->priv);
    link->priv = NULL;

    /* 关闭后清空接口名与地址，保证快照契约「未启用时为空串」成立 */
    link->ifname[0] = '\0';
    link->ip[0] = '\0';
    link->mask[0] = '\0';
    link->gate[0] = '\0';
    link->dns[0] = '\0';
    link->state = NET_M_ST_DOWN;
    link->route_on = 0;
    link->err = 0;
    link->dhcpSeen = 0;
    link->dhcpErr = 0;
    return err;
}

static int eth_refresh(NET_M_LINK_RT* link)
{
    NET_M_ETH_PRIV* priv = NULL; /* 私有数据 */

    if (NULL == link || NULL == link->priv)
    {
        return -NETMGRERR_PARAM;
    }
    priv = (NET_M_ETH_PRIV*)link->priv;

    /* DHCP：连通性、地址、网关与 DOWN 归因交给公共判定 */
    if (0 != priv->conf.useDhcp)
    {
        return net_m_link_ready_dhcp(link);
    }

    /* 静态地址：网关取配置值，不查路由表 */
    link->err = 0;
    return net_m_link_ready_static(link, priv->conf.gate);
}

/**
 * @brief 自愈：载体恢复后按配置重建地址/取址
 * @details udhcpc 不监听 carrier：拔插网线后它不会重新 DISCOVER，租约内也不会重装
 *          默认路由，链路会长期停在 DOWN。本钩子在有载体且曾取址成功时重建一次
 *          （DHCP 重建会话 / 静态重下发地址），把地址、网关与默认路由一起刷新。
 * @param link 链路运行时上下文，不可为空
 * @return 0 无需动作或已重建；负值为错误码（触发退避）
 */
static int eth_heal(NET_M_LINK_RT* link)
{
    NET_M_ETH_PRIV* priv = NULL; /* 私有数据 */

    if (NULL == link || NULL == link->priv)
    {
        return -NETMGRERR_PARAM;
    }
    priv = (NET_M_ETH_PRIV*)link->priv;

    /* 未插线：等载体恢复，不重建 */
    if (!netmgr_iface_ready(priv->conf.ifname))
    {
        return 0;
    }

    /* 首次取址尚未成功：udhcpc 自己仍在重试，不打断它 */
    if (0 != priv->conf.useDhcp && 0 == link->dhcpSeen)
    {
        return 0;
    }

    return net_m_link_restore(link, priv->conf.ifname, priv->conf.useDhcp,
                              priv->conf.ip, priv->conf.mask, priv);
}
