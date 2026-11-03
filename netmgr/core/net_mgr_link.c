/**
 * @file net_mgr_link.c
 * @brief netmgr 链路实现层：链路通用就绪判定实现
 * @details 由 linktype 层统一承担「接口 + 路由」的组合判定，
 *          避免 sys 层的接口能力反向依赖路由能力。
 */
#include "net_mgr_core_int.h"

#include "net_mgr_err.h"
#include "net_mgr_iface.h"
#include "net_mgr_route.h"

#include <stdio.h>

/**
 * @brief 按「物理连通 + 已取地址 + 有默认网关」判定链路是否就绪并刷新快照
 * @param ctx 链路上下文，不可为空
 * @return 0 成功（未就绪时状态置为 DOWN）；负值为 NETMGRERR_* 错误码
 */
int core_link_ready(NETMGR_CTX* ctx)
{
    NETMGR_ROUTE route = {0}; /* 该接口上的默认路由 */
    int ret = 0;              /* 子步骤返回值 */

    if (NULL == ctx)
    {
        return -NETMGRERR_PARAM;
    }

    ctx->ip[0] = '\0';
    ctx->mask[0] = '\0';
    ctx->gate[0] = '\0';

    /* 第 1 步：物理连通性，未连通直接判为不可用 */
    if (!netmgr_iface_up(ctx->ifname))
    {
        ctx->state = NETMGR_ST_DOWN;
        return 0;
    }

    /* 第 2 步：地址是否已取得 */
    ret = netmgr_iface_ip(ctx->ifname, ctx->ip, (int)sizeof(ctx->ip));
    if (ret < 0)
        return ret;
    if ('\0' == ctx->ip[0])
    {
        ctx->state = NETMGR_ST_DOWN;
        return 0;
    }

    /* 第 3 步：掩码，与地址同源，取接口实际生效值 */
    ret = netmgr_iface_mask(ctx->ifname, ctx->mask, (int)sizeof(ctx->mask));
    if (ret < 0)
        return ret;

    /* 第 4 步：默认网关，由 DHCP 客户端写入路由表，此处回查 */
    ret = netmgr_route_dev(ctx->ifname, &route);
    if (ret < 0)
    {
        ctx->state = NETMGR_ST_DOWN;
        return 0;
    }

    snprintf(ctx->gate, (size_t)sizeof(ctx->gate), "%s", route.gate);
    ctx->state = NETMGR_ST_READY;
    return 0;
}

/**
 * @brief 按「物理连通 + 已取地址 + 配置网关非空」判定静态地址链路是否就绪
 * @param ctx  链路上下文，不可为空
 * @param gate 配置网关，不可为空（空串表示该链路不参与默认路由，判为 DOWN）
 * @return 0 成功（未就绪时状态置为 DOWN）；负值为 NETMGRERR_* 错误码
 */
int core_link_ready_static(NETMGR_CTX* ctx, const char* gate)
{
    int ret = 0; /* 子步骤返回值 */

    if (NULL == ctx || NULL == gate)
    {
        return -NETMGRERR_PARAM;
    }

    ctx->ip[0] = '\0';
    ctx->mask[0] = '\0';
    ctx->gate[0] = '\0';

    /* 第 1 步：物理连通性，未连通直接判为不可用 */
    if (!netmgr_iface_up(ctx->ifname))
    {
        ctx->state = NETMGR_ST_DOWN;
        return 0;
    }

    /* 第 2 步：地址是否在位 */
    ret = netmgr_iface_ip(ctx->ifname, ctx->ip, (int)sizeof(ctx->ip));
    if (ret < 0)
        return ret;
    if ('\0' == ctx->ip[0])
    {
        ctx->state = NETMGR_ST_DOWN;
        return 0;
    }

    /* 第 3 步：掩码，与地址同源，取接口实际生效值 */
    ret = netmgr_iface_mask(ctx->ifname, ctx->mask, (int)sizeof(ctx->mask));
    if (ret < 0)
        return ret;

    /* 第 4 步：静态链路无 DHCP 写入的网关，直接取配置值 */
    snprintf(ctx->gate, (size_t)sizeof(ctx->gate), "%s", gate);
    ctx->state = ('\0' != ctx->gate[0]) ? NETMGR_ST_READY : NETMGR_ST_DOWN;
    return 0;
}
