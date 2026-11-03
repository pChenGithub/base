/**
 * @file net_mgr_cell.c
 * @brief netmgr 链路实现层：4G 蜂窝链路实现（ECM 网卡 + DHCP）
 * @details 模组重新注网后 DHCP 进程可能已退出，刷新时按需重新拉起。
 */
#include "net_mgr_core_int.h"

#include "net_mgr_dhcp.h"
#include "net_mgr_err.h"
#include "net_mgr_iface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** 4G 链路私有数据 */
typedef struct {
    NETMGR_DHCP_BASE dhcp; /**< DHCP 公共头（链路类型 + 取址状态），须为首成员供公共回调使用 */
    NETMGR_CELL_CONF conf; /**< 配置副本，生命周期与链路上下文一致 */
} NETMGR_CELL_PRIV;

static int cell_open(NETMGR_CTX* ctx, const void* conf);
static int cell_close(NETMGR_CTX* ctx);
static int cell_refresh(NETMGR_CTX* ctx);

/** 4G 链路操作表 */
const NETMGR_OPS core_cell_ops = {cell_open, cell_close, cell_refresh};

/**
 * @brief 拉起 4G 链路：探测网卡、启用接口并发起 DHCP 取址
 * @param ctx  链路上下文，不可为空
 * @param conf 4G 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int cell_open(NETMGR_CTX* ctx, const void* conf)
{
    const NETMGR_CELL_CONF* cfg = (const NETMGR_CELL_CONF*)conf; /* 入参配置视图 */
    const char* const devs[] = {"usb0", "wwan0", "eth1"};        /* 4G 网卡候选接口名，ECM/RNDIS/NCM 常见命名 */
    NETMGR_CELL_PRIV* priv = NULL;                               /* 私有数据 */
    int ret = 0;                                                 /* 子步骤返回值 */
    int dev_num = (int)(sizeof(devs) / sizeof(devs[0]));         /* 候选接口数量 */

    if (NULL == ctx || NULL == cfg)
    {
        return -NETMGRERR_PARAM;
    }

    /* 第 1 步：先校验接入方式，未实现的方式在分配资源前拒绝 */
    if (NETMGR_CELL_ECM != cfg->mode)
    {
        return -NETMGRERR_CONF;
    }

    priv = (NETMGR_CELL_PRIV*)calloc(1, sizeof(NETMGR_CELL_PRIV));
    if (NULL == priv)
    {
        return -NETMGRERR_MEM;
    }
    memcpy(&priv->conf, cfg, sizeof(priv->conf));
    priv->dhcp.type = NETMGR_CELL; /* 公共回调经它上报事件 */

    /* 第 2 步：确定接口名，未指定时按候选列表自动探测 */
    if ('\0' == priv->conf.ifname[0])
    {
        ret = netmgr_iface_find(devs, dev_num, priv->conf.ifname,
                                (int)sizeof(priv->conf.ifname));
        if (ret < 0)
        {
            free(priv);
            return ret;
        }
    }
    if (!netmgr_iface_exist(priv->conf.ifname))
    {
        free(priv);
        return -NETMGRERR_NOTFOUND;
    }

    /* 第 3 步：启用接口并发起 DHCP，地址异步获取 */
    ret = netmgr_iface_enable(priv->conf.ifname, 1);
    if (ret < 0)
    {
        free(priv);
        return ret;
    }
    ret = netmgr_dhcp_start(priv->conf.ifname, 0, core_dhcp_cb, priv);
    if (ret < 0)
    {
        free(priv);
        return ret;
    }

    /* 第 4 步：全部步骤成功后才挂载上下文 */
    snprintf(ctx->ifname, sizeof(ctx->ifname), "%s", priv->conf.ifname);
    snprintf(ctx->dns, sizeof(ctx->dns), "%s", priv->conf.dns);
    ctx->priv = priv;
    return 0;
}

/**
 * @brief 关闭 4G 链路并释放私有数据
 * @param ctx 链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int cell_close(NETMGR_CTX* ctx)
{
    int ret = 0; /* 停止 DHCP 的返回值，负值为错误码 */

    if (NULL == ctx)
    {
        return -NETMGRERR_PARAM;
    }

    /* 停止取址失败不阻断资源释放，错误码在末尾上抛 */
    if ('\0' != ctx->ifname[0])
        ret = netmgr_dhcp_stop(ctx->ifname);

    free(ctx->priv);
    ctx->priv = NULL;
    ctx->state = NETMGR_ST_DOWN;
    ctx->route_on = 0;
    ctx->err = 0;
    return ret;
}

/**
 * @brief 刷新 4G 链路状态
 * @param ctx 链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int cell_refresh(NETMGR_CTX* ctx)
{
    NETMGR_CELL_PRIV* priv = NULL; /* 私有数据 */
    int ret = 0;                   /* 子步骤返回值 */

    if (NULL == ctx || NULL == ctx->priv)
    {
        return -NETMGRERR_PARAM;
    }
    priv = (NETMGR_CELL_PRIV*)ctx->priv;

    ret = core_link_ready(ctx);
    if (ret < 0)
        return ret;

    /* 未就绪时带上 DHCP 侧原因，供事件与状态快照上报 */
    ctx->err = (NETMGR_ST_DOWN == ctx->state &&
                NETMGR_DHCP_FAIL == priv->dhcp.dhcpState) ?
               (int)priv->dhcp.dhcpErr : 0;

    /* 模组重新注网后取址线程可能已退出，按需重新拉起 */
    if (NETMGR_ST_DOWN == ctx->state && !netmgr_dhcp_busy(ctx->ifname))
    {
        ret = netmgr_dhcp_start(ctx->ifname, 0, core_dhcp_cb, priv);
        if (ret < 0)
            return ret;
    }
    return 0;
}
