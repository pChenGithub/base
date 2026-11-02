/**
 * @file net_mgr_cell.c
 * @brief 4G 链路实现：当前支持 ECM/RNDIS/NCM 网卡形态（驱动已生成网卡，走 DHCP）
 * @details PPP 与 QMI 属于拨号形态，需额外进程与 AT/控制通道配合，
 *          本次仅在操作表上预留，未实现时返回明确的配置错误。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_mgr_def.h"

/** 4G 网卡候选接口名，覆盖 ECM/RNDIS/NCM 驱动生成的常见命名 */
static const char* const g_cell_devs[] = {"usb0", "wwan0", "eth1"};

/** 4G 链路私有数据 */
typedef struct {
    NETMGR_CELL_CONF conf; /**< 配置副本，生命周期与链路上下文一致 */
} NETMGR_CELL_PRIV;

static int cell_open(NETMGR_CTX* ctx, const void* conf);
static int cell_close(NETMGR_CTX* ctx);
static int cell_refresh(NETMGR_CTX* ctx);

/** 4G 链路操作表 */
const NETMGR_OPS netmgr_cell_ops = {cell_open, cell_close, cell_refresh};

/**
 * @brief 拉起 4G 链路：探测网卡、启用接口并发起 DHCP 取址
 * @param ctx  链路上下文，不可为空
 * @param conf 4G 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int cell_open(NETMGR_CTX* ctx, const void* conf)
{
    const NETMGR_CELL_CONF* cfg = (const NETMGR_CELL_CONF*)conf; /* 入参配置视图 */
    NETMGR_CELL_PRIV* priv = NULL;                                /* 私有数据 */
    int ret = 0;                                                  /* 子步骤返回值 */
    int dev_num = (int)(sizeof(g_cell_devs) / sizeof(g_cell_devs[0])); /* 候选接口数量 */

    if (NULL == ctx || NULL == cfg)
    {
        NETMGR_ERR("入参非法");
        return -NETMGRERR_PARAM;
    }

    /* 第 1 步：先校验接入方式，未实现的方式在分配资源前拒绝 */
    if (NETMGR_CELL_ECM != cfg->mode)
    {
        NETMGR_ERR("暂不支持的 4G 接入方式, mode=%d", (int)cfg->mode);
        return -NETMGRERR_CONF;
    }

    priv = (NETMGR_CELL_PRIV*)calloc(1, sizeof(NETMGR_CELL_PRIV));
    if (NULL == priv)
    {
        NETMGR_ERR("申请 4G 链路上下文失败");
        return -NETMGRERR_MEM;
    }
    memcpy(&priv->conf, cfg, sizeof(priv->conf));

    /* 第 2 步：确定接口名，未指定时按候选列表自动探测 */
    if ('\0' == priv->conf.ifname[0])
    {
        ret = netmgr_dev_find(g_cell_devs, dev_num, priv->conf.ifname,
                              (int)sizeof(priv->conf.ifname));
        if (ret < 0)
        {
            NETMGR_ERR("未探测到 4G 网卡, 候选数量=%d, ret=%d", dev_num, ret);
            free(priv);
            return ret;
        }
    }
    if (!netmgr_link_exist(priv->conf.ifname))
    {
        NETMGR_ERR("接口 %s 不存在", priv->conf.ifname);
        free(priv);
        return -NETMGRERR_NOTFOUND;
    }

    /* 第 3 步：启用接口并发起 DHCP，地址异步获取 */
    ret = netmgr_link_enable(priv->conf.ifname, 1);
    if (ret < 0)
    {
        NETMGR_ERR("启用 %s 失败, ret=%d", priv->conf.ifname, ret);
        free(priv);
        return ret;
    }
    ret = netmgr_dhcp_start(priv->conf.ifname, 0);
    if (ret < 0)
    {
        NETMGR_ERR("%s 启动 DHCP 失败, ret=%d", priv->conf.ifname, ret);
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
    int ret = 0; /* 停止 DHCP 的返回值 */

    if (NULL == ctx)
    {
        NETMGR_ERR("入参 ctx 为空");
        return -NETMGRERR_PARAM;
    }

    if ('\0' != ctx->ifname[0])
    {
        ret = netmgr_dhcp_stop(ctx->ifname);
        if (ret < 0)
            NETMGR_ERR("停止 %s 的 DHCP 失败, ret=%d", ctx->ifname, ret);
    }

    free(ctx->priv);
    ctx->priv = NULL;
    ctx->state = NETMGR_ST_DOWN;
    ctx->route_on = 0;
    return 0;
}

/**
 * @brief 刷新 4G 链路状态
 * @param ctx 链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int cell_refresh(NETMGR_CTX* ctx)
{
    int ret = 0; /* 子步骤返回值 */

    if (NULL == ctx || NULL == ctx->priv)
    {
        NETMGR_ERR("链路上下文未初始化");
        return -NETMGRERR_PARAM;
    }

    ret = netmgr_link_ready(ctx);
    if (ret < 0)
        return ret;

    /* 模组重新注网后 DHCP 进程可能已退出，按需重新拉起 */
    if (NETMGR_ST_DOWN == ctx->state && !netmgr_dhcp_busy(ctx->ifname))
    {
        ret = netmgr_dhcp_start(ctx->ifname, 0);
        if (ret < 0)
            NETMGR_ERR("%s 重新拉起 DHCP 失败, ret=%d", ctx->ifname, ret);
    }
    return 0;
}
