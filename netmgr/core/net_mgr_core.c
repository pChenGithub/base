/**
 * @file net_mgr_core.c
 * @brief netmgr 仲裁层实现：链路装配、优先级仲裁、默认路由同步与事件分发
 * @details 本层直接基于 sys 层能力实现，不再设链路实现层：
 *          1) 三条链路的 open/close/refresh 与就绪判定在本层内部（net_mgr_eth/wifi/cell/link.c）；
 *          2) 本层不创建线程：netlink 事件由 sys 事件回调直接驱动一轮仲裁，
 *             不产生事件的状态变化由业务周期调用 netmgr_core_poll 驱动；
 *          3) 就绪链路按其优先级下发默认路由，metric 取优先级值，由内核选路；
 *          4) 就绪链路中优先级最高者标记为 ACTIVE，仅用于状态上报与 DNS 下发。
 */
#include "net_mgr_core.h"

#include "net_mgr_core_int.h"

#include "net_mgr_dhcp.h"
#include "net_mgr_dns.h"
#include "net_mgr_err.h"
#include "net_mgr_event.h"
#include "net_mgr_route.h"
#include "net_mgr_util.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

/** 路由 metric 基准值，叠加优先级后作为最终 metric */
#define NETMGR_METRIC_BASE  0

/** 一轮仲裁中最多收集的事件数：三链路 ×（状态翻转 + 信息变化）+ 主链路切换 */
#define CORE_EV_MAX  8

/** 管理核心上下文，承载仲裁层全部运行状态 */
typedef struct {
    int started;                            /**< 1=服务已启动 */
    int active;                             /**< 当前生效链路索引，-1 表示无 */
    netmgr_event_cb cb;                     /**< 事件回调，可为空 */
    void* user;                             /**< 事件回调的用户指针，可为空 */
    char dns[NETMGR_IP_LEN];                /**< 当前已下发的 DNS */
    pthread_mutex_t lock;                   /**< 仲裁互斥锁，事件回调与业务轮询共用 */
    const NETMGR_OPS* ops[NETMGR_TYPE_MAX]; /**< 链路类型到操作表的映射，下标与 NETMGR_TYPE 对齐 */
    NETMGR_ETH_CONF ethConf;                /**< 以太网配置副本，供链路开关按上次配置重新拉起 */
    NETMGR_WIFI_CONF wifiConf;              /**< WiFi 配置副本 */
    NETMGR_CELL_CONF cellConf;              /**< 4G 配置副本 */
    NETMGR_CTX link[NETMGR_TYPE_MAX];       /**< 三条链路上下文 */
} NETMGR_CORE;

/**
 * 管理核心上下文（模块内单实例，仲裁层状态全部收敛于此）
 * 用途：保存三条链路运行态、当前生效链路、业务回调、已下发 DNS 与仲裁互斥锁
 * 单位/范围：active 取 -1 或 0~2 的链路下标；prio 取 0~255；started 取 0/1
 * 默认值依据：静态零初始化，start 时显式重建（active=-1 表示无生效链路）；互斥锁用静态初始化器
 * 写入方/读取方：sys 层事件回调与业务轮询接口均在 g_core.lock 保护下读写
 * 生命周期：仅内存态，start/stop 管理，掉电不保存
 */
static NETMGR_CORE g_core = {
    .ops = {&core_eth_ops, &core_wifi_ops, &core_cell_ops},
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/**
 * @brief 执行一轮仲裁：刷新状态、同步路由、选举主链路、上报事件
 * @return 0 全部步骤成功；负值为本轮首个失败步骤的错误码
 */
static int core_tick(void);

/**
 * @brief 上报一次默认路由变化事件
 * @param nev 触发变化的 netlink 路由事件，不可为空
 */
static void core_report_route(const NETMGR_NEV* nev);

/**
 * @brief netlink 事件回调：任一链路/地址/路由变化立即驱动一轮仲裁
 * @details 回调运行在 sys 层事件接收线程内，直接处理事件而不是唤醒其它线程；
 *          g_core.lock 保证与兜底线程串行，不会并发刷新
 * @param ev   事件内容，当前不区分，统一交由状态刷新判断
 * @param user 未使用
 * @return 恒返回 0
 */
static int core_event_cb(const NETMGR_NEV* ev, void* user)
{
    (void)user;

    /* 事件到达即跑一轮仲裁，状态与信息变化由仲裁层统一上报 */
    (void)core_tick();

    /* 默认路由自身的增删（本模块下发或外部改写）单独通知业务 */
    if (NULL != ev && 0 != ev->isDefault &&
        (NETMGR_NEV_ROUTE_ADD == ev->type || NETMGR_NEV_ROUTE_DEL == ev->type))
    {
        core_report_route(ev);
    }
    return 0;
}

/**
 * @brief 链路上下文转换为对外快照
 * @param ctx  链路上下文，不可为空
 * @param link 输出快照，不可为空
 */
static void core_conv(const NETMGR_CTX* ctx, NETMGR_LINK* link)
{
    if (NULL == ctx || NULL == link)
        return;

    link->type = ctx->type;
    link->state = ctx->state;
    link->prio = ctx->prio;
    snprintf(link->ifname, sizeof(link->ifname), "%s", ctx->ifname);
    snprintf(link->ip, sizeof(link->ip), "%s", ctx->ip);
    snprintf(link->mask, sizeof(link->mask), "%s", ctx->mask);
    snprintf(link->gate, sizeof(link->gate), "%s", ctx->gate);
    link->err = ctx->err;
}

/** 单条待上报事件：事件数据为值语义，回调期间不访问内部上下文 */
typedef NETMGR_EVENT CORE_EV_ITEM;

/**
 * @brief 把链路上下文转换为对外快照（含 ACTIVE 呈现）
 * @param ctx 链路上下文，不可为空
 * @param out 输出快照，不可为空
 */
static void core_snapshot(const NETMGR_CTX* ctx, NETMGR_LINK* out)
{
    if (NULL == ctx || NULL == out)
        return;

    core_conv(ctx, out);

    /* 与当前生效链路一致时对外呈现 ACTIVE，便于应用直接区分主链路 */
    if (g_core.active >= 0 && g_core.active < NETMGR_TYPE_MAX &&
        &g_core.link[g_core.active] == ctx && NETMGR_ST_READY == ctx->state)
    {
        out->state = NETMGR_ST_ACTIVE;
    }
}

/**
 * @brief 向本轮事件缓冲追加一条事件
 * @param items 事件缓冲，不可为空
 * @param num   当前条数，不可为空
 * @param ev    已构造好的事件，不可为空（值拷贝，调用后原对象可复用）
 */
static void core_ev_add(CORE_EV_ITEM* items, int* num, const NETMGR_EVENT* ev)
{
    if (NULL == items || NULL == num || NULL == ev || *num >= CORE_EV_MAX)
        return;

    items[*num] = *ev;
    ++(*num);
}

/**
 * @brief 用链路快照填充事件载荷（UP/DOWN 等以完整快照承载的事件）
 * @param ev    待填充事件，不可为空
 * @param type  事件类型
 * @param link  链路快照，不可为空
 */
static void core_ev_link(NETMGR_EVENT* ev, NETMGR_EV type, const NETMGR_LINK* link)
{
    if (NULL == ev || NULL == link)
        return;

    memset(ev, 0, sizeof(*ev));
    ev->ev = type;
    ev->u.link = *link;
}

/**
 * @brief 分发一条事件给已注册的回调
 * @details 在锁外调用，业务回调内再调本模块接口不会造成仲裁锁自死锁
 * @param ev 事件数据，不可为空
 */
static void core_dispatch(const NETMGR_EVENT* ev)
{
    if (NULL == ev || NULL == g_core.cb)
        return;

    (void)g_core.cb(ev, g_core.user);
}

/**
 * @brief 按当前优先级同步一条链路的默认路由
 * @param ctx 链路上下文，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码（首个失败步骤）
 */
static int core_route_sync(NETMGR_CTX* ctx)
{
    int need = 0;   /* 1=该链路应下发默认路由 */
    int metric = 0; /* 目标 metric，等于优先级 */
    int ret = 0;    /* 子步骤返回值 */
    int err = 0;    /* 首个错误码，0 表示尚无失败 */

    if (NULL == ctx)
        return -NETMGRERR_PARAM;

    /* 就绪且有网关才参与默认路由，AP 模式等场景网关为空 */
    need = (NETMGR_ST_READY == ctx->state && '\0' != ctx->gate[0]) ? 1 : 0;
    metric = NETMGR_METRIC_BASE + (int)ctx->prio;

    if (!need)
    {
        if (!ctx->route_on)
            return 0;
        ret = netmgr_route_del(ctx->ifname, ctx->rgate, ctx->rmetric);
        ctx->route_on = 0;
        return ret;
    }

    /* 网关与 metric 均未变化时无需触碰内核，减少无谓抖动 */
    if (ctx->route_on && ctx->rmetric == metric && 0 == strcmp(ctx->rgate, ctx->gate))
        return 0;

    /* 先删旧条目再建新条目，保证同一接口只有一条本模块下发的默认路由；
       旧条目删除失败不阻断新条目下发，错误码在末尾上抛 */
    if (ctx->route_on)
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
    snprintf(ctx->rgate, sizeof(ctx->rgate), "%s", ctx->gate);
    return err;
}

/**
 * @brief 按当前生效链路下发 DNS
 * @return 0 成功（无生效链路或无需下发同样返回 0）；负值为 NETMGRERR_* 错误码
 */
static int core_dns_sync(void)
{
    const NETMGR_CTX* ctx = NULL; /* 当前生效链路 */
    int ret = 0;                  /* 子步骤返回值 */

    if (g_core.active < 0 || g_core.active >= NETMGR_TYPE_MAX)
        return 0;
    ctx = &g_core.link[g_core.active];

    /* 生效链路未配置 DNS 时保留现有配置，避免把可用配置清空 */
    if ('\0' == ctx->dns[0] || 0 == strcmp(ctx->dns, g_core.dns))
        return 0;

    ret = netmgr_dns_apply(ctx->dns);
    if (ret < 0)
        return ret;
    snprintf(g_core.dns, sizeof(g_core.dns), "%s", ctx->dns);
    return 0;
}

/**
 * @brief 执行一轮仲裁：刷新状态、同步路由、选举主链路、收集并上报事件
 * @return 0 全部步骤成功；负值为本轮首个失败步骤的错误码
 */
static int core_tick(void)
{
    CORE_EV_ITEM items[CORE_EV_MAX] = {0}; /* 本轮收集的事件 */
    NETMGR_EVENT evItem = {0};             /* 待追加的单条事件 */
    NETMGR_LINK before = {0};              /* 刷新前的链路快照 */
    NETMGR_LINK after = {0};               /* 刷新后的链路快照 */
    NETMGR_WIFI_INFO wifiBefore = {0};     /* 刷新前的 WiFi 连接信息 */
    NETMGR_CTX* ctx = NULL;                /* 当前遍历的链路上下文 */
    NETMGR_EV flip = NETMGR_EV_DOWN;       /* 状态翻转对应的事件类型 */
    unsigned int changed = 0;              /* INFO_CHANGE 的变化字段位图 */
    int itemNum = 0;                       /* 已收集事件数 */
    int old_active = -1;                   /* 上一轮生效链路 */
    int new_active = -1;                   /* 本轮生效链路 */
    int err = 0;                           /* 本轮首个错误码，0 表示尚无失败 */
    int ret = 0;                           /* 子步骤返回值 */
    int i = 0;                             /* 链路下标，从 0 开始 */

    pthread_mutex_lock(&g_core.lock);
    old_active = g_core.active;

    /* 第一阶段：逐条刷新链路自身状态，并同步其默认路由 */
    for (i = 0; i < NETMGR_TYPE_MAX; ++i)
    {
        ctx = &g_core.link[i];

        /* 未启用链路不刷新，只在状态刚变为 DISABLED 时补发一次事件 */
        if (!ctx->enable || NULL == g_core.ops[i])
        {
            if (ctx->prev != ctx->state)
            {
                ctx->prev = ctx->state;
                core_snapshot(ctx, &after);
                core_ev_link(&evItem, NETMGR_EV_DISABLED, &after);
                core_ev_add(items, &itemNum, &evItem);
            }
            continue;
        }

        core_snapshot(ctx, &before);
        wifiBefore = ctx->wifi;
        ret = g_core.ops[i]->refresh(ctx);
        if (ret < 0)
        {
            /* 单条链路刷新失败不阻断其它链路，仅记录首个错误码 */
            if (0 == err)
                err = ret;
            continue;
        }

        /* WiFi 关联状态不产生 netlink 事件，按刷新前后的差异单独上报 */
        if (wifiBefore.connected != ctx->wifi.connected)
        {
            memset(&evItem, 0, sizeof(evItem));
            evItem.ev = (0 != ctx->wifi.connected) ? NETMGR_EV_WIFI_CONNECTED :
                                                    NETMGR_EV_WIFI_DISCONNECTED;
            evItem.u.wifi.ref.type = ctx->type;
            (void)snprintf(evItem.u.wifi.ref.ifname, sizeof(evItem.u.wifi.ref.ifname), "%s",
                           ctx->ifname);
            evItem.u.wifi.info = ctx->wifi;
            core_ev_add(items, &itemNum, &evItem);
        }

        ret = core_route_sync(ctx);
        if (ret < 0 && 0 == err)
            err = ret;

        core_snapshot(ctx, &after);

        if (ctx->prev != ctx->state)
        {
            /* 状态翻转：从未启用到启用报 ENABLED，其余按是否就绪报 UP / DOWN */
            if (NETMGR_ST_DISABLED == ctx->prev)
                flip = NETMGR_EV_ENABLED;
            else if (NETMGR_ST_READY == ctx->state)
                flip = NETMGR_EV_UP;
            else
                flip = NETMGR_EV_DOWN;

            ctx->prev = ctx->state;
            core_ev_link(&evItem, flip, &after);
            core_ev_add(items, &itemNum, &evItem);
        }
        else if (NETMGR_ST_READY == ctx->state)
        {
            /* 状态未变时按字段比对，给出变化位图与前后快照 */
            changed = 0;
            if (0 != strcmp(before.ip, after.ip))
                changed |= NETMGR_LINK_F_IP;
            if (0 != strcmp(before.mask, after.mask))
                changed |= NETMGR_LINK_F_MASK;
            if (0 != strcmp(before.gate, after.gate))
                changed |= NETMGR_LINK_F_GATE;

            if (0 != changed)
            {
                memset(&evItem, 0, sizeof(evItem));
                evItem.ev = NETMGR_EV_INFO_CHANGE;
                evItem.u.info.cur = after;
                evItem.u.info.prev = before;
                evItem.u.info.changed = changed;
                core_ev_add(items, &itemNum, &evItem);
            }
        }
    }

    /* 第二阶段：就绪链路中优先级最高者成为生效链路，AP 等无网关链路不参与 */
    for (i = 0; i < NETMGR_TYPE_MAX; ++i)
    {
        ctx = &g_core.link[i];
        if (!ctx->enable || NETMGR_ST_READY != ctx->state || '\0' == ctx->gate[0])
            continue;
        if (new_active < 0 || ctx->prio < g_core.link[new_active].prio)
            new_active = i;
    }
    g_core.active = new_active;

    /* 第三阶段：生效链路变化时先更新 DNS，再收集主链路切换事件 */
    if (old_active != new_active)
    {
        ret = core_dns_sync();
        if (ret < 0 && 0 == err)
            err = ret;

        memset(&evItem, 0, sizeof(evItem));
        evItem.ev = NETMGR_EV_SWITCH;
        evItem.u.sw.from = (old_active >= 0) ? (NETMGR_TYPE)old_active : NETMGR_TYPE_MAX;
        evItem.u.sw.to = (new_active >= 0) ? (NETMGR_TYPE)new_active : NETMGR_TYPE_MAX;
        core_ev_add(items, &itemNum, &evItem);
    }

    pthread_mutex_unlock(&g_core.lock);

    /* 锁外分发：业务回调内可安全调用本模块的查询与配置接口 */
    for (i = 0; i < itemNum; ++i)
        core_dispatch(&items[i]);
    return err;
}

/**
 * @brief 上报一次默认路由变化事件
 * @param nev 触发变化的 netlink 路由事件，不可为空
 */
static void core_report_route(const NETMGR_NEV* nev)
{
    CORE_EV_ITEM item = {0}; /* 待分发事件 */
    NETMGR_LINK snap = {0};  /* 命中的链路快照 */
    int started = 0;         /* 1=服务运行中 */
    int i = 0;               /* 链路下标，从 0 开始 */

    if (NULL == nev || '\0' == nev->ifname[0])
        return;

    pthread_mutex_lock(&g_core.lock);
    started = g_core.started;
    if (started)
    {
        for (i = 0; i < NETMGR_TYPE_MAX; ++i)
        {
            if (0 != strcmp(g_core.link[i].ifname, nev->ifname))
                continue;
            core_snapshot(&g_core.link[i], &snap);
            break;
        }
    }
    pthread_mutex_unlock(&g_core.lock);

    /* 事件来自本模块未管理的接口，无对应链路可上报 */
    if (!started || '\0' == snap.ifname[0])
        return;

    memset(&item, 0, sizeof(item));
    item.ev = NETMGR_EV_ROUTE_CHANGE;
    item.u.route.ref.type = snap.type;
    (void)snprintf(item.u.route.ref.ifname, sizeof(item.u.route.ref.ifname), "%s", snap.ifname);
    item.u.route.isAdd = (NETMGR_NEV_ROUTE_ADD == nev->type) ? 1 : 0;
    (void)snprintf(item.u.route.gate, sizeof(item.u.route.gate), "%s", nev->gate);
    core_dispatch(&item);
}

NETMGR_DHCP_RET core_dhcp_cb(NETMGR_DHCP_STATE state, const char* ifname, int err, void* user)
{
    NETMGR_DHCP_BASE* base = (NETMGR_DHCP_BASE*)user; /* 链路私有数据的公共头 */
    int failErr = (NETMGR_DHCP_FAIL == state) ? err : 0; /* FAIL 时上抛的错误码，其余为 0 */

    if (NULL == base || NULL == ifname)
        return NETMGR_DHCP_RET_OK;

    /* 只记录状态与错误码；地址仍由刷新流程从接口读取，避免跨线程改写链路上下文 */
    base->dhcpState = state;
    base->dhcpErr = failErr;

    /* 同步上报仲裁层，转成业务可见的 DHCP 事件 */
    core_report_dhcp(base->type, (int)state, failErr);
    return NETMGR_DHCP_RET_OK;
}

void core_report_dhcp(NETMGR_TYPE type, int state, int err)
{
    CORE_EV_ITEM item = {0}; /* 待分发事件 */
    NETMGR_LINK snap = {0};  /* 当前链路快照 */
    int started = 0;         /* 1=服务运行中 */
    NETMGR_EV ev = NETMGR_EV_DHCP_RUNNING; /* 事件类型 */

    if ((int)type < 0 || (int)type >= NETMGR_TYPE_MAX)
        return;

    pthread_mutex_lock(&g_core.lock);
    started = g_core.started;
    if (started)
        core_snapshot(&g_core.link[type], &snap);
    pthread_mutex_unlock(&g_core.lock);

    if (!started)
        return;

    if (NETMGR_DHCP_RUNNING == state)
        ev = NETMGR_EV_DHCP_RUNNING;
    else if (NETMGR_DHCP_SUCC == state)
        ev = NETMGR_EV_DHCP_SUCC;
    else
        ev = NETMGR_EV_DHCP_FAIL;

    memset(&item, 0, sizeof(item));
    item.ev = ev;
    item.u.dhcp.ref.type = type;
    (void)snprintf(item.u.dhcp.ref.ifname, sizeof(item.u.dhcp.ref.ifname), "%s", snap.ifname);
    item.u.dhcp.err = (NETMGR_DHCP_FAIL == state) ? err : 0;
    core_dispatch(&item);
}



/**
 * @brief 按配置拉起单条链路
 * @param type   链路类型
 * @param conf   链路配置，不可为空
 * @param enable 1=启用
 * @param prio   优先级
 * @return 0 成功（未启用同样返回 0）；负值为 NETMGRERR_* 错误码
 */
static int core_link_open(NETMGR_TYPE type, const void* conf, int enable, unsigned char prio)
{
    NETMGR_CTX* ctx = &g_core.link[type]; /* 目标链路上下文 */
    int ret = 0;                          /* 子步骤返回值 */

    /* 仅 netmgr_core_start 首次装配时调用一次：start 入口有 started 守卫拒绝重入，
       不存在「已 open 再 open」的路径；运行期开关走 core_link_reset 的先关后开 */
    if (!enable)
        return 0;
    if (NULL == g_core.ops[type])
    {
        return -NETMGRERR_CONF;
    }

    pthread_mutex_lock(&g_core.lock);
    ctx->prio = prio;
    ctx->enable = 1;
    ret = g_core.ops[type]->open(ctx, conf);
    if (ret < 0)
    {
        ctx->enable = 0;
        ctx->state = NETMGR_ST_DISABLED;
    }
    pthread_mutex_unlock(&g_core.lock);
    return ret;
}

/**
 * @brief 拆除全部已启用链路：先撤路由再关链路
 * @return 0 全部步骤成功；负值为首个失败步骤的错误码
 */
static int core_links_stop(void)
{
    NETMGR_CTX* ctx = NULL; /* 当前遍历的链路上下文 */
    int err = 0;            /* 首个错误码，0 表示尚无失败 */
    int ret = 0;            /* 子步骤返回值 */
    int i = 0;              /* 链路下标，从 0 开始 */

    pthread_mutex_lock(&g_core.lock);
    for (i = 0; i < NETMGR_TYPE_MAX; ++i)
    {
        ctx = &g_core.link[i];
        if (!ctx->enable)
            continue;

        /* 先撤默认路由再关链路，避免关闭过程中仍按旧网关转发 */
        if (ctx->route_on)
        {
            ret = netmgr_route_del(ctx->ifname, ctx->rgate, ctx->rmetric);
            if (ret < 0 && 0 == err)
                err = ret;
            ctx->route_on = 0;
        }
        if (NULL != g_core.ops[i])
        {
            ret = g_core.ops[i]->close(ctx);
            if (ret < 0 && 0 == err)
                err = ret;
        }
        ctx->enable = 0;
        ctx->state = NETMGR_ST_DISABLED;
        ctx->prev = NETMGR_ST_DISABLED;
        ctx->err = 0;
    }
    g_core.active = -1;
    pthread_mutex_unlock(&g_core.lock);
    return err;
}

/**
 * @brief 重新装配单条链路：先撤旧配置再按新配置拉起
 * @param type   链路类型
 * @param conf   链路配置，不可为空
 * @param enable 1=启用
 * @param prio   优先级
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int core_link_reset(NETMGR_TYPE type, const void* conf, int enable, unsigned char prio)
{
    NETMGR_CTX* ctx = &g_core.link[type]; /* 目标链路上下文 */
    int err = 0;                          /* 首个错误码，0 表示尚无失败 */
    int ret = 0;                          /* 子步骤返回值 */

    pthread_mutex_lock(&g_core.lock);
    if (!g_core.started)
    {
        pthread_mutex_unlock(&g_core.lock);
        return -NETMGRERR_STATE;
    }

    /* 第一步：撤掉旧路由，避免新旧配置交替期间按旧网关转发 */
    if (ctx->route_on)
    {
        ret = netmgr_route_del(ctx->ifname, ctx->rgate, ctx->rmetric);
        if (ret < 0)
            err = ret;
        ctx->route_on = 0;
    }

    /* 第二步：关闭旧链路，释放其占用的进程与地址 */
    if (ctx->enable && NULL != g_core.ops[type])
    {
        ret = g_core.ops[type]->close(ctx);
        if (ret < 0 && 0 == err)
            err = ret;
        ctx->enable = 0;
    }

    if (!enable)
    {
        /* 只改状态，prev 留给仲裁轮次比对，保证 DISABLED 事件不丢 */
        ctx->state = NETMGR_ST_DISABLED;
        ctx->ip[0] = '\0';
        ctx->mask[0] = '\0';
        ctx->gate[0] = '\0';
        ctx->err = 0;
        pthread_mutex_unlock(&g_core.lock);
        return err;
    }

    /* 第三步：按新配置拉起，失败时保持停用状态并上报错误。
       上一步已无条件 close 旧链路，此处是唯一 open 点，重复调用 link_enable(on=1)
       表现为「先关后开」的整链重建，不会叠开第二个链路实例 */
    ctx->prio = prio;
    ctx->enable = 1;
    ret = g_core.ops[type]->open(ctx, conf);
    if (ret < 0)
    {
        ctx->enable = 0;
        ctx->state = NETMGR_ST_DISABLED;
        pthread_mutex_unlock(&g_core.lock);
        return ret;
    }
    pthread_mutex_unlock(&g_core.lock);
    return err;
}
int netmgr_core_start(const NETMGR_CONF* conf, netmgr_event_cb cb, void* user)
{
    int want = 0;  /* 配置中启用的链路数量 */
    int fail = 0;  /* 拉起失败的链路数量 */
    int err = 0;   /* 最近一次失败的错误码 */
    int ret = 0;   /* 子步骤返回值 */
    int i = 0;     /* 链路下标，从 0 开始 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    pthread_mutex_lock(&g_core.lock);
    if (g_core.started)
    {
        pthread_mutex_unlock(&g_core.lock);
        return -NETMGRERR_STATE;
    }

    /* 初始化三条链路上下文，未启用的链路保持 DISABLED；rmetric 用 -1 表示尚未下发路由 */
    for (i = 0; i < NETMGR_TYPE_MAX; ++i)
    {
        memset(&g_core.link[i], 0, sizeof(NETMGR_CTX));
        g_core.link[i].type = (NETMGR_TYPE)i;
        g_core.link[i].state = NETMGR_ST_DISABLED;
        g_core.link[i].prev = NETMGR_ST_DISABLED;
        g_core.link[i].rmetric = -1;
    }
    g_core.active = -1;
    g_core.cb = cb;
    g_core.user = user;
    g_core.dns[0] = '\0';

    /* 保存配置副本，运行期链路开关按此副本重新拉起 */
    g_core.ethConf = conf->eth;
    g_core.wifiConf = conf->wifi;
    g_core.cellConf = conf->cell;
    pthread_mutex_unlock(&g_core.lock);

    want = (conf->eth.enable ? 1 : 0) + (conf->wifi.enable ? 1 : 0) + (conf->cell.enable ? 1 : 0);
    if (0 == want)
    {
        return -NETMGRERR_CONF;
    }

    /* 装配顺序：先建事件监听，再拉起链路，保证取址过程中的事件不丢失 */
    ret = netmgr_event_open(core_event_cb, NULL, NETMGR_EVENT_GROUPS_DEFAULT);
    if (ret < 0)
        return ret;

    ret = core_link_open(NETMGR_ETH, &conf->eth, conf->eth.enable, conf->eth.prio);
    if (ret < 0)
    {
        ++fail;
        err = ret;
    }
    ret = core_link_open(NETMGR_WIFI, &conf->wifi, conf->wifi.enable, conf->wifi.prio);
    if (ret < 0)
    {
        ++fail;
        err = ret;
    }
    ret = core_link_open(NETMGR_CELL, &conf->cell, conf->cell.enable, conf->cell.prio);
    if (ret < 0)
    {
        ++fail;
        err = ret;
    }

    /* 单条链路失败不阻断其它链路，全部失败才判定启动失败 */
    if (fail >= want)
    {
        (void)core_links_stop();
        (void)netmgr_event_close();
        return err;
    }

    g_core.started = 1;

    /* 立即执行一轮，让静态配置链路无需等待业务轮询即可就绪；
       首轮失败只影响个别链路，启动本身已成功，错误由后续轮次自愈 */
    (void)core_tick();
    return 0;
}

int netmgr_core_stop(void)
{
    int err = 0; /* 首个错误码，0 表示全部步骤成功 */
    int ret = 0; /* 子步骤返回值 */

    pthread_mutex_lock(&g_core.lock);
    if (!g_core.started)
    {
        pthread_mutex_unlock(&g_core.lock);
        return -NETMGRERR_STATE;
    }
    pthread_mutex_unlock(&g_core.lock);

    /* 第一步：停事件监听，之后不会再有事件回调并发进入仲裁 */
    ret = netmgr_event_close();
    if (ret < 0 && 0 == err)
        err = ret;

    /* 第二步：撤路由并关闭链路 */
    ret = core_links_stop();
    if (ret < 0 && 0 == err)
        err = ret;

    pthread_mutex_lock(&g_core.lock);
    g_core.started = 0;
    g_core.cb = NULL;
    g_core.user = NULL;
    pthread_mutex_unlock(&g_core.lock);
    return err;
}

int netmgr_core_poll(void)
{
    pthread_mutex_lock(&g_core.lock);
    if (!g_core.started)
    {
        pthread_mutex_unlock(&g_core.lock);
        return -NETMGRERR_STATE;
    }
    pthread_mutex_unlock(&g_core.lock);

    return core_tick();
}

int netmgr_core_prio_get(NETMGR_TYPE type, unsigned char* prio)
{
    if ((int)type < 0 || (int)type >= NETMGR_TYPE_MAX || NULL == prio)
    {
        return -NETMGRERR_PARAM;
    }

    pthread_mutex_lock(&g_core.lock);
    *prio = g_core.link[type].prio;
    pthread_mutex_unlock(&g_core.lock);
    return 0;
}

int netmgr_core_stat_get(NETMGR_LINK* list, int* num)
{
    NETMGR_LINK* item = NULL; /* 当前写入的快照项 */
    int cap = 0;             /* 数组容量 */
    int cnt = 0;             /* 实际写入条数 */
    int i = 0;               /* 链路下标，从 0 开始 */

    if (NULL == list || NULL == num || *num <= 0)
    {
        return -NETMGRERR_PARAM;
    }
    cap = *num;

    pthread_mutex_lock(&g_core.lock);
    for (i = 0; i < NETMGR_TYPE_MAX && cnt < cap; ++i)
    {
        item = &list[cnt];
        memset(item, 0, sizeof(NETMGR_LINK));
        core_conv(&g_core.link[i], item);
        if (i == g_core.active && NETMGR_ST_READY == g_core.link[i].state)
            item->state = NETMGR_ST_ACTIVE;
        ++cnt;
    }
    pthread_mutex_unlock(&g_core.lock);

    *num = cnt;
    return 0;
}

int netmgr_core_prio_set(NETMGR_TYPE type, unsigned char prio)
{
    if ((int)type < 0 || (int)type >= NETMGR_TYPE_MAX)
    {
        return -NETMGRERR_PARAM;
    }

    pthread_mutex_lock(&g_core.lock);
    if (!g_core.started)
    {
        pthread_mutex_unlock(&g_core.lock);
        return -NETMGRERR_STATE;
    }
    g_core.link[type].prio = prio;
    pthread_mutex_unlock(&g_core.lock);

    /* 与其它显式操作一致立即跑一轮仲裁：主备改选与默认路由 metric 更新随返回即可见，
       不再等业务的下一次 poll */
    (void)core_tick();
    return 0;
}

int netmgr_core_eth_set(const NETMGR_ETH_CONF* conf)
{
    int ret = 0; /* 装配结果 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    ret = core_link_reset(NETMGR_ETH, conf, conf->enable, conf->prio);

    /* 无论装配成败都保存最新配置，后续开关按它重新拉起 */
    pthread_mutex_lock(&g_core.lock);
    g_core.ethConf = *conf;
    pthread_mutex_unlock(&g_core.lock);

    /* 重配属显式操作，立即跑一轮仲裁让状态与事件同步生效 */
    (void)core_tick();
    return ret;
}

int netmgr_core_wifi_set(const NETMGR_WIFI_CONF* conf)
{
    int ret = 0; /* 装配结果 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    ret = core_link_reset(NETMGR_WIFI, conf, conf->enable, conf->prio);

    /* 无论装配成败都保存最新配置，后续开关按它重新拉起 */
    pthread_mutex_lock(&g_core.lock);
    g_core.wifiConf = *conf;
    pthread_mutex_unlock(&g_core.lock);

    /* 重配属显式操作，立即跑一轮仲裁让状态与事件同步生效 */
    (void)core_tick();
    return ret;
}

int netmgr_core_cell_set(const NETMGR_CELL_CONF* conf)
{
    int ret = 0; /* 装配结果 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    ret = core_link_reset(NETMGR_CELL, conf, conf->enable, conf->prio);

    /* 无论装配成败都保存最新配置，后续开关按它重新拉起 */
    pthread_mutex_lock(&g_core.lock);
    g_core.cellConf = *conf;
    pthread_mutex_unlock(&g_core.lock);

    /* 重配属显式操作，立即跑一轮仲裁让状态与事件同步生效 */
    (void)core_tick();
    return ret;
}

int netmgr_core_link_enable(NETMGR_TYPE type, int on)
{
    NETMGR_ETH_CONF eth = {0};   /* 以太网配置副本 */
    NETMGR_WIFI_CONF wifi = {0}; /* WiFi 配置副本 */
    NETMGR_CELL_CONF cell = {0}; /* 4G 配置副本 */
    int started = 0;             /* 1=服务已启动 */
    int ret = 0;                 /* 链路重装结果 */

    if ((int)type < 0 || (int)type >= NETMGR_TYPE_MAX)
        return -NETMGRERR_PARAM;

    /* 配置副本在锁内拷出，避免与并发的 set 交叉；链路重装本身在 reset 内加锁 */
    pthread_mutex_lock(&g_core.lock);
    started = g_core.started;
    eth = g_core.ethConf;
    wifi = g_core.wifiConf;
    cell = g_core.cellConf;
    pthread_mutex_unlock(&g_core.lock);

    if (!started)
        return -NETMGRERR_STATE;

    switch (type)
    {
    case NETMGR_ETH:
        ret = core_link_reset(NETMGR_ETH, &eth, on, eth.prio);
        break;
    case NETMGR_WIFI:
        ret = core_link_reset(NETMGR_WIFI, &wifi, on, wifi.prio);
        break;
    case NETMGR_CELL:
        ret = core_link_reset(NETMGR_CELL, &cell, on, cell.prio);
        break;
    default:
        return -NETMGRERR_PARAM;
    }

    /* 开关属于显式操作，立即跑一轮仲裁让状态与事件同步生效 */
    (void)core_tick();
    return ret;
}

int netmgr_core_dns_set(const char* dns)
{
    int ret = 0; /* 写入结果 */

    if (NULL != dns && '\0' != dns[0] && !netmgr_ip_valid(dns))
    {
        return -NETMGRERR_PARAM;
    }

    pthread_mutex_lock(&g_core.lock);
    ret = netmgr_dns_apply(dns);
    if (ret < 0)
    {
        pthread_mutex_unlock(&g_core.lock);
        return ret;
    }
    snprintf(g_core.dns, sizeof(g_core.dns), "%s", (NULL == dns) ? "" : dns);
    pthread_mutex_unlock(&g_core.lock);
    return 0;
}

int netmgr_core_wifi_scan(NETMGR_WIFI_AP* list, int* num)
{
    CORE_EV_ITEM item = {0}; /* 待分发事件 */
    int count = 0;           /* 实际写入条数，扫描失败时保持 0 */
    int ret = 0;             /* 扫描结果 */

    if (NULL == list || NULL == num || *num <= 0)
        return -NETMGRERR_PARAM;

    ret = netmgr_wpa_scan(0);

    /* 无论成功与否都通知业务：complete 标记本轮是否正常拿到结果 */
    memset(&item, 0, sizeof(item));
    item.ev = NETMGR_EV_WIFI_SCAN_DONE;
    item.u.scan.complete = (ret < 0) ? 0 : 1;
    if (0 == ret && 0 == netmgr_wpa_scan_result(list, &count))
        item.u.scan.apNum = count;
    *num = count;
    core_dispatch(&item);
    return ret;
}

int netmgr_core_wifi_scan_result(NETMGR_WIFI_AP* list, int* num)
{
    return netmgr_wpa_scan_result(list, num);
}
