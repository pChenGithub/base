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
 * @brief 就绪判定的公共前三步：物理连通 → 已取地址 → 掩码
 * @details 两类就绪判定（动态网关 / 静态网关）只在最后一步取网关处不同，
 *          前三步完全一致，抽出来避免两处维护同一套判定口径
 * @param ctx 链路上下文，不可为空
 * @return 0 继续后续判定；1 已判定为 DOWN（本函数已置 state）；负值为 NETMGRERR_* 错误码
 */
static int link_probe_iface(NETMGR_CTX* ctx)
{
    int ret = 0; /* 子步骤返回值 */

    ctx->ip[0] = '\0';
    ctx->mask[0] = '\0';
    ctx->gate[0] = '\0';

    /* 第 1 步：物理连通性，未连通直接判为不可用 */
    if (!netmgr_iface_ready(ctx->ifname))
    {
        ctx->state = NETMGR_ST_DOWN;
        return 1;
    }

    /* 第 2 步：地址是否已取得 */
    ret = netmgr_iface_ip(ctx->ifname, ctx->ip, (int)sizeof(ctx->ip));
    if (ret < 0)
        return ret;
    if ('\0' == ctx->ip[0])
    {
        ctx->state = NETMGR_ST_DOWN;
        return 1;
    }

    /* 第 3 步：掩码，与地址同源，取接口实际生效值 */
    ret = netmgr_iface_mask(ctx->ifname, ctx->mask, (int)sizeof(ctx->mask));
    if (ret < 0)
        return ret;
    return 0;
}

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

    /* 第 1~3 步：物理连通、地址、掩码（公共前置） */
    ret = link_probe_iface(ctx);
    if (ret < 0)
        return ret;
    if (1 == ret)
        return 0;

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

    /* 第 1~3 步：物理连通、地址、掩码（公共前置） */
    ret = link_probe_iface(ctx);
    if (ret < 0)
        return ret;
    if (1 == ret)
        return 0;

    /* 第 4 步：静态链路无 DHCP 写入的网关，直接取配置值 */
    snprintf(ctx->gate, (size_t)sizeof(ctx->gate), "%s", gate);
    ctx->state = ('\0' != ctx->gate[0]) ? NETMGR_ST_READY : NETMGR_ST_DOWN;
    return 0;
}

int core_link_ready_dhcp(NETMGR_CTX* ctx, NETMGR_DHCP_BASE* base)
{
    int ret = 0; /* 子步骤返回值 */

    if (NULL == ctx || NULL == base)
        return -NETMGRERR_PARAM;

    /* 接口已消失（USB 网卡拔出、驱动卸载、模组掉线）：直接判 DOWN 并给出原因，
       不再做地址/路由查询，避免连续 ioctl 失败且 err 语义不清 */
    if (!netmgr_iface_exist(ctx->ifname))
    {
        ctx->state = NETMGR_ST_DOWN;
        ctx->err = -NETMGRERR_NOTFOUND;
        return 0;
    }

    ret = core_link_ready(ctx);
    if (ret < 0)
        return ret;

    /* 未就绪原因：取址失败给出具体错误码；已持有租约却掉线（carrier 断开、
       网关消失）置 -NETMGRERR_DHCP，供业务从 DOWN 事件区分“网络中断”与“从未取到地址” */
    if (NETMGR_ST_DOWN == ctx->state && NETMGR_DHCP_FAIL == base->dhcpState)
        ctx->err = (int)base->dhcpErr;
    else if (NETMGR_ST_DOWN == ctx->state && NETMGR_DHCP_SUCC == base->dhcpState)
        ctx->err = -NETMGRERR_DHCP;
    else
        ctx->err = 0;
    return 0;
}
