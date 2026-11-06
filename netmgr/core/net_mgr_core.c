/**
 * @file net_mgr_core.c
 * @brief netmgr 仲裁层实现：对外接口、链路生命周期与事件分发
 * @details 本层是「编排者」，仲裁机制本身独立在 net_mgr_arbiter.c（见该文件头五阶段说明）：
 *          1) 三条链路的 open/close/refresh 与就绪判定在本层内部（net_mgr_eth/wifi/cell/link.c）；
 *          2) 本层起 1 个定时线程周期驱动仲裁（间隔 NETMGR_CONF.tickMs，默认 500ms），
 *             与 netlink 事件、DHCP 回调、配置接口共同构成四类驱动源（见 core_drive 上方说明）；
 *          3) 一轮仲裁的入口是本文件的 core_drive()：加锁 → 调 netmgr_arb_run()
 *             → 解锁后分发事件；仲裁内部不启停进程、不调用业务回调；
 *          4) 就绪链路按其优先级下发默认路由，metric = NETMGR_METRIC_BASE + prio，
 *             就绪链路中优先级最高者标记为 ACTIVE（仅用于状态上报与 DNS 下发）。
 */
#include "net_mgr_core.h"

#include "net_mgr_core_int.h"
#include "net_mgr_arbiter.h"

#include "net_mgr_dhcp.h"
#include "net_mgr_dns.h"
#include "net_mgr_err.h"
#include "net_mgr_event.h"
#include "net_mgr_route.h"
#include "net_mgr_util.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/**
 * 仲裁驱动源：标明“这一轮仲裁是被谁拉起的”
 * 用途：所有驱动点统一写成 core_drive(CORE_DRIVE_xxx)，
 *       阅读时无需追调用栈即可知道触发原因；也为将来排查“状态为何没更新”留出追踪锚点
 * 单位/范围：枚举值，仅作标识，不参与逻辑判断
 */
typedef enum {
    CORE_DRIVE_START = 0, /**< 服务启动：完成链路装配后的首轮 */
    CORE_DRIVE_EVENT,     /**< netlink 事件：插拔、地址、路由变化 */
    CORE_DRIVE_TIMER,     /**< 定时线程到期：兜住“无事件”变化（AP 进程退出等） */
    CORE_DRIVE_CONFIG     /**< 业务显式改配置：*_set / 链路开关 / 改优先级 */
} CORE_DRIVE;

/** 管理核心上下文，承载仲裁层全部运行状态 */
typedef struct {
    int started;                            /**< 1=服务已启动 */
    NETMGR_ARB arb;                         /**< 仲裁器状态：主链路选举结果 + 已下发 DNS */
    pthread_t tickTid;                      /**< 定时驱动线程（tickMs>0 时创建） */
    pthread_cond_t tickCond;                /**< 定时线程等待条件（绑定单调时钟，启动时初始化） */
    int tickOn;                             /**< 1=定时线程运行中 */
    int tickMs;                             /**< 定时驱动间隔（毫秒），0 表示未启用 */
    netmgr_event_cb cb;                     /**< 事件回调，可为空 */
    void* user;                             /**< 事件回调的用户指针，可为空 */
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
static int core_drive(CORE_DRIVE src);

/**
 * @brief 定时驱动线程：每隔 tickMs 跑一轮仲裁
 * @details 存在的唯一理由是覆盖「既无 netlink 事件、也无会话回调」的变化
 *          （典型：AP 模式下 hostapd 进程意外退出）。等待用条件变量而非 sleep，
 *          stop 时 broadcast 可立即唤醒，不必等满一个间隔。实际间隔 ≈ tickMs + 本轮
 *          仲裁耗时（毫秒级）；条件变量绑定单调时钟，系统时间跳变不影响计时
 * @param arg 未使用
 * @return 固定返回 NULL
 */
static void* core_tick_thread(void* arg)
{
    struct timespec ts = {0}; /* 下次到期时刻 */
    int tickMs = 0;           /* 本轮使用的间隔（锁内读取） */
    int running = 0;          /* 1=继续运行 */

    (void)arg;

    while (1)
    {
        pthread_mutex_lock(&g_core.lock);
        running = g_core.tickOn;
        tickMs = g_core.tickMs;
        if (0 != running && tickMs > 0)
        {
            /* 用单调时钟：系统时间被 NTP 回拨/前跳都不会让等待时长错乱 */
            (void)clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += tickMs / 1000;
            ts.tv_nsec += (long)(tickMs % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L)
            {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            /* 等待期间自动释放锁；被 stop 广播唤醒时 running 已为 0 */
            (void)pthread_cond_timedwait(&g_core.tickCond, &g_core.lock, &ts);
            running = g_core.tickOn;
        }
        pthread_mutex_unlock(&g_core.lock);

        if (0 == running)
            break;

        (void)core_drive(CORE_DRIVE_TIMER);
    }
    return NULL;
}

/**
 * @brief 按配置启动/停止定时驱动线程
 * @param tickMs >0 启动（已启动则更新间隔）；0 停止
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int core_tick_ctrl(int tickMs)
{
    pthread_condattr_t attr; /* 条件变量属性，用于绑定单调时钟 */
    int needJoin = 0;        /* 1=需要等待线程退出 */
    int condErr = 0;         /* 条件变量初始化结果 */

    pthread_mutex_lock(&g_core.lock);
    g_core.tickMs = tickMs;

    if (tickMs > 0 && 0 == g_core.tickOn)
    {
        /* 条件变量必须绑定 CLOCK_MONOTONIC：pthread_cond_timedwait 默认按实时钟计时，
           系统时间被回拨会让一次等待变成几十秒，定时驱动随之失效 */
        (void)pthread_condattr_init(&attr);
        condErr = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        if (0 == condErr)
            condErr = pthread_cond_init(&g_core.tickCond, &attr);
        (void)pthread_condattr_destroy(&attr);

        if (0 != condErr)
        {
            g_core.tickMs = 0;
            pthread_mutex_unlock(&g_core.lock);
            return -NETMGRERR_THREAD;
        }

        g_core.tickOn = 1;
        if (0 != pthread_create(&g_core.tickTid, NULL, core_tick_thread, NULL))
        {
            (void)pthread_cond_destroy(&g_core.tickCond);
            g_core.tickOn = 0;
            g_core.tickMs = 0;
            pthread_mutex_unlock(&g_core.lock);
            return -NETMGRERR_THREAD;
        }
    }
    else if (tickMs <= 0 && 0 != g_core.tickOn)
    {
        g_core.tickOn = 0;
        (void)pthread_cond_broadcast(&g_core.tickCond);
        needJoin = 1;
    }
    pthread_mutex_unlock(&g_core.lock);

    if (0 != needJoin)
    {
        (void)pthread_join(g_core.tickTid, NULL);
        (void)pthread_cond_destroy(&g_core.tickCond);
    }
    return 0;
}

/**
 * @brief 上报一次默认路由变化事件
 * @param nev 触发变化的 netlink 路由事件，不可为空
 */
static void core_report_route(const NETMGR_NEV* nev);

/**
 * @brief 判断事件相关接口是否属于本模块已启用的链路
 * @details 无关接口（lo、容器桥、蓝牙等）的链路/地址事件不触发仲裁刷新，
 *          避免网络抖动时每来一条事件就跑一轮全量刷新
 * @param ifname 事件接口名，不可为空
 * @return 1 命中已启用链路；0 与本模块无关
 */
static int core_link_match(const char* ifname)
{
    int found = 0; /* 1=命中 */
    int i = 0;     /* 链路下标，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0])
        return 0;

    pthread_mutex_lock(&g_core.lock);
    for (i = 0; i < NETMGR_TYPE_MAX; ++i)
    {
        if (!g_core.link[i].enable)
            continue;
        if (0 != strcmp(g_core.link[i].ifname, ifname))
            continue;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_core.lock);
    return found;
}

/**
 * @brief 判断默认路由事件是否由本模块下发（回环抑制）
 * @details 本模块下发或撤销默认路由时，内核会回吐一条路由事件；若其 metric 与网关
 *          与本模块记录一致，则视为自环，不再作为“外部路由变化”上报业务
 * @note 属启发式判定：外部添加与模块完全相同的 metric + 网关时会被一并抑制；
 *       要精确区分需给 NETMGR_NEV 增加路由 protocol 字段（接口变更，暂无必要）
 * @param ev 路由事件，不可为空
 * @return 1 本模块自身动作；0 外部改写
 */
static int core_route_self(const NETMGR_NEV* ev)
{
    const NETMGR_CTX* ctx = NULL; /* 命中链路上下文 */
    int self = 0;                 /* 1=自身动作 */
    int i = 0;                    /* 链路下标，从 0 开始 */

    if (NULL == ev || '\0' == ev->ifname[0])
        return 0;

    pthread_mutex_lock(&g_core.lock);
    for (i = 0; i < NETMGR_TYPE_MAX; ++i)
    {
        ctx = &g_core.link[i];
        if (0 != strcmp(ctx->ifname, ev->ifname))
            continue;
        if (ctx->rmetric == ev->u.route.metric && 0 == strcmp(ctx->rgate, ev->u.route.gate))
            self = 1;
        break;
    }
    pthread_mutex_unlock(&g_core.lock);
    return self;
}

/* ===== netlink 事件处理（运行在 sys 层事件接收线程上下文）=====
 * 处理策略，三层职责清晰划分，避免出现两处事实源：
 *   1) 事件只作「触发器」：链路 / 地址 / LOST / 空事件都不直接改 ctx，
 *      统一由 core_on_refresh_event() 唤醒一轮，由仲裁刷新重读内核；
 *   2) 状态以刷新为准：UP / DOWN / INFO_CHANGE 等全部由 core_drive 比对刷新前后快照产生；
 *   3) 只有默认路由增删需要用到载荷（上报网关与方向，并抑制本模块自身动作造成的回环）；
 *      LOST 与空事件无载荷，与链路/地址事件同走“唤醒入口”，只是不做接口过滤。
 */

/**
 * @brief 唤醒一轮仲裁（所有“只需刷新”事件的唯一入口）
 * @details 链路、地址、LOST、空事件四者的语义完全一致：都不使用载荷，只是提示
 *          「内核状态可能变了」，状态一律由 core_drive 刷新重读，事件本身不改 ctx。
 *          因此不再为每种事件各写一个处理器，统一走本入口。
 *          过滤规则：事件能定位到接口、且该接口不属于本模块（lo、容器桥等）时跳过；
 *          事件为空或接口名解析失败（LOST / DELLINK 取不到名字）视为“无法定位”，
 *          保守刷新——宁可多刷一轮，也不漏掉一次状态变化
 * @param ev 事件内容，可为空（空表示“有变化但无内容”，按保守刷新处理）
 * @return 1 已驱动一轮；0 与本模块无关、未驱动
 */
static int core_on_refresh_event(const NETMGR_NEV* ev)
{
    if (NULL != ev && '\0' != ev->ifname[0] && !core_link_match(ev->ifname))
        return 0;

    (void)core_drive(CORE_DRIVE_EVENT);
    return 1;
}

/**
 * @brief 处理路由事件（ROUTE_ADD / ROUTE_DEL）
 * @details 先刷新（网关可能变化），再把「外部改写的默认路由」上报业务；
 *          本模块自己下发 / 撤销的默认路由属自环，不上报，避免业务收到自己引起的变化
 * @param ev 事件内容，不可为空
 */
static void core_on_route_event(const NETMGR_NEV* ev)
{
    /* 先按“唤醒仲裁”处理（含接口过滤）；无关接口不必再看载荷 */
    if (0 == core_on_refresh_event(ev))
        return;

    if (0 == ev->u.route.isDefault)
        return;
    if (0 != core_route_self(ev))
        return;

    core_report_route(ev);
}

/**
 * @brief netlink 事件回调：按事件类别分派到对应处理函数（策略见上方块注释）
 * @param ev   事件内容，可为空（按 LOST 处理）
 * @param user 未使用
 */
static void core_event_cb(const NETMGR_NEV* ev, void* user)
{
    (void)user;

    /* 空事件（回调约定“按 LOST 处理”）与 LOST 一样：无载荷，只唤醒一轮 */
    if (NULL == ev)
    {
        (void)core_on_refresh_event(NULL);
        return;
    }

    switch (ev->type)
    {
    case NETMGR_NEV_LOST:  /* 缓冲溢出、无法定位接口：保守刷新 */
    case NETMGR_NEV_LINK_UP:
    case NETMGR_NEV_LINK_DOWN:
    case NETMGR_NEV_LINK_DEL:
    case NETMGR_NEV_ADDR_ADD:
    case NETMGR_NEV_ADDR_DEL:
        /* 四类事件都不使用载荷，统一走唤醒入口（LOST 的 ifname 为空 → 保守刷新） */
        (void)core_on_refresh_event(ev);
        break;
    case NETMGR_NEV_ROUTE_ADD:
    case NETMGR_NEV_ROUTE_DEL:
        core_on_route_event(ev);
        break;
    default:
        break;
    }
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

/* ===== 仲裁驱动：谁让仲裁跑起来（阅读入口）=====
 *
 * 仲裁器（net_mgr_arbiter.c）是纯逻辑，自己不会运行；一轮仲裁必须由下列
 * 驱动源之一显式调用 core_drive() 拉起。全文件只有这四类调用点：
 *
 *   驱动源（CORE_DRIVE_*）     调用点                          覆盖的变化
 *   ─────────────────────────────────────────────────────────────────────────
 *   CORE_DRIVE_EVENT    core_on_refresh_event（链路/地址/    插拔、地址、路由、
 *                       LOST/空事件共用同一入口）              事件丢失补刷新
 *                       core_on_route_event（刷新后再上报）
 *   CORE_DRIVE_START    netmgr_core_start                      启动后首轮（静态配置立即就绪）
 *   CORE_DRIVE_CONFIG   *_set / link_enable / prio_set         业务改配置后立即生效
 *   CORE_DRIVE_TIMER    core_tick_thread                       兜住“无通知”的变化
 *                                                              （AP 进程退出等）
 *
 * 注意 DHCP 会话回调（core_dhcp_cb）**不驱动仲裁**：它只把 DHCP 状态变化
 * 作为事件上报，链路状态（地址/网关/READY）由下一轮驱动收敛——通常就是
 * 定时线程（≤ tickMs）或紧接着到达的 netlink 地址事件。
 *
 * core_drive 只做三件事：加锁 → 调仲裁器 → 解锁后分发事件。
 */

/**
 * @brief 拉起一轮仲裁并把产出的事件分发给业务（所有驱动源的唯一入口）
 * @details 仲裁本身的五阶段（刷新/差异/路由/选举/应用）在 net_mgr_arbiter.c，
 *          机制细节见该文件头说明；本函数不判断是否需要跑，何时跑由驱动源决定
 * @param src 驱动源（仅用于阅读定位与后续追踪，不参与逻辑）
 * @return 0 本轮各阶段成功；负值为本轮首个失败步骤的错误码
 */
static int core_drive(CORE_DRIVE src)
{
    NETMGR_EVENT evs[NETMGR_ARB_EV_MAX] = {0}; /* 本轮仲裁产出的事件 */
    int evNum = 0;                             /* 事件条数 */
    int err = 0;                               /* 本轮首个错误码 */
    int i = 0;                                 /* 事件下标，从 0 开始 */

    (void)src; /* 驱动源当前只用于阅读定位；需要追踪时在此挂调试钩子即可 */

    pthread_mutex_lock(&g_core.lock);
    err = netmgr_arb_run(&g_core.arb, g_core.link, g_core.ops, evs, NETMGR_ARB_EV_MAX, &evNum);
    pthread_mutex_unlock(&g_core.lock);

    /* 锁外分发：业务回调内可安全调用本模块的查询与配置接口 */
    for (i = 0; i < evNum; ++i)
        core_dispatch(&evs[i]);
    return err;
}

/**
 * @brief 上报一次默认路由变化事件
 * @param nev 触发变化的 netlink 路由事件，不可为空
 */
static void core_report_route(const NETMGR_NEV* nev)
{
    NETMGR_EVENT item = {0}; /* 待分发事件，值语义：回调期间不访问内部上下文 */
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
            netmgr_arb_snapshot_one(&g_core.arb, g_core.link, i, &snap);
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
    (void)snprintf(item.u.route.gate, sizeof(item.u.route.gate), "%s", nev->u.route.gate);
    core_dispatch(&item);
}

/**
 * @brief DHCP 会话状态回调：状态变化时上报一次业务事件
 * @details 只记录状态与错误码，地址仍由仲裁刷新从接口读取（避免跨线程改写链路上下文）；
 *          常驻会话会重复回调同一状态（如持续失败重试），仅在状态/错误码变化时上报。
 *          快照在锁内取、事件在锁外分发。
 *          **本回调不驱动仲裁**（不调 core_drive）：DHCP 事件只负责“通知”，
 *          链路 READY/DOWN 由下一轮驱动按刷新结果产生，避免在取址线程里
 *          额外跑一遍路由/选举，也让“事件 ≠ 状态”这条边界保持清晰
 * @param state  会话状态
 * @param ifname 接口名，可为空（为空直接忽略）
 * @param err    失败时的错误码（非 FAIL 状态忽略）
 * @param user   链路私有数据的公共头（NETMGR_DHCP_BASE），不可为空
 */
void core_dhcp_cb(NETMGR_DHCP_STATE state, const char* ifname, int err, void* user)
{
    NETMGR_DHCP_BASE* base = (NETMGR_DHCP_BASE*)user; /* 链路私有数据的公共头 */
    NETMGR_EVENT item = {0};                          /* 待分发事件 */
    NETMGR_LINK snap = {0};                           /* 当前链路快照 */
    int failErr = (NETMGR_DHCP_FAIL == state) ? err : 0; /* FAIL 时上抛的错误码 */
    int changed = 0;                                  /* 1=状态或错误码变化 */
    int started = 0;                                  /* 1=服务运行中 */

    if (NULL == base || NULL == ifname)
        return;
    if ((int)base->type < 0 || (int)base->type >= NETMGR_TYPE_MAX)
        return;

    /* 变化判定与状态记录：避免常驻会话反复回调同一状态导致事件刷屏 */
    changed = (0 == base->reported) || ((int)state != (int)base->dhcpState) ||
              (failErr != base->dhcpErr);
    base->reported = 1;
    base->dhcpState = state;
    base->dhcpErr = failErr;
    if (0 == changed)
        return;

    pthread_mutex_lock(&g_core.lock);
    started = g_core.started;
    if (started)
        netmgr_arb_snapshot_one(&g_core.arb, g_core.link, (int)base->type, &snap);
    pthread_mutex_unlock(&g_core.lock);

    /* 服务未启动（回调晚到）时不上报，避免业务收到启动前的事件 */
    if (!started)
        return;

    if (NETMGR_DHCP_RUNNING == state)
        item.ev = NETMGR_EV_DHCP_RUNNING;
    else if (NETMGR_DHCP_SUCC == state)
        item.ev = NETMGR_EV_DHCP_SUCC;
    else if (NETMGR_DHCP_LOST == state)
        item.ev = NETMGR_EV_DHCP_LOST;
    else
        item.ev = NETMGR_EV_DHCP_FAIL;

    item.u.dhcp.ref.type = base->type;
    (void)snprintf(item.u.dhcp.ref.ifname, sizeof(item.u.dhcp.ref.ifname), "%s", snap.ifname);
    item.u.dhcp.err = failErr;
    core_dispatch(&item);
}

/**
 * @brief 撤掉本模块为某条链路下发的默认路由（幂等）
 * @details 链路关闭 / 重新装配 / 全量拆除都需要先撤路由，避免按旧网关转发；
 *          未下发过时直接返回 0，不做任何内核操作
 * @param ctx 链路上下文，不可为空
 * @return 0 成功或无需撤销；负值为 NETMGRERR_* 错误码
 */
static int core_route_uninstall(NETMGR_CTX* ctx)
{
    int ret = 0; /* 撤销结果 */

    if (NULL == ctx || 0 == ctx->route_on)
        return 0;

    ret = netmgr_route_del(ctx->ifname, ctx->rgate, ctx->rmetric);
    ctx->route_on = 0;
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
        ret = core_route_uninstall(ctx);
        if (ret < 0 && 0 == err)
            err = ret;
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
    netmgr_arb_reset(&g_core.arb);
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

    /* started 守卫由调用方（配置接口 / 链路开关）负责：start 首次装配时 started 尚未置位 */
    pthread_mutex_lock(&g_core.lock);

    /* 第一步：撤掉旧路由，避免新旧配置交替期间按旧网关转发 */
    ret = core_route_uninstall(ctx);
    if (ret < 0)
        err = ret;

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
        ctx->err = ret; /* 保留失败原因，随快照与 DISABLED 事件上报 */
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
    netmgr_arb_reset(&g_core.arb);
    g_core.cb = cb;
    g_core.user = user;

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

    ret = core_link_reset(NETMGR_ETH, &conf->eth, conf->eth.enable, conf->eth.prio);
    if (ret < 0)
    {
        ++fail;
        err = ret;
    }
    ret = core_link_reset(NETMGR_WIFI, &conf->wifi, conf->wifi.enable, conf->wifi.prio);
    if (ret < 0)
    {
        ++fail;
        err = ret;
    }
    ret = core_link_reset(NETMGR_CELL, &conf->cell, conf->cell.enable, conf->cell.prio);
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
    (void)core_drive(CORE_DRIVE_START);

    /* 定时驱动是“无事件变化”的唯一兜底，恒定启动；间隔未配置时取默认值 */
    ret = core_tick_ctrl((conf->tickMs > 0) ? conf->tickMs : NETMGR_TICK_MS);
    if (ret < 0)
    {
        (void)core_links_stop();
        (void)netmgr_event_close();
        g_core.started = 0;
        return ret;
    }
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

    /* 第一步：停定时驱动，保证后续拆链路期间不再有仲裁轮次进入 */
    (void)core_tick_ctrl(0);

    /* 第二步：停事件监听，之后不会再有事件回调并发进入仲裁 */
    ret = netmgr_event_close();
    if (ret < 0 && 0 == err)
        err = ret;

    /* 第三步：撤路由并关闭链路 */
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
    int cap = 0; /* 调用方传入的数组容量 */
    int cnt = 0; /* 实际写入条数 */

    if (NULL == list || NULL == num || *num <= 0)
    {
        return -NETMGRERR_PARAM;
    }
    cap = *num;

    pthread_mutex_lock(&g_core.lock);
    cnt = cap; /* 入参是容量，出参是条数：仲裁器按容量截断 */
    netmgr_arb_snapshot(&g_core.arb, g_core.link, list, &cnt);
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

    /* 显式改配置：立即跑一轮，主备改选与路由 metric 更新随返回即可见 */
    (void)core_drive(CORE_DRIVE_CONFIG);
    return 0;
}

/**
 * @brief 应用一条链路的配置：按新配置重建 → 保存副本 → 立即仲裁
 * @details 保存副本的目的：运行期 netmgr_core_link_enable 按上次配置重新拉起；
 *          重建失败同样保存，便于业务修正参数后仅切换开关即生效
 * @param type   链路类型
 * @param conf   配置，不可为空
 * @param enable 1=启用
 * @param prio   优先级
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int core_conf_apply(NETMGR_TYPE type, const void* conf, int enable, unsigned char prio)
{
    int started = 0; /* 1=服务已启动 */
    int ret = 0;     /* 重建结果 */

    pthread_mutex_lock(&g_core.lock);
    started = g_core.started;
    pthread_mutex_unlock(&g_core.lock);
    if (!started)
        return -NETMGRERR_STATE;

    ret = core_link_reset(type, conf, enable, prio);

    /* 无论重建成败都保存最新配置，后续开关按它重新拉起 */
    pthread_mutex_lock(&g_core.lock);
    if (NETMGR_ETH == type)
        g_core.ethConf = *(const NETMGR_ETH_CONF*)conf;
    else if (NETMGR_WIFI == type)
        g_core.wifiConf = *(const NETMGR_WIFI_CONF*)conf;
    else
        g_core.cellConf = *(const NETMGR_CELL_CONF*)conf;
    pthread_mutex_unlock(&g_core.lock);

    /* 显式改配置：立即跑一轮，状态与事件随返回同步 */
    (void)core_drive(CORE_DRIVE_CONFIG);
    return ret;
}

int netmgr_core_eth_set(const NETMGR_ETH_CONF* conf)
{
    if (NULL == conf)
        return -NETMGRERR_PARAM;

    return core_conf_apply(NETMGR_ETH, conf, conf->enable, conf->prio);
}

int netmgr_core_wifi_set(const NETMGR_WIFI_CONF* conf)
{
    if (NULL == conf)
        return -NETMGRERR_PARAM;

    return core_conf_apply(NETMGR_WIFI, conf, conf->enable, conf->prio);
}

int netmgr_core_cell_set(const NETMGR_CELL_CONF* conf)
{
    if (NULL == conf)
        return -NETMGRERR_PARAM;

    return core_conf_apply(NETMGR_CELL, conf, conf->enable, conf->prio);
}

int netmgr_core_link_enable(NETMGR_TYPE type, int on)
{
    NETMGR_ETH_CONF eth = {0};   /* 以太网配置副本（仅 ETH 分支使用） */
    NETMGR_WIFI_CONF wifi = {0}; /* WiFi 配置副本（仅 WIFI 分支使用） */
    NETMGR_CELL_CONF cell = {0}; /* 4G 配置副本（仅 CELL 分支使用） */
    int started = 0;             /* 1=服务已启动 */
    int ret = 0;                 /* 链路重装结果 */

    if ((int)type < 0 || (int)type >= NETMGR_TYPE_MAX)
        return -NETMGRERR_PARAM;

    /* 开关语义＝“按上次保存的配置整链重建”：配置副本在锁内拷出避免与并发 set 交叉，
       按类型只取需要的一份，链路重装本身在 reset 内加锁 */
    pthread_mutex_lock(&g_core.lock);
    started = g_core.started;
    switch (type)
    {
    case NETMGR_ETH:
        eth = g_core.ethConf;
        break;
    case NETMGR_WIFI:
        wifi = g_core.wifiConf;
        break;
    default:
        cell = g_core.cellConf;
        break;
    }
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
    default:
        ret = core_link_reset(NETMGR_CELL, &cell, on, cell.prio);
        break;
    }

    /* 显式改配置：立即跑一轮，开关结果随返回同步 */
    (void)core_drive(CORE_DRIVE_CONFIG);
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
    snprintf(g_core.arb.dns, sizeof(g_core.arb.dns), "%s", (NULL == dns) ? "" : dns);
    pthread_mutex_unlock(&g_core.lock);
    return 0;
}

int netmgr_core_wifi_scan(NETMGR_WIFI_AP* list, int* num)
{
    NETMGR_EVENT item = {0}; /* 待分发事件，值语义：回调期间不访问内部上下文 */
    int cap = 0;             /* 调用方数组容量 */
    int count = 0;           /* 实际写入条数，失败时保持 0 */
    int ret = 0;             /* 扫描结果 */

    if (NULL == list || NULL == num || *num <= 0)
        return -NETMGRERR_PARAM;

    cap = *num;
    ret = netmgr_wpa_scan(0);

    /* 无论成功与否都通知业务：complete 标记本轮是否正常拿到结果 */
    memset(&item, 0, sizeof(item));
    item.ev = NETMGR_EV_WIFI_SCAN_DONE;
    item.u.scan.complete = (ret < 0) ? 0 : 1;

    /* 结果读取的入参是容量（不是 0）：传 0 会被判为参数非法，永远拿不到列表 */
    if (0 == ret)
    {
        count = cap;
        if (0 != netmgr_wpa_scan_result(list, &count))
            count = 0;
        item.u.scan.apNum = count;
    }
    *num = count;
    core_dispatch(&item);
    return ret;
}

int netmgr_core_wifi_scan_result(NETMGR_WIFI_AP* list, int* num)
{
    return netmgr_wpa_scan_result(list, num);
}
