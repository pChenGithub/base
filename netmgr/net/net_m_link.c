/**
 * @file net_m_link.c
 * @brief net 链路就绪判定公共实现（net 层）
 * @details 「接口状态 + 地址 + 网关」的组合判定集中在此，三条链路实现共享，
 *          避免以太网与 4G 各写一份导致口径漂移。
 */
#include "net_m_ctx.h"

#include "net_mgr_err.h"
#include "net_mgr_iface.h"
#include "net_mgr_route.h"

#include <stdio.h>

/**
 * @brief 就绪判定公共前三步：物理连通 → 已取地址 → 掩码
 * @details 静态与动态两类判定只在最后一步取网关处不同
 * @param link 链路运行时上下文，不可为空
 * @return 0 继续后续判定；1 已判定为 DOWN；负值为错误码
 */
static int link_probe(NET_M_LINK_RT* link)
{
    int ret = 0; /* 子步骤返回值 */

    link->ip[0] = '\0';
    link->mask[0] = '\0';
    link->gate[0] = '\0';

    /* 第 1 步：管理态与载波同时就绪才算物理连通 */
    if (!netmgr_iface_ready(link->ifname))
    {
        link->state = NET_M_ST_DOWN;
        return 1;
    }

    /* 第 2 步：地址是否已取得 */
    ret = netmgr_iface_ip(link->ifname, link->ip, (int)sizeof(link->ip));
    if (ret < 0)
    {
        return ret;
    }
    if ('\0' == link->ip[0])
    {
        link->state = NET_M_ST_DOWN;
        return 1;
    }

    /* 第 3 步：掩码与地址同源，取接口实际生效值 */
    ret = netmgr_iface_mask(link->ifname, link->mask, (int)sizeof(link->mask));
    if (ret < 0)
    {
        return ret;
    }
    return 0;
}

int net_m_link_ready(NET_M_LINK_RT* link)
{
    NETMGR_ROUTE route = {0}; /* 该接口上的默认路由 */
    int ret = 0;              /* 子步骤返回值 */

    if (NULL == link)
    {
        return -NETMGRERR_PARAM;
    }

    ret = link_probe(link);
    if (ret < 0)
    {
        return ret;
    }
    if (1 == ret)
    {
        return 0;
    }

    /* 第 4 步：网关由 DHCP 客户端写入路由表，此处回查；查不到即视为未就绪 */
    ret = netmgr_route_dev(link->ifname, &route);
    if (ret < 0)
    {
        link->state = NET_M_ST_DOWN;
        return 0;
    }

    (void)snprintf(link->gate, sizeof(link->gate), "%s", route.gate);
    link->state = NET_M_ST_READY;
    return 0;
}

int net_m_link_ready_static(NET_M_LINK_RT* link, const char* gate)
{
    int ret = 0; /* 子步骤返回值 */

    if (NULL == link || NULL == gate)
    {
        return -NETMGRERR_PARAM;
    }

    ret = link_probe(link);
    if (ret < 0)
    {
        return ret;
    }
    if (1 == ret)
    {
        return 0;
    }

    /* 静态链路的网关由配置给出；为空表示该链路不参与默认路由，判为 DOWN */
    (void)snprintf(link->gate, sizeof(link->gate), "%s", gate);
    link->state = ('\0' != link->gate[0]) ? NET_M_ST_READY : NET_M_ST_DOWN;
    return 0;
}

/**
 * @brief 接口消失判定：统一给出 -NETMGRERR_NOTFOUND 并置 DOWN
 * @param link 链路运行时上下文，不可为空
 * @return 0 接口存在；1 接口已消失（已置 DOWN）；负值为错误码
 */
static int link_check_exist(NET_M_LINK_RT* link)
{
    if (!netmgr_iface_exist(link->ifname))
    {
        link->state = NET_M_ST_DOWN;
        link->err = -NETMGRERR_NOTFOUND;
        return 1;
    }
    return 0;
}

/**
 * @brief DHCP 链路的 DOWN 归因：区分「取址失败」与「曾取到租约后中断」
 * @param link 链路运行时上下文，不可为空
 */
void net_m_link_attr_dhcp(NET_M_LINK_RT* link)
{
    if (NET_M_ST_DOWN != link->state)
    {
        link->err = 0;
        return;
    }

    if (0 != link->dhcpSeen && NETMGR_DHCP_FAIL == link->dhcpState)
    {
        link->err = link->dhcpErr;
    }
    else if (0 != link->dhcpSeen && NETMGR_DHCP_SUCC == link->dhcpState)
    {
        link->err = -NETMGRERR_DHCP;
    }
    else
    {
        link->err = 0;
    }
}

int net_m_link_ready_dhcp(NET_M_LINK_RT* link)
{
    int ret = 0; /* 子步骤返回值 */

    if (NULL == link)
    {
        return -NETMGRERR_PARAM;
    }

    /* 接口已消失（USB 网卡拔出、驱动卸载）：直接判 DOWN 并给出原因 */
    ret = link_check_exist(link);
    if (ret != 0)
    {
        return (ret < 0) ? ret : 0;
    }

    ret = net_m_link_ready(link);
    if (ret < 0)
    {
        return ret;
    }

    net_m_link_attr_dhcp(link);
    return 0;
}
