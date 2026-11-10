/**
 * @file net_m_route.c
 * @brief net 默认路由同步：按优先级下发 metric，失败保留状态以便重试（net 层）
 * @details 相对 netmgr 的差异：删除失败不再把 route_on 清 0，避免「内核仍有路由但
 *          模块认为已撤销」的状态失真；路由变更采用先建新再删旧的顺序，
 *          缩小切换瞬间的无路由窗口。
 *          默认路由所有权：本模块独占。启动时由 core 清空既有默认路由；
 *          运行期每条链路在本函数里接管该接口上的非目标条目（如 DHCP 脚本按
 *          metric 0 装的默认路由），保证内核里只有按 prio 下发的那一条。
 */
#include "net_m_ctx.h"

#include "net_mgr_err.h"
#include "net_mgr_route.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief 撤销某链路的默认路由（幂等）
 * @param link 链路运行时上下文，不可为空
 * @return 0 成功或无需撤销；负值为错误码
 */
int net_m_route_revoke(NET_M_LINK_RT* link)
{
    int ret = 0; /* 删除结果 */

    if (NULL == link)
    {
        return -NETMGRERR_PARAM;
    }
    if (0 == link->route_on)
    {
        return 0;
    }

    ret = netmgr_route_del(link->ifname, link->rgate, link->rmetric);
    if (ret < 0)
    {
        /* 保留 route_on 与 rmetric：下一轮继续尝试删除，状态与内核保持一致 */
        return ret;
    }

    link->route_on = 0;
    link->rmetric = -1;
    link->rgate[0] = '\0';
    return 0;
}

/**
 * @brief 按链路当前状态与优先级同步其默认路由
 * @details 参与条件 = 就绪 + 网关非空；metric = prio（小者优）。
 *          变更时先建新再删旧：新路由 metric 更小则立即生效，再撤旧路由，
 *          避免「先删后建」造成的默认路由空窗
 * @param link 链路运行时上下文，不可为空
 * @return 0 成功或无需变更；负值为错误码
 */
int net_m_route_sync(NET_M_LINK_RT* link)
{
    NETMGR_ROUTE cur = {0}; /* 该接口当前最优默认路由 */
    int need = 0;   /* 1=应下发默认路由 */
    int metric = 0; /* 目标 metric */
    int ret = 0;    /* 子步骤返回值 */
    int err = 0;    /* 首个错误码 */

    if (NULL == link)
    {
        return -NETMGRERR_PARAM;
    }

    need = (NET_M_ST_READY == link->state && '\0' != link->gate[0]) ? 1 : 0;
    metric = (int)link->prio;

    if (0 == need)
    {
        return net_m_route_revoke(link);
    }

    /* 接管：该接口当前最优默认路由若非目标形态（DHCP 脚本按 metric 0 装的条目、
       旧网关或旧 metric 的条目），先删掉它再下发本模块条目，
       保证内核里该接口只保留一条由本模块按 prio 管理的默认路由 */
    ret = netmgr_route_dev(link->ifname, &cur);
    if (0 == ret && (cur.metric != metric || 0 != strcmp(cur.gate, link->gate)))
    {
        ret = netmgr_route_del(link->ifname, cur.gate, cur.metric);
        if (ret < 0)
        {
            return ret; /* 旧条目删不掉就先不下发，下一轮重试 */
        }
        link->route_on = 0;
        link->rmetric = -1;
        link->rgate[0] = '\0';
    }

    /* 已下发且网关与 metric 均未变：不触碰内核，避免无谓抖动 */
    if (0 != link->route_on && link->rmetric == metric &&
        0 == strcmp(link->rgate, link->gate))
    {
        return 0;
    }

    /* 先建新路由：metric 更小时内核立即改选，不产生无默认路由的窗口 */
    ret = netmgr_route_add(link->ifname, link->gate, metric);
    if (ret < 0)
    {
        /* 建新失败：保留旧路由不动，调用方据此记故障，下一轮重试 */
        return ret;
    }

    if (0 != link->route_on)
    {
        ret = netmgr_route_del(link->ifname, link->rgate, link->rmetric);
        if (ret < 0)
        {
            err = ret; /* 旧路由残留：记录错误，但新路由已生效 */
        }
    }

    link->route_on = 1;
    link->rmetric = metric;
    (void)snprintf(link->rgate, sizeof(link->rgate), "%s", link->gate);
    return err;
}
