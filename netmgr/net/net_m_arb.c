/**
 * @file net_m_arb.c
 * @brief net 优先级仲裁器：刷新 → 差异 → 路由 → 选举 → 应用（net 层）
 * @details 相对 netmgr 仲裁器的差异（均为缺陷修复，语义见各函数注释）：
 *          1) 刷新失败不再静默跳过：链路降级为 DOWN、撤除路由、上报 NET_M_EV_FAULT；
 *          2) DNS 每轮同步（内部去重），不再依赖「主链路是否变化」；
 *          3) 故障位 fault 每轮重新评估，只反映本轮状态，不做历史累积；
 *          4) 就绪链路的优先级唯一性由配置阶段保证，本层不再处理同 metric 冲突。
 *          本函数不起停进程、不调用业务回调，链路槽由管理线程独占写入。
 */
#include "net_m_ctx.h"

#include "net_mgr_err.h"
#include "net_mgr_dns.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief 上下文转对外快照（不含 ACTIVE 呈现）
 * @param link 链路运行时上下文，不可为空
 * @param out  输出快照，不可为空
 */
static void arb_conv(const NET_M_LINK_RT* link, NET_M_LINK* out)
{
    out->type = link->type;
    out->state = link->state;
    out->prio = link->prio;
    (void)snprintf(out->ifname, sizeof(out->ifname), "%s", link->ifname);
    (void)snprintf(out->ip, sizeof(out->ip), "%s", link->ip);
    (void)snprintf(out->mask, sizeof(out->mask), "%s", link->mask);
    (void)snprintf(out->gate, sizeof(out->gate), "%s", link->gate);
    out->err = link->err;
    out->fault = link->fault;
    out->faultStage = link->faultStage;
    out->wifi = link->wifi;
}

void net_m_arb_snapshot_one(const NET_M_ARB* arb, const NET_M_LINK_RT* links,
                            int index, NET_M_LINK* out)
{
    if (NULL == links || NULL == out || index < 0 || index >= NET_M_TYPE_MAX)
    {
        return;
    }

    memset(out, 0, sizeof(*out));
    arb_conv(&links[index], out);

    /* ACTIVE 是选举结果，不属于客观状态，只在对外快照中叠加 */
    if (NULL != arb && index == arb->active && NET_M_ST_READY == links[index].state)
    {
        out->state = NET_M_ST_ACTIVE;
    }
}

void net_m_arb_snapshot(const NET_M_ARB* arb, const NET_M_LINK_RT* links,
                        NET_M_LINK* list, int* num)
{
    int cap = 0; /* 输出容量 */
    int cnt = 0; /* 已写入条数 */
    int i = 0;   /* 链路下标，从 0 开始 */

    if (NULL == links || NULL == list || NULL == num || *num <= 0)
    {
        return;
    }
    cap = *num;

    for (i = 0; i < NET_M_TYPE_MAX && cnt < cap; ++i)
    {
        net_m_arb_snapshot_one(arb, links, i, &list[cnt]);
        ++cnt;
    }
    *num = cnt;
}

void net_m_arb_ev_push(NET_M_EVENT* evs, int max, int* num, const NET_M_EVENT* ev)
{
    if (NULL == evs || NULL == num || NULL == ev || *num >= max)
    {
        return;
    }

    evs[*num] = *ev;
    ++(*num);
}

/**
 * @brief 构造一条以完整链路快照承载的事件
 * @param ev   待填充事件，不可为空
 * @param type 事件类型
 * @param link 链路快照，不可为空
 */
static void arb_ev_link(NET_M_EVENT* ev, NET_M_EV type, const NET_M_LINK* link)
{
    memset(ev, 0, sizeof(*ev));
    ev->ev = type;
    ev->u.link = *link;
}

/**
 * @brief 构造并追加一条故障事件
 * @param evs   事件数组，不可为空
 * @param max   数组容量
 * @param num   当前条数，不可为空
 * @param type  链路类型
 * @param stage 故障阶段
 * @param code  错误码
 */
static void arb_ev_fault(NET_M_EVENT* evs, int max, int* num, NET_M_TYPE type,
                         NET_M_STAGE stage, int code)
{
    NET_M_EVENT item = {0}; /* 待追加事件 */

    item.ev = NET_M_EV_FAULT;
    item.u.fault.type = type;
    item.u.fault.stage = stage;
    item.u.fault.code = code;
    net_m_arb_ev_push(evs, max, num, &item);
}

/**
 * @brief 判定状态翻转对应的事件类型
 * @details DISABLED → 其它：ENABLED；非 READY → READY：UP；其它 → 非 READY：DOWN
 * @param prev 上一轮状态
 * @param now  本轮状态
 * @return 事件类型
 */
static NET_M_EV arb_flip_type(NET_M_STATE prev, NET_M_STATE now)
{
    if (NET_M_ST_DISABLED == prev)
    {
        return NET_M_EV_ENABLED;
    }

    return (NET_M_ST_READY == now) ? NET_M_EV_UP : NET_M_EV_DOWN;
}

/**
 * @brief 状态未翻转时按字段比对，产出 INFO_CHANGE 事件
 * @details 只在 READY 态比对：DOWN 态地址字段本就是空值
 * @param evs    事件数组，不可为空
 * @param max    数组容量
 * @param num    当前条数，不可为空
 * @param link   链路运行时上下文，不可为空
 * @param before 刷新前快照，不可为空
 * @param after  刷新后快照，不可为空
 */
static void arb_info_change(NET_M_EVENT* evs, int max, int* num, const NET_M_LINK_RT* link,
                            const NET_M_LINK* before, const NET_M_LINK* after)
{
    NET_M_EVENT item = {0};  /* 待追加事件 */
    unsigned int changed = 0;/* 变化字段位图 */

    if (NET_M_ST_READY != link->state)
    {
        return;
    }

    if (0 != strcmp(before->ip, after->ip))
    {
        changed |= NET_M_LINK_F_IP;
    }
    if (0 != strcmp(before->mask, after->mask))
    {
        changed |= NET_M_LINK_F_MASK;
    }
    if (0 != strcmp(before->gate, after->gate))
    {
        changed |= NET_M_LINK_F_GATE;
    }
    if (0 == changed)
    {
        return;
    }

    item.ev = NET_M_EV_INFO_CHANGE;
    item.u.info.cur = *after;
    item.u.info.prev = *before;
    item.u.info.changed = changed;
    net_m_arb_ev_push(evs, max, num, &item);
}

/**
 * @brief WiFi 关联状态变化上报（关联/断开不产生 netlink 事件）
 * @param evs    事件数组，不可为空
 * @param max    数组容量
 * @param num    当前条数，不可为空
 * @param link   链路运行时上下文，不可为空
 * @param before 刷新前的 WiFi 信息，不可为空
 */
static void arb_wifi_change(NET_M_EVENT* evs, int max, int* num, const NET_M_LINK_RT* link,
                            const NET_M_WIFI_INFO* before)
{
    NET_M_EVENT item = {0}; /* 待追加事件 */

    if (before->connected == link->wifi.connected)
    {
        return;
    }

    item.ev = (0 != link->wifi.connected) ? NET_M_EV_WIFI_CONNECTED :
                                            NET_M_EV_WIFI_DISCONNECTED;
    item.u.wifi.ref.type = link->type;
    (void)snprintf(item.u.wifi.ref.ifname, sizeof(item.u.wifi.ref.ifname), "%s",
                   link->ifname);
    item.u.wifi.info = link->wifi;
    net_m_arb_ev_push(evs, max, num, &item);
}

/**
 * @brief 按主链路下发 DNS（每轮调用，内部去重）
 * @details 主链路未配置 DNS 时保留现有配置；与已下发值相同则跳过，减少写盘
 * @param arb   仲裁器（读写已下发记录），不可为空
 * @param links 链路上下文数组，不可为空
 * @return 0 成功或无需下发；负值为错误码
 */
static int arb_dns_sync(NET_M_ARB* arb, NET_M_LINK_RT* links)
{
    const NET_M_LINK_RT* link = NULL; /* 主链路上下文 */
    int ret = 0;                      /* 子步骤返回值 */

    if (arb->active < 0 || arb->active >= NET_M_TYPE_MAX)
    {
        return 0;
    }

    link = &links[arb->active];
    if ('\0' == link->dns[0] || 0 == strcmp(link->dns, arb->dns))
    {
        return 0;
    }

    ret = netmgr_dns_apply(link->dns);
    if (ret < 0)
    {
        return ret;
    }

    (void)snprintf(arb->dns, sizeof(arb->dns), "%s", link->dns);
    return 0;
}

/**
 * @brief 在就绪链路中选举主链路
 * @details 参与条件与路由一致（就绪 + 有网关），保证「谁承担默认路由」与
 *          「谁是 ACTIVE」是同一条链路；同等优先级取下标小者（正常配置下不存在）
 * @param links 链路上下文数组，不可为空
 * @return 主链路下标；-1 表示无可用链路
 */
static int arb_elect(const NET_M_LINK_RT* links)
{
    int best = -1; /* 当前最优链路下标 */
    int i = 0;     /* 链路下标，从 0 开始 */

    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        if (0 == links[i].enable || NET_M_ST_READY != links[i].state)
        {
            continue;
        }
        if ('\0' == links[i].gate[0])
        {
            continue;
        }
        if (best < 0 || links[i].prio < links[best].prio)
        {
            best = i;
        }
    }
    return best;
}

/**
 * @brief 单链路的一轮处理：刷新 → WiFi 差异 → 路由同步 → 状态/信息事件 → 故障上报
 * @param arb   仲裁器，不可为空
 * @param links 链路上下文数组，不可为空
 * @param ops   操作表映射，不可为空
 * @param i     链路下标
 * @param evs   事件输出数组，不可为空
 * @param max   事件数组容量
 * @param num   输出条数，不可为空
 * @param err   本轮首个错误码，入出参，不可为空
 */
static void arb_link_cycle(NET_M_ARB* arb, NET_M_LINK_RT* links, const NET_M_OPS* const ops[],
                           int i, NET_M_EVENT* evs, int max, int* num, int* err)
{
    NET_M_EVENT item = {0};          /* 复用的单条事件 */
    NET_M_LINK before = {0};         /* 刷新前快照 */
    NET_M_LINK after = {0};          /* 刷新后快照 */
    NET_M_WIFI_INFO wifiBefore = {0};/* 刷新前 WiFi 信息 */
    NET_M_LINK_RT* link = &links[i]; /* 当前链路上下文 */
    int ret = 0;                     /* 子步骤返回值 */

    /* 故障位每轮重新评估，只反映本轮 */
    link->fault = 0;
    link->faultStage = NET_M_STAGE_OPEN;

    /* 未启用链路不刷新，只在状态刚落到 DISABLED 时补发一次事件 */
    if (0 == link->enable || NULL == ops[i])
    {
        if (link->prev != link->state)
        {
            link->prev = link->state;
            net_m_arb_snapshot_one(arb, links, i, &after);
            arb_ev_link(&item, NET_M_EV_DISABLED, &after);
            net_m_arb_ev_push(evs, max, num, &item);
        }
        return;
    }

    net_m_arb_snapshot_one(arb, links, i, &before);
    wifiBefore = link->wifi;

    /* 阶段 1：状态投影。失败即降级为 DOWN 并上报，不再静默跳过 */
    ret = ops[i]->refresh(link);
    if (ret < 0)
    {
        link->state = NET_M_ST_DOWN;
        link->fault = ret;
        link->faultStage = NET_M_STAGE_REFRESH;
        if (0 == *err)
        {
            *err = ret;
        }
        (void)net_m_route_sync(link); /* DOWN 态撤除本模块下发的路由 */
    }
    else
    {
        /* 阶段 2-a：WiFi 关联变化（无对应 netlink 事件，只能比对快照） */
        arb_wifi_change(evs, max, num, link, &wifiBefore);

        /* 阶段 3：默认路由跟随状态与优先级 */
        ret = net_m_route_sync(link);
        if (ret < 0)
        {
            link->fault = ret;
            link->faultStage = NET_M_STAGE_ROUTE;
            if (0 == *err)
            {
                *err = ret;
            }
        }
    }

    net_m_arb_snapshot_one(arb, links, i, &after);

    /* 阶段 2-b：状态翻转事件；未翻转则比对字段差异 */
    if (link->prev != link->state)
    {
        arb_ev_link(&item, arb_flip_type(link->prev, link->state), &after);
        link->prev = link->state;
        net_m_arb_ev_push(evs, max, num, &item);
    }
    else
    {
        arb_info_change(evs, max, num, link, &before, &after);
    }

    if (0 != link->fault)
    {
        arb_ev_fault(evs, max, num, link->type, (NET_M_STAGE)link->faultStage, link->fault);
    }
}

void net_m_arb_reset(NET_M_ARB* arb)
{
    if (NULL == arb)
    {
        return;
    }

    memset(arb, 0, sizeof(*arb));
    arb->active = -1;
}

int net_m_arb_run(NET_M_ARB* arb, NET_M_LINK_RT* links, const NET_M_OPS* const ops[],
                  NET_M_EVENT* evs, int max, int* num)
{
    NET_M_EVENT item = {0};         /* 复用的单条事件 */
    int oldActive = -1;             /* 本轮开始时的主链路 */
    int newActive = -1;             /* 本轮选举结果 */
    int err = 0;                    /* 本轮首个错误码 */
    int ret = 0;                    /* 子步骤返回值 */
    int i = 0;                      /* 链路下标，从 0 开始 */

    if (NULL == arb || NULL == links || NULL == ops || NULL == evs || NULL == num ||
        max <= 0)
    {
        return -NETMGRERR_PARAM;
    }

    *num = 0;
    oldActive = arb->active;

    /* ---------- 阶段 1~3：逐链路刷新 → 差异识别 → 路由同步 ---------- */
    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        arb_link_cycle(arb, links, ops, i, evs, max, num, &err);
    }

    /* ---------- 阶段 4：选举主链路 ---------- */
    newActive = arb_elect(links);
    arb->active = newActive;

    /* ---------- 阶段 5：DNS 每轮同步（去重），主链路变化上报切换 ---------- */
    ret = arb_dns_sync(arb, links);
    if (ret < 0)
    {
        if (0 == err)
        {
            err = ret;
        }
        if (newActive >= 0)
        {
            links[newActive].fault = ret;
            links[newActive].faultStage = NET_M_STAGE_DNS;
        }

        /* 链路仍就绪但子能力异常：以 DEGRADED 上报，不改变链路状态 */
        memset(&item, 0, sizeof(item));
        item.ev = NET_M_EV_DEGRADED;
        item.u.fault.type = (newActive >= 0) ? (NET_M_TYPE)newActive : NET_M_TYPE_MAX;
        item.u.fault.stage = NET_M_STAGE_DNS;
        item.u.fault.code = ret;
        net_m_arb_ev_push(evs, max, num, &item);
    }

    if (oldActive != newActive)
    {
        memset(&item, 0, sizeof(item));
        item.ev = NET_M_EV_SWITCH;
        item.u.sw.from = (oldActive >= 0) ? (NET_M_TYPE)oldActive : NET_M_TYPE_MAX;
        item.u.sw.to = (newActive >= 0) ? (NET_M_TYPE)newActive : NET_M_TYPE_MAX;
        net_m_arb_ev_push(evs, max, num, &item);
    }

    return err;
}
