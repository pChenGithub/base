/**
 * @file net_mgr_eth.c
 * @brief netmgr 链路实现层：以太网链路实现（静态地址或 DHCP 取址）
 * @details DHCP 取址走通用就绪判定并支持自愈重拉；静态地址只判定连通与地址在位。
 */
#include "net_mgr_core_int.h"

#include "net_mgr_dhcp.h"
#include "net_mgr_err.h"
#include "net_mgr_iface.h"
#include "net_mgr_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** 默认以太网接口名 */
#define NETMGR_DEV_ETH      "eth0"

/** 以太网链路私有数据 */
typedef struct {
    NETMGR_DHCP_BASE dhcp; /**< DHCP 公共头（链路类型 + 取址状态），须为首成员供公共回调使用 */
    NETMGR_ETH_CONF conf;  /**< 配置副本，生命周期与链路上下文一致 */
} NETMGR_ETH_PRIV;

static int eth_open(NETMGR_CTX* ctx, const void* conf);
static int eth_close(NETMGR_CTX* ctx);
static int eth_refresh(NETMGR_CTX* ctx);

/** 以太网链路操作表 */
const NETMGR_OPS core_eth_ops = {eth_open, eth_close, eth_refresh};

/**
 * @brief 拉起以太网链路：启用接口并按配置取址
 * @param ctx  链路上下文，不可为空
 * @param conf 以太网配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int eth_open(NETMGR_CTX* ctx, const void* conf)
{
    const NETMGR_ETH_CONF* cfg = (const NETMGR_ETH_CONF*)conf; /* 入参配置视图 */
    NETMGR_ETH_PRIV* priv = NULL;                              /* 私有数据 */
    int ret = 0;                                               /* 子步骤返回值 */

    if (NULL == ctx || NULL == cfg)
    {
        return -NETMGRERR_PARAM;
    }

    priv = (NETMGR_ETH_PRIV*)calloc(1, sizeof(NETMGR_ETH_PRIV));
    if (NULL == priv)
    {
        return -NETMGRERR_MEM;
    }
    memcpy(&priv->conf, cfg, sizeof(priv->conf));
    priv->dhcp.type = NETMGR_ETH; /* 公共回调经它上报事件 */

    /* 第 1 步：确定接口名，未指定时使用默认 eth0 */
    if ('\0' == priv->conf.ifname[0])
        snprintf(priv->conf.ifname, sizeof(priv->conf.ifname), "%s", NETMGR_DEV_ETH);
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

    /* 第 3 步：按取址方式配置，DHCP 为异步，静态配置在此同步完成 */
    if (priv->conf.use_dhcp)
    {
        ret = netmgr_dhcp_start(priv->conf.ifname, 0, core_dhcp_cb, priv);
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

    /* 第 4 步：全部步骤成功后才挂载上下文，失败路径不留下半成品 */
    snprintf(ctx->ifname, sizeof(ctx->ifname), "%s", priv->conf.ifname);
    snprintf(ctx->dns, sizeof(ctx->dns), "%s", priv->conf.dns);
    ctx->priv = priv;
    return 0;
}

/**
 * @brief 关闭以太网链路并释放私有数据
 * @param ctx 链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int eth_close(NETMGR_CTX* ctx)
{
    int ret = 0; /* 停止 DHCP 的返回值，负值为错误码 */

    if (NULL == ctx)
    {
        return -NETMGRERR_PARAM;
    }

    /* 停止取址失败不阻断资源释放，错误码在末尾上抛 */
    if ('\0' != ctx->ifname[0])
        ret = netmgr_dhcp_stop(ctx->ifname);

    /* 私有数据在本层释放，上下文本身由 core 层统一管理 */
    free(ctx->priv);
    ctx->priv = NULL;
    ctx->state = NETMGR_ST_DOWN;
    ctx->route_on = 0;
    ctx->err = 0;
    return ret;
}

/**
 * @brief 刷新以太网链路状态
 * @param ctx 链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int eth_refresh(NETMGR_CTX* ctx)
{
    NETMGR_ETH_PRIV* priv = NULL; /* 私有数据 */
    int ret = 0;                  /* 子步骤返回值 */

    if (NULL == ctx || NULL == ctx->priv)
    {
        return -NETMGRERR_PARAM;
    }
    priv = (NETMGR_ETH_PRIV*)ctx->priv;

    /* DHCP 取址：连通性、地址、网关完全交给通用判定 */
    if (priv->conf.use_dhcp)
    {
        ret = core_link_ready(ctx);
        if (ret < 0)
            return ret;
        /* 未就绪时带上 DHCP 侧原因，供事件与状态快照上报 */
        ctx->err = (NETMGR_ST_DOWN == ctx->state &&
                    NETMGR_DHCP_FAIL == priv->dhcp.dhcpState) ?
                   (int)priv->dhcp.dhcpErr : 0;
        /* 取址线程已结束且仍无地址时重新拉起，实现链路自愈 */
        if (NETMGR_ST_DOWN == ctx->state && !netmgr_dhcp_busy(ctx->ifname))
        {
            ret = netmgr_dhcp_start(ctx->ifname, 0, core_dhcp_cb, priv);
            if (ret < 0)
                return ret;
        }
        return 0;
    }

    /* 静态地址：只判定连通性与地址是否在位，网关取配置值 */
    ret = core_link_ready_static(ctx, priv->conf.gate);
    ctx->err = 0;
    return ret;
}
