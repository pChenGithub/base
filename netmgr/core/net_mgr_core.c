/**
 * @file net_mgr_core.c
 * @brief netmgr 仲裁层实现：链路装配、优先级仲裁、默认路由同步与事件分发
 * @details 仲裁模型：
 *          1) 周期性（或事件到达时提前）刷新各链路自身状态 DOWN/READY；
 *          2) 就绪链路按其优先级下发默认路由，metric 取优先级值，由内核选路；
 *          3) 就绪链路中优先级最高者标记为 ACTIVE，仅用于状态上报与 DNS 下发。
 */
#include "net_mgr_core.h"

#include "net_mgr_cell.h"
#include "net_mgr_dns.h"
#include "net_mgr_err.h"
#include "net_mgr_eth.h"
#include "net_mgr_event.h"
#include "net_mgr_route.h"
#include "net_mgr_util.h"
#include "net_mgr_wifi.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

/** 链路状态刷新周期（毫秒），netlink 事件到达时提前唤醒 */
#define NETMGR_TICK_MS  200
/** 管理线程等待周期（毫秒） */
#define CORE_TICK_MS    NETMGR_TICK_MS
/** 毫秒转纳秒的换算系数 */
#define NS_PER_MS       1000000L
/** 一纳秒进位阈值（10^9） */
#define CORE_NS_SEC     1000000000L
/** 路由 metric 基准值，叠加优先级后作为最终 metric */
#define NETMGR_METRIC_BASE  0

/** 管理核心上下文，承载仲裁层全部运行状态 */
typedef struct {
    volatile int running;                   /**< 1=管理线程运行中 */
    int started;                            /**< 1=服务已启动 */
    int active;                             /**< 当前生效链路索引，-1 表示无 */
    pthread_t tid;                          /**< 管理线程 */
    netmgr_ev_cb cb;                        /**< 事件回调，可为空 */
    char dns[NETMGR_IP_LEN];                /**< 当前已下发的 DNS */
    pthread_mutex_t lock;                   /**< 链路上下文互斥锁 */
    pthread_mutex_t wait;                   /**< 管理线程等待锁，与 cond 配对 */
    pthread_cond_t cond;                    /**< 管理线程等待条件 */
    const NETMGR_OPS* ops[NETMGR_TYPE_MAX]; /**< 链路类型到操作表的映射，下标与 NETMGR_TYPE 对齐 */
    NETMGR_CTX link[NETMGR_TYPE_MAX];       /**< 三条链路上下文 */
} NETMGR_CORE;

/**
 * 管理核心上下文（模块内单实例，仲裁层状态全部收敛于此）
 * 用途：保存三条链路运行态、当前生效链路、事件回调、已下发 DNS 与同步原语
 * 单位/范围：active 取 -1 或 0~2 的链路下标；prio 取 0~255；running 取 0/1
 * 默认值依据：静态零初始化，start 时显式重建（active=-1 表示无生效链路）；同步原语用静态初始化器
 * 写入方/读取方：仲裁线程与各配置接口在 g_core.lock 保护下读写
 * 生命周期：仅内存态，start/stop 管理，掉电不保存
 */
static NETMGR_CORE g_core = {
    .ops = {&netmgr_eth_ops, &netmgr_wifi_ops, &netmgr_cell_ops},
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .wait = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

/**
 * @brief 唤醒管理线程提前执行一轮仲裁
 */
static void core_wake(void)
{
    pthread_mutex_lock(&g_core.wait);
    pthread_cond_signal(&g_core.cond);
    pthread_mutex_unlock(&g_core.wait);
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
}

/**
 * @brief 上报一次链路事件
 * @param ev  事件类型
 * @param ctx 触发事件的链路上下文，不可为空
 */
static void core_notify(NETMGR_EV ev, const NETMGR_CTX* ctx)
{
    NETMGR_LINK snapshot = {0}; /* 传给应用的状态快照 */

    if (NULL == ctx || NULL == g_core.cb)
        return;

    core_conv(ctx, &snapshot);
    /* 与当前生效链路一致时对外呈现 ACTIVE，便于应用直接区分主链路 */
    if (g_core.active >= 0 && g_core.active < NETMGR_TYPE_MAX &&
        &g_core.link[g_core.active] == ctx && NETMGR_ST_READY == ctx->state)
    {
        snapshot.state = NETMGR_ST_ACTIVE;
    }

    (void)g_core.cb(ev, &snapshot);
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
 * @brief 执行一轮仲裁：刷新状态、同步路由、选举主链路、上报事件
 * @return 0 全部步骤成功；负值为本轮首个失败步骤的错误码
 */
static int core_tick(void)
{
    NETMGR_CTX* ctx = NULL; /* 当前遍历的链路上下文 */
    NETMGR_EV ev = NETMGR_EV_UP; /* 待上报事件 */
    int old_active = -1;    /* 上一轮生效链路 */
    int new_active = -1;    /* 本轮生效链路 */
    int err = 0;            /* 本轮首个错误码，0 表示尚无失败 */
    int ret = 0;            /* 子步骤返回值 */
    int i = 0;              /* 链路下标，从 0 开始 */

    pthread_mutex_lock(&g_core.lock);
    old_active = g_core.active;

    /* 第一阶段：逐条刷新链路自身状态，并同步其默认路由 */
    for (i = 0; i < NETMGR_TYPE_MAX; ++i)
    {
        ctx = &g_core.link[i];
        if (!ctx->enable || NULL == g_core.ops[i])
            continue;
        ret = g_core.ops[i]->refresh(ctx);
        if (ret < 0)
        {
            /* 单条链路刷新失败不阻断其它链路，仅记录首个错误码 */
            if (0 == err)
                err = ret;
            continue;
        }
        ret = core_route_sync(ctx);
        if (ret < 0 && 0 == err)
            err = ret;

        /* 状态翻转才通知应用，避免周期刷新造成重复上报 */
        if (ctx->prev != ctx->state)
        {
            ev = (NETMGR_ST_READY == ctx->state) ? NETMGR_EV_UP : NETMGR_EV_DOWN;
            ctx->prev = ctx->state;
            core_notify(ev, ctx);
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

    /* 第三阶段：生效链路变化时先更新 DNS，再上报主链路切换 */
    if (old_active != new_active)
    {
        ret = core_dns_sync();
        if (ret < 0 && 0 == err)
            err = ret;
        if (new_active >= 0)
            core_notify(NETMGR_EV_SWITCH, &g_core.link[new_active]);
    }

    pthread_mutex_unlock(&g_core.lock);
    return err;
}

/**
 * @brief 管理线程主循环：定时与事件双触发
 * @param arg 未使用
 * @return 恒定返回空指针
 */
static void* core_thread(void* arg)
{
    struct timespec ts = {0}; /* 本轮等待截止时间 */

    (void)arg;
    while (1)
    {
        pthread_mutex_lock(&g_core.wait);
        if (!g_core.running)
        {
            pthread_mutex_unlock(&g_core.wait);
            break;
        }

        /* 每轮重新计算截止时间，事件唤醒后下一轮仍按完整周期等待 */
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)CORE_TICK_MS * NS_PER_MS;
        if (ts.tv_nsec >= CORE_NS_SEC)
        {
            ts.tv_sec += 1;
            ts.tv_nsec -= CORE_NS_SEC;
        }
        (void)pthread_cond_timedwait(&g_core.cond, &g_core.wait, &ts);
        pthread_mutex_unlock(&g_core.wait);

        /* 线程内无返回通道，本轮错误码由周期仲裁自行收敛 */
        (void)core_tick();
    }
    return NULL;
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
        ctx->state = NETMGR_ST_DOWN;
        ctx->prev = NETMGR_ST_DOWN;
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
        ctx->state = NETMGR_ST_DOWN;
        ctx->prev = NETMGR_ST_DOWN;
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
        ctx->state = NETMGR_ST_DOWN;
        ctx->prev = NETMGR_ST_DOWN;
        pthread_mutex_unlock(&g_core.lock);
        return err;
    }

    /* 第三步：按新配置拉起，失败时保持停用状态并上报错误 */
    ctx->prio = prio;
    ctx->enable = 1;
    ret = g_core.ops[type]->open(ctx, conf);
    if (ret < 0)
    {
        ctx->enable = 0;
        ctx->state = NETMGR_ST_DOWN;
        ctx->prev = NETMGR_ST_DOWN;
        pthread_mutex_unlock(&g_core.lock);
        return ret;
    }
    pthread_mutex_unlock(&g_core.lock);
    return err;
}

/**
 * @brief 启动仲裁核心，按配置拉起全部启用的链路并开始周期仲裁
 * @param conf 总配置，不可为空
 * @param cb   链路状态回调，可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_start(const NETMGR_CONF* conf, netmgr_ev_cb cb)
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

    /* 初始化三条链路上下文，rmetric 用 -1 表示尚未下发路由 */
    for (i = 0; i < NETMGR_TYPE_MAX; ++i)
    {
        memset(&g_core.link[i], 0, sizeof(NETMGR_CTX));
        g_core.link[i].type = (NETMGR_TYPE)i;
        g_core.link[i].state = NETMGR_ST_DOWN;
        g_core.link[i].prev = NETMGR_ST_DOWN;
        g_core.link[i].rmetric = -1;
    }
    g_core.active = -1;
    g_core.cb = cb;
    g_core.dns[0] = '\0';
    pthread_mutex_unlock(&g_core.lock);

    want = (conf->eth.enable ? 1 : 0) + (conf->wifi.enable ? 1 : 0) + (conf->cell.enable ? 1 : 0);
    if (0 == want)
    {
        return -NETMGRERR_CONF;
    }

    /* 装配顺序：先建事件监听，再拉起链路，保证取址过程中的事件不丢失 */
    ret = netmgr_event_open(core_wake);
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
    g_core.running = 1;
    if (0 != pthread_create(&g_core.tid, NULL, core_thread, NULL))
    {
        g_core.running = 0;
        g_core.started = 0;
        (void)core_links_stop();
        (void)netmgr_event_close();
        return -NETMGRERR_THREAD;
    }

    /* 立即执行一轮，让静态配置链路无需等待首个周期即可就绪；
       首轮失败只影响个别链路，启动本身已成功，错误由后续轮次自愈 */
    (void)core_tick();
    return 0;
}

/**
 * @brief 停止仲裁核心，关闭全部链路并删除本模块下发的默认路由
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
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

    /* 第一步：停管理线程，后续拆除流程不再有并发刷新 */
    pthread_mutex_lock(&g_core.wait);
    g_core.running = 0;
    pthread_cond_signal(&g_core.cond);
    pthread_mutex_unlock(&g_core.wait);
    if (0 != pthread_join(g_core.tid, NULL))
        err = -NETMGRERR_THREAD;

    /* 第二步：停事件监听，避免拆除期间被反复唤醒 */
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
    pthread_mutex_unlock(&g_core.lock);
    return err;
}

/**
 * @brief 读取指定类型链路的当前优先级
 * @param type 链路类型
 * @param prio 输出当前优先级，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
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

/**
 * @brief 查询全部链路的状态快照
 * @param list 输出数组，容量由 *num 传入
 * @param num  入参为数组容量，出参为实际写入条数
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
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

/**
 * @brief 运行时修改指定类型链路的优先级
 * @param type 链路类型
 * @param prio 新优先级（0-255，小者优先）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
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

    /* 立即唤醒仲裁，使路由 metric 尽快与新优先级一致 */
    core_wake();
    return 0;
}

/**
 * @brief 重新配置以太网链路
 * @param conf 以太网配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_eth_set(const NETMGR_ETH_CONF* conf)
{
    int ret = 0; /* 装配结果 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    ret = core_link_reset(NETMGR_ETH, conf, conf->enable, conf->prio);
    core_wake();
    return ret;
}

/**
 * @brief 重新配置 WiFi 链路
 * @param conf WiFi 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_wifi_set(const NETMGR_WIFI_CONF* conf)
{
    int ret = 0; /* 装配结果 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    ret = core_link_reset(NETMGR_WIFI, conf, conf->enable, conf->prio);
    core_wake();
    return ret;
}

/**
 * @brief 重新配置 4G 链路
 * @param conf 4G 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_cell_set(const NETMGR_CELL_CONF* conf)
{
    int ret = 0; /* 装配结果 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    ret = core_link_reset(NETMGR_CELL, conf, conf->enable, conf->prio);
    core_wake();
    return ret;
}

/**
 * @brief 下发 DNS 服务器地址
 * @param dns 单个 IPv4 地址字符串，为空或空串表示清除已下发的 DNS
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
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
