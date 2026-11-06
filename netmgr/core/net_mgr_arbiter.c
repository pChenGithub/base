/**
 * @file net_mgr_arbiter.c
 * @brief netmgr 仲裁器实现：刷新 → 差异识别 → 路由同步 → 主链路选举 → 应用
 * @details 机制与约束见 net_mgr_arbiter.h 文件头。本文件只做「判断与产出」，
 *          不做进程启停、不加锁、不直接调用业务回调。
 */
#include "net_mgr_arbiter.h"

#include "net_mgr_err.h"
#include "net_mgr_dns.h"
#include "net_mgr_route.h"

#include <stdio.h>
#include <string.h>

/* ===================== 快照与事件构造 ===================== */

/**
 * @brief 上下文转对外快照（不含 ACTIVE 呈现）
 * @param ctx 链路上下文，不可为空
 * @param out 输出快照，不可为空
 */
static void arb_conv(const NETMGR_CTX* ctx, NETMGR_LINK* out)
{
    out->type = ctx->type;
    out->state = ctx->state;
    out->prio = ctx->prio;
    (void)snprintf(out->ifname, sizeof(out->ifname), "%s", ctx->ifname);
    (void)snprintf(out->ip, sizeof(out->ip), "%s", ctx->ip);
    (void)snprintf(out->mask, sizeof(out->mask), "%s", ctx->mask);
    (void)snprintf(out->gate, sizeof(out->gate), "%s", ctx->gate);
    out->err = ctx->err;
}

void netmgr_arb_snapshot_one(const NETMGR_ARB* arb, const NETMGR_CTX* links,
                             int index, NETMGR_LINK* out)
{
    if (NULL == links || NULL == out || index < 0 || index >= NETMGR_TYPE_MAX)
        return;

    arb_conv(&links[index], out);

    /* ACTIVE 是「选举结果」，不属于上下文客观状态，只在对外快照里叠加 */
    if (NULL != arb && index == arb->active && NETMGR_ST_READY == links[index].state)
        out->state = NETMGR_ST_ACTIVE;
}

void netmgr_arb_snapshot(const NETMGR_ARB* arb, const NETMGR_CTX* links,
                         NETMGR_LINK* list, int* num)
{
    int cap = 0; /* 输出容量 */
    int cnt = 0; /* 已写入条数 */
    int i = 0;   /* 链路下标，从 0 开始 */

    if (NULL == links || NULL == list || NULL == num || *num <= 0)
        return;

    cap = *num;
    for (i = 0; i < NETMGR_TYPE_MAX && cnt < cap; ++i)
    {
        memset(&list[cnt], 0, sizeof(NETMGR_LINK));
        arb_conv(&links[i], &list[cnt]);

        /* 主链路且已就绪 → 对业务呈现 ACTIVE（DISABLED/DOWN 不伪装成主链路） */
        if (NULL != arb && i == arb->active && NETMGR_ST_READY == links[i].state)
            list[cnt].state = NETMGR_ST_ACTIVE;
        ++cnt;
    }
    *num = cnt;
}

void netmgr_arb_ev_link(NETMGR_EVENT* ev, NETMGR_EV type, const NETMGR_LINK* link)
{
    if (NULL == ev || NULL == link)
        return;

    memset(ev, 0, sizeof(*ev));
    ev->ev = type;
    ev->u.link = *link;
}

void netmgr_arb_ev_push(NETMGR_EVENT* evs, int max, int* num, const NETMGR_EVENT* ev)
{
    if (NULL == evs || NULL == num || NULL == ev || *num >= max)
        return;

    evs[*num] = *ev;
    ++(*num);
}

/* ===================== 阶段 2：差异识别 ===================== */

/**
 * @brief 判定状态翻转对应的事件类型
 * @details 语义按「从哪来、到哪去」确定，调用方负责更新 prev：
 *          DISABLED → 其它      ：ENABLED（链路被重新启用）
 *          非 READY → READY      ：UP（链路可用）
 *          其它 → 非 READY        ：DOWN（链路不可用）
 * @param prev  上一轮状态
 * @param now   本轮状态
 * @return 事件类型
 */
static NETMGR_EV arb_flip_type(NETMGR_STATE prev, NETMGR_STATE now)
{
    if (NETMGR_ST_DISABLED == prev)
        return NETMGR_EV_ENABLED;

    return (NETMGR_ST_READY == now) ? NETMGR_EV_UP : NETMGR_EV_DOWN;
}

/**
 * @brief 状态未变时按字段比对，产出 INFO_CHANGE 事件
 * @details 只在 READY 态比对：DOWN 态的 ip/mask/gate 本就是空值，比较无意义
 * @param evs 事件数组，不可为空
 * @param max 数组容量
 * @param num 当前条数，不可为空
 * @param ctx 链路上下文，不可为空
 * @param before 刷新前快照，不可为空
 * @param after  刷新后快照，不可为空
 */
static void arb_info_change(NETMGR_EVENT* evs, int max, int* num,
                            const NETMGR_CTX* ctx, const NETMGR_LINK* before,
                            const NETMGR_LINK* after)
{
    NETMGR_EVENT evItem = {0}; /* 待追加事件 */
    unsigned int changed = 0;  /* 变化字段位图 */

    if (NETMGR_ST_READY != ctx->state)
        return;

    if (0 != strcmp(before->ip, after->ip))
        changed |= NETMGR_LINK_F_IP;
    if (0 != strcmp(before->mask, after->mask))
        changed |= NETMGR_LINK_F_MASK;
    if (0 != strcmp(before->gate, after->gate))
        changed |= NETMGR_LINK_F_GATE;
    if (0 == changed)
        return;

    evItem.ev = NETMGR_EV_INFO_CHANGE;
    evItem.u.info.cur = *after;
    evItem.u.info.prev = *before;
    evItem.u.info.changed = changed;
    netmgr_arb_ev_push(evs, max, num, &evItem);
}

/**
 * @brief WiFi 关联状态变化上报
 * @details 关联/断开不产生 netlink 事件，只能靠刷新前后的 WiFi 快照差异发现
 * @param evs 事件数组，不可为空
 * @param max 数组容量
 * @param num 当前条数，不可为空
 * @param ctx 链路上下文，不可为空
 * @param before 刷新前的 WiFi 信息
 */
static void arb_wifi_change(NETMGR_EVENT* evs, int max, int* num,
                            const NETMGR_CTX* ctx, const NETMGR_WIFI_INFO* before)
{
    NETMGR_EVENT evItem = {0}; /* 待追加事件 */

    if (before->connected == ctx->wifi.connected)
        return;

    evItem.ev = (0 != ctx->wifi.connected) ? NETMGR_EV_WIFI_CONNECTED :
                                            NETMGR_EV_WIFI_DISCONNECTED;
    evItem.u.wifi.ref.type = ctx->type;
    (void)snprintf(evItem.u.wifi.ref.ifname, sizeof(evItem.u.wifi.ref.ifname), "%s",
                   ctx->ifname);
    evItem.u.wifi.info = ctx->wifi;
    netmgr_arb_ev_push(evs, max, num, &evItem);
}

/* ===================== 阶段 3：路由同步 ===================== */

/**
 * @brief 按当前状态与优先级同步一条链路的默认路由
 * @details 规则：
 *          · 参与条件 = 就绪 + 网关非空（AP 等无网关链路不参与默认路由）
 *          · metric = NETMGR_METRIC_BASE + prio，保证「优先级小者在路由表中更优」
 *          · 网关与 metric 都未变时不触碰内核，避免无谓抖动
 *          · 变更时先删旧再建新，保证同一接口只有一条本模块下发的默认路由
 * @param ctx 链路上下文，不可为空
 * @return 0 成功或无需变更；负值为 NETMGRERR_* 错误码（删旧失败不阻断建新）
 */
static int arb_route_sync(NETMGR_CTX* ctx)
{
    int need = 0;   /* 1=应下发默认路由 */
    int metric = 0; /* 目标 metric */
    int ret = 0;    /* 子步骤返回值 */
    int err = 0;    /* 首个错误码 */

    need = (NETMGR_ST_READY == ctx->state && '\0' != ctx->gate[0]) ? 1 : 0;
    metric = NETMGR_METRIC_BASE + (int)ctx->prio;

    if (0 == need)
    {
        if (0 == ctx->route_on)
            return 0;

        /* 链路不再具备资格（DOWN 或网关清空）：撤掉本模块下发的路由 */
        ret = netmgr_route_del(ctx->ifname, ctx->rgate, ctx->rmetric);
        ctx->route_on = 0;
        return ret;
    }

    if (0 != ctx->route_on && ctx->rmetric == metric && 0 == strcmp(ctx->rgate, ctx->gate))
        return 0;

    if (0 != ctx->route_on)
    {
        ret = netmgr_route_del(ctx->ifname, ctx->rgate, ctx->rmetric);
        if (ret < 0)
            err = ret;
        ctx->route_on = 0;
    }

    ret = netmgr_route_add(ctx->ifname, ctx->gate, metric);
    if (ret < 0)
        return (0 != err) ? err : ret;

    ctx->route_on = 1;
    ctx->rmetric = metric;
    (void)snprintf(ctx->rgate, sizeof(ctx->rgate), "%s", ctx->gate);
    return err;
}

/* ===================== 阶段 5：DNS 应用 ===================== */

/**
 * @brief 按主链路下发 DNS（去重）
 * @details 主链路未配置 DNS 时保留现有配置，避免把可用配置清空；
 *          与已下发值相同则跳过，减少写盘
 * @param arb   仲裁器（读写已下发记录），不可为空
 * @param links 链路上下文数组，不可为空
 * @return 0 成功或无需下发；负值为 NETMGRERR_* 错误码
 */
static int arb_dns_sync(NETMGR_ARB* arb, NETMGR_CTX* links)
{
    const NETMGR_CTX* ctx = NULL; /* 主链路上下文 */
    int ret = 0;                  /* 子步骤返回值 */

    if (arb->active < 0 || arb->active >= NETMGR_TYPE_MAX)
        return 0;

    ctx = &links[arb->active];
    if ('\0' == ctx->dns[0] || 0 == strcmp(ctx->dns, arb->dns))
        return 0;

    ret = netmgr_dns_apply(ctx->dns);
    if (ret < 0)
        return ret;

    (void)snprintf(arb->dns, sizeof(arb->dns), "%s", ctx->dns);
    return 0;
}

/* ===================== 阶段 4：主链路选举 ===================== */

/**
 * @brief 在就绪链路中选举主链路
 * @details 参与条件与路由一致（就绪 + 有网关），保证「谁承担默认路由」与
 *          「谁是 ACTIVE」始终是同一条链路；同等优先级取下标小者
 * @param links 链路上下文数组，不可为空
 * @return 主链路下标；-1 表示无可用链路
 */
static int arb_elect(const NETMGR_CTX* links)
{
    int best = -1; /* 当前最优链路下标 */
    int i = 0;     /* 链路下标，从 0 开始 */

    for (i = 0; i < NETMGR_TYPE_MAX; ++i)
    {
        if (0 == links[i].enable || NETMGR_ST_READY != links[i].state)
            continue;
        if ('\0' == links[i].gate[0])
            continue;
        if (best < 0 || links[i].prio < links[best].prio)
            best = i;
    }
    return best;
}

void netmgr_arb_reset(NETMGR_ARB* arb)
{
    if (NULL == arb)
        return;

    memset(arb, 0, sizeof(*arb));
    arb->active = -1;
}

/* ===================== 一轮仲裁 ===================== */

int netmgr_arb_run(NETMGR_ARB* arb, NETMGR_CTX* links, const NETMGR_OPS* const ops[],
                   NETMGR_EVENT* evs, int max, int* num)
{
    NETMGR_EVENT evItem = {0};         /* 复用的单条事件 */
    NETMGR_LINK before = {0};          /* 刷新前快照 */
    NETMGR_LINK after = {0};           /* 刷新后快照 */
    NETMGR_WIFI_INFO wifiBefore = {0}; /* 刷新前 WiFi 信息 */
    NETMGR_CTX* ctx = NULL;            /* 当前链路上下文 */
    int old_active = -1;               /* 本轮开始时的主链路 */
    int new_active = -1;               /* 本轮选举结果 */
    int err = 0;                       /* 本轮首个错误码 */
    int ret = 0;                       /* 子步骤返回值 */
    int i = 0;                         /* 链路下标，从 0 开始 */

    if (NULL == arb || NULL == links || NULL == ops || NULL == evs || NULL == num || max <= 0)
        return -NETMGRERR_PARAM;

    *num = 0;
    old_active = arb->active;

    /* ---------- 阶段 1 + 2 + 3：逐链路刷新 → 差异识别 → 路由同步 ---------- */
    for (i = 0; i < NETMGR_TYPE_MAX; ++i)
    {
        ctx = &links[i];

        /* 未启用链路不刷新，只在状态刚落到 DISABLED 时补发一次事件 */
        if (0 == ctx->enable || NULL == ops[i])
        {
            if (ctx->prev != ctx->state)
            {
                ctx->prev = ctx->state;
                netmgr_arb_snapshot_one(arb, links, i, &after);
                netmgr_arb_ev_link(&evItem, NETMGR_EV_DISABLED, &after);
                netmgr_arb_ev_push(evs, max, num, &evItem);
            }
            continue;
        }

        netmgr_arb_snapshot_one(arb, links, i, &before);
        wifiBefore = ctx->wifi;

        /* 阶段 1：只做状态投影（读内核 → 写上下文），失败不阻断其它链路 */
        ret = ops[i]->refresh(ctx);
        if (ret < 0)
        {
            if (0 == err)
                err = ret;
            continue;
        }

        /* 阶段 2-a：WiFi 关联变化（无对应 netlink 事件，只能比对快照） */
        arb_wifi_change(evs, max, num, ctx, &wifiBefore);

        /* 阶段 3：默认路由跟随状态与优先级 */
        ret = arb_route_sync(ctx);
        if (ret < 0 && 0 == err)
            err = ret;

        netmgr_arb_snapshot_one(arb, links, i, &after);

        /* 阶段 2-b：状态翻转事件；未翻转则比对字段差异 */
        if (ctx->prev != ctx->state)
        {
            evItem.ev = arb_flip_type(ctx->prev, ctx->state);
            ctx->prev = ctx->state;
            netmgr_arb_ev_link(&evItem, evItem.ev, &after);
            netmgr_arb_ev_push(evs, max, num, &evItem);
        }
        else
        {
            arb_info_change(evs, max, num, ctx, &before, &after);
        }
    }

    /* ---------- 阶段 4：选举主链路（选举结果影响 ACTIVE 呈现） ---------- */
    new_active = arb_elect(links);
    arb->active = new_active;

    /* ---------- 阶段 5：主链路变化 → 应用 DNS + 产出切换事件 ---------- */
    if (old_active != new_active)
    {
        ret = arb_dns_sync(arb, links);
        if (ret < 0 && 0 == err)
            err = ret;

        memset(&evItem, 0, sizeof(evItem));
        evItem.ev = NETMGR_EV_SWITCH;
        evItem.u.sw.from = (old_active >= 0) ? (NETMGR_TYPE)old_active : NETMGR_TYPE_MAX;
        evItem.u.sw.to = (new_active >= 0) ? (NETMGR_TYPE)new_active : NETMGR_TYPE_MAX;
        netmgr_arb_ev_push(evs, max, num, &evItem);
    }

    return err;
}
