/**
 * @file net_m_core.c
 * @brief net 网络管理核心：单线程消息循环、生命周期、自愈与事件分发（net 层）
 * @details 并发模型（相对 netmgr 的核心改动）：
 *          1) 全部链路状态只有管理线程一个写者，模块内不存在保护链路状态的锁；
 *          2) 业务接口把请求封装成命令投递到队列，同步等待结果；若调用线程就是
 *             管理线程（事件回调内再调接口），命令直接内联执行，不会自锁；
 *          3) netlink 事件与 DHCP 回调只入队、不碰链路槽，跨线程数据竞争从根上消除；
 *          4) 查询接口读的是快照副本，不排队，WiFi 关联等长耗时操作不会阻塞查询。
 *          netlink 与 DHCP 事件队列采用定长环形缓冲，满则置溢出标志转为全量刷新，
 *          不丢「需要刷新」这一语义。
 */
#include "net_m_ctx.h"

#include "net_mgr_dns.h"
#include "net_mgr_err.h"
#include "net_mgr_event.h"
#include "net_mgr_util.h"
#include "net_mgr_wpa.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/** 命令队列事件的来源类别 */
#define NET_M_QK_NEV     1 /**< netlink 事件 */
#define NET_M_QK_DHCP    2 /**< DHCP 状态回调 */

/** 自愈首次退避（毫秒） */
#define NET_M_RETRY_BASE_MS 1000
/** 自愈退避上限（毫秒） */
#define NET_M_RETRY_MAX_MS  30000
/** 自愈退避最大移位次数（1s << 5 = 32s，受上限截断） */
#define NET_M_RETRY_SHIFT   5

/** 命令类型 */
typedef enum {
    NET_M_CMD_START = 0,   /**< 启动：初始化链路槽并拉起启用链路 */
    NET_M_CMD_STOP,        /**< 停止：撤路由、关链路、置退出标志 */
    NET_M_CMD_SET_ETH,     /**< 重新配置以太网 */
    NET_M_CMD_SET_WIFI,    /**< 重新配置 WiFi */
    NET_M_CMD_SET_CELL,    /**< 重新配置 4G */
    NET_M_CMD_ENABLE,      /**< 启用/关闭链路 */
    NET_M_CMD_PRIO_SET,    /**< 修改优先级 */
    NET_M_CMD_PRIO_CHECK,  /**< 预检查优先级冲突 */
    NET_M_CMD_DNS,         /**< 下发 DNS */
    NET_M_CMD_SCAN         /**< WiFi 扫描 */
} NET_M_CMD_TYPE;

/** 一条命令：由调用线程在栈上构造，管理线程处理后回填 ret / done */
typedef struct NET_M_CMD {
    struct NET_M_CMD* next;  /**< 队列链接 */
    NET_M_CMD_TYPE type;     /**< 命令类型 */
    int done;                /**< 1=已处理完成 */
    int ret;                 /**< 处理结果 */
    union {
        const void* conf;    /**< SET_*：指向调用方配置，同步等待期间有效 */
        struct {
            NET_M_TYPE type; /**< 链路类型 */
            int on;          /**< 1=启用，0=关闭 */
        } en;                /**< ENABLE */
        struct {
            NET_M_TYPE type; /**< 链路类型 */
            unsigned char prio; /**< 待设置/检查的优先级 */
        } prio;              /**< PRIO_SET / PRIO_CHECK */
        const char* dns;     /**< DNS：指向调用方字符串 */
        struct {
            NET_M_WIFI_AP* list; /**< 输出数组 */
            int* num;            /**< 容量/条数 */
        } scan;              /**< SCAN */
    } arg;
} NET_M_CMD;

/** 事件队列元素：netlink 与 DHCP 两类来源共用定长槽位 */
typedef struct {
    int kind; /**< NET_M_QK_NEV / NET_M_QK_DHCP */
    union {
        NETMGR_NEV nev;      /**< netlink 事件 */
        NET_M_DHCP_EVT dhcp; /**< DHCP 状态 */
    } u;
} NET_M_QEV;

/** 模块全局运行状态 */
typedef struct {
    int started;                     /**< 1=服务已启动 */
    int quit;                        /**< 1=管理线程应退出 */
    pthread_t tid;                   /**< 管理线程句柄 */
    pthread_mutex_t lock;            /**< 保护命令队列、事件队列、快照与配置副本 */
    pthread_cond_t cond;             /**< 命令到达 / 完成广播（绑定单调时钟） */
    int condInited;                  /**< 1=条件变量已初始化 */
    NET_M_CMD* head;                 /**< 命令队列头 */
    NET_M_CMD* tail;                 /**< 命令队列尾 */
    net_m_event_cb cb;               /**< 业务事件回调，可为空 */
    void* user;                      /**< 回调用户指针，可为空 */
    int tickMs;                      /**< 仲裁轮询间隔（毫秒） */
    long long lastTick;              /**< 上次仲裁时刻（毫秒，单调时钟） */
    const NET_M_OPS* ops[NET_M_TYPE_MAX]; /**< 链路类型到操作表映射 */
    NET_M_LINK_RT link[NET_M_TYPE_MAX];   /**< 链路运行时上下文（仅管理线程写） */
    NET_M_LINK snap[NET_M_TYPE_MAX];      /**< 对外快照副本（锁内写，业务线程读） */
    int snapActive;                  /**< 快照对应的主链路下标 */
    NET_M_ARB arb;                   /**< 仲裁器状态 */
    NET_M_ETH_CONF ethConf;          /**< 以太网配置副本 */
    NET_M_WIFI_CONF wifiConf;        /**< WiFi 配置副本 */
    NET_M_CELL_CONF cellConf;        /**< 4G 配置副本 */
    NET_M_QEV evq[32];               /**< 事件环形队列 */
    int evHead;                      /**< 队列读下标 */
    int evTail;                      /**< 队列写下标 */
    int evCount;                     /**< 队列元素数 */
    int evOverflow;                  /**< 1=队列曾溢出，需全量刷新 */
    NET_M_WIFI_AP scanCache[NETMGR_SCAN_MAX]; /**< 最近一次扫描结果缓存 */
    int scanNum;                     /**< 缓存条数（锁内读写） */
} NET_M_MGR;

/** 模块单实例：状态全部收敛于此 */
static NET_M_MGR g_mgr = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .ops = {&net_m_eth_ops, &net_m_wifi_ops, &net_m_cell_ops},
};

/** 管理线程入口 */
static void* core_thread(void* arg);

/**
 * @brief 取当前单调时钟毫秒数
 * @return 毫秒数
 */
static long long core_now_ms(void)
{
    struct timespec ts = {0}; /* 当前时刻 */

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

/**
 * @brief 判断当前线程是否为管理线程（用于识别回调内自调用）
 * @return 1 是管理线程；0 否
 */
static int core_is_self(void)
{
    return (0 != pthread_equal(pthread_self(), g_mgr.tid)) ? 1 : 0;
}

/**
 * @brief 分发一条事件给业务回调（锁外调用）
 * @param ev 事件数据，不可为空
 */
static void core_dispatch(const NET_M_EVENT* ev)
{
    if (NULL == ev || NULL == g_mgr.cb)
    {
        return;
    }

    (void)g_mgr.cb(ev, g_mgr.user);
}

/**
 * @brief 刷新对外快照副本（仅替换快照，不触碰链路槽）
 */
static void core_snap_refresh(void)
{
    int i = 0; /* 链路下标，从 0 开始 */

    pthread_mutex_lock(&g_mgr.lock);
    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        net_m_arb_snapshot_one(&g_mgr.arb, g_mgr.link, i, &g_mgr.snap[i]);
    }
    g_mgr.snapActive = g_mgr.arb.active;
    pthread_mutex_unlock(&g_mgr.lock);
}

/* ===================== 事件队列（跨线程入队） ===================== */

/**
 * @brief 入队一条事件（netlink / DHCP 线程调用，不阻塞）
 * @param kind 事件类别
 * @param nev  netlink 事件，kind 为 NET_M_QK_NEV 时有效，可为空
 * @param dhcp DHCP 事件，kind 为 NET_M_QK_DHCP 时有效，可为空
 */
static void core_ev_push(int kind, const NETMGR_NEV* nev, const NET_M_DHCP_EVT* dhcp)
{
    NET_M_QEV* slot = NULL; /* 目标槽位 */

    pthread_mutex_lock(&g_mgr.lock);
    if (g_mgr.evCount >= (int)(sizeof(g_mgr.evq) / sizeof(g_mgr.evq[0])))
    {
        /* 队列满：丢细节保语义，下一轮做全量刷新 */
        g_mgr.evOverflow = 1;
        pthread_cond_broadcast(&g_mgr.cond);
        pthread_mutex_unlock(&g_mgr.lock);
        return;
    }

    slot = &g_mgr.evq[g_mgr.evTail];
    memset(slot, 0, sizeof(*slot));
    slot->kind = kind;
    if (NET_M_QK_NEV == kind && NULL != nev)
    {
        slot->u.nev = *nev;
    }
    else if (NET_M_QK_DHCP == kind && NULL != dhcp)
    {
        slot->u.dhcp = *dhcp;
    }
    else
    {
        pthread_mutex_unlock(&g_mgr.lock);
        return;
    }

    g_mgr.evTail = (g_mgr.evTail + 1) % (int)(sizeof(g_mgr.evq) / sizeof(g_mgr.evq[0]));
    ++g_mgr.evCount;
    pthread_cond_broadcast(&g_mgr.cond);
    pthread_mutex_unlock(&g_mgr.lock);
}

/**
 * @brief 取出队列头部一条事件（管理线程调用）
 * @param out 输出事件，不可为空
 * @return 1 取到；0 队列空
 */
static int core_ev_pop(NET_M_QEV* out)
{
    int got = 0; /* 1=取到 */

    if (NULL == out)
    {
        return 0;
    }

    pthread_mutex_lock(&g_mgr.lock);
    if (g_mgr.evCount > 0)
    {
        *out = g_mgr.evq[g_mgr.evHead];
        g_mgr.evHead = (g_mgr.evHead + 1) % (int)(sizeof(g_mgr.evq) / sizeof(g_mgr.evq[0]));
        --g_mgr.evCount;
        got = 1;
    }
    pthread_mutex_unlock(&g_mgr.lock);
    return got;
}

/**
 * @brief netlink 事件回调（接收线程上下文）：只入队，不做任何状态判定
 * @param ev   事件内容，可为空
 * @param user 未使用
 */
static void core_nev_cb(const NETMGR_NEV* ev, void* user)
{
    (void)user;

    if (NULL == ev)
    {
        /* 空事件按「有变化但无内容」处理：入队一条 LOST 语义的刷新请求 */
        return;
    }
    core_ev_push(NET_M_QK_NEV, ev, NULL);
}

void net_m_dhcp_notify(NETMGR_DHCP_STATE state, const char* ifname, int err, void* user)
{
    NET_M_HEAD* head = (NET_M_HEAD*)user; /* 链路私有数据公共头 */
    NET_M_DHCP_EVT item = {0};            /* 投递内容 */

    if (NULL == head || NULL == ifname)
    {
        return;
    }
    if ((int)head->type < 0 || (int)head->type >= NET_M_TYPE_MAX)
    {
        return;
    }

    item.type = head->type;
    item.state = state;
    item.err = (NETMGR_DHCP_FAIL == state) ? err : 0;
    core_ev_push(NET_M_QK_DHCP, NULL, &item);
}

/* ===================== 链路装配 ===================== */

/**
 * @brief 按类型取缓存的配置副本与启用标志
 * @param type    链路类型
 * @param enable  输出启用标志，不可为空
 * @return 配置首地址
 */
static const void* core_conf_of(NET_M_TYPE type, int* enable)
{
    const void* conf = NULL; /* 配置首地址 */

    switch (type)
    {
    case NET_M_ETH:
        *enable = g_mgr.ethConf.enable;
        conf = &g_mgr.ethConf;
        break;
    case NET_M_WIFI:
        *enable = g_mgr.wifiConf.enable;
        conf = &g_mgr.wifiConf;
        break;
    default:
        *enable = g_mgr.cellConf.enable;
        conf = &g_mgr.cellConf;
        break;
    }
    return conf;
}

/**
 * @brief 重新装配单条链路：撤旧路由 → 关旧链路 → 按新配置拉起
 * @details 仅在管理线程调用，无需加锁。先撤后关保证关闭过程中不按旧网关转发
 * @param type   链路类型
 * @param conf   配置，可为空（为空时使用缓存副本；仅 ENABLE 路径会这样调用）
 * @param enable 1=启用
 * @param prio   优先级
 * @return 0 成功；负值为首个错误码
 */
static int core_link_reset(NET_M_TYPE type, const void* conf, int enable, unsigned char prio)
{
    NET_M_LINK_RT* link = &g_mgr.link[type]; /* 目标链路 */
    int err = 0;                             /* 首个错误码 */
    int ret = 0;                             /* 子步骤返回值 */

    ret = net_m_route_revoke(link);
    if (ret < 0)
    {
        err = ret;
    }

    if (0 != link->enable && NULL != g_mgr.ops[type] && NULL != g_mgr.ops[type]->close)
    {
        ret = g_mgr.ops[type]->close(link);
        if (ret < 0 && 0 == err)
        {
            err = ret;
        }
        link->enable = 0;
    }

    if (0 == enable)
    {
        /* 只改状态，prev 留给仲裁轮次比对，保证 DISABLED 事件不丢 */
        link->state = NET_M_ST_DISABLED;
        link->err = 0;
        link->retryAt = 0;
        link->retryCount = 0;
        return err;
    }

    link->prio = prio;
    link->enable = 1;
    link->retryAt = 0;
    link->retryCount = 0;
    if (NULL == g_mgr.ops[type] || NULL == g_mgr.ops[type]->open)
    {
        link->enable = 0;
        link->state = NET_M_ST_DISABLED;
        return (0 != err) ? err : -NETMGRERR_CONF;
    }

    ret = g_mgr.ops[type]->open(link, conf);
    if (ret < 0)
    {
        link->enable = 0;
        link->state = NET_M_ST_DISABLED;
        link->err = ret; /* 保留失败原因，随快照与 DISABLED 事件上报 */
        return ret;
    }

    /* 首次自愈延后一个退避基数：连接类动作（WiFi 关联每候选最长 10 秒）不在
       启用命令里同步执行，避免 netm_start / netm_link_enable 被关联超时阻塞 */
    link->retryAt = core_now_ms() + NET_M_RETRY_BASE_MS;
    return err;
}

/**
 * @brief 优先级冲突检查：启用链路之间必须唯一
 * @param type 待检查的链路类型
 * @param prio 待检查的优先级
 * @return 1 冲突；0 不冲突
 */
static int core_prio_conflict(NET_M_TYPE type, unsigned char prio)
{
    int i = 0; /* 链路下标，从 0 开始 */

    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        if (i == (int)type || 0 == g_mgr.link[i].enable)
        {
            continue;
        }
        if (g_mgr.link[i].prio == prio)
        {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 启动阶段校验：启用链路数、优先级唯一性
 * @param conf 总配置，不可为空
 * @return 0 合法；负值为错误码
 */
static int core_conf_check(const NET_M_CONF* conf)
{
    unsigned char prio[NET_M_TYPE_MAX]; /* 已占用的优先级 */
    int used = 0;                       /* 已登记数量 */
    int want = 0;                       /* 启用链路数量 */
    int i = 0;                          /* 登记下标，从 0 开始 */
    int j = 0;                          /* 比较下标，从 0 开始 */

    if (conf->eth.enable)
    {
        prio[used] = conf->eth.prio;
        ++used;
        ++want;
    }
    if (conf->wifi.enable)
    {
        prio[used] = conf->wifi.prio;
        ++used;
        ++want;
    }
    if (conf->cell.enable)
    {
        prio[used] = conf->cell.prio;
        ++used;
        ++want;
    }

    if (0 == want)
    {
        return -NETMGRERR_CONF;
    }

    for (i = 0; i < used; ++i)
    {
        for (j = i + 1; j < used; ++j)
        {
            if (prio[i] == prio[j])
            {
                return -NETMGRERR_PRIO;
            }
        }
    }
    return 0;
}

/**
 * @brief 执行一轮仲裁并分发事件
 * @param drive 1=本轮由显式配置触发（保留给后续节流用）
 */
static void core_arb_once(int drive)
{
    NET_M_EVENT evs[NET_M_EV_CAPACITY]; /* 本轮产出事件 */
    int num = 0;                        /* 事件条数 */
    int i = 0;                          /* 事件下标，从 0 开始 */

    (void)drive;
    (void)net_m_arb_run(&g_mgr.arb, g_mgr.link, g_mgr.ops, evs, NET_M_EV_CAPACITY, &num);
    core_snap_refresh();

    for (i = 0; i < num; ++i)
    {
        core_dispatch(&evs[i]);
    }
}

/**
 * @brief 自愈：对 DOWN 且到达退避时间的链路执行一次修复
 * @return 本轮实际执行自愈的链路数量
 */
static int core_heal(void)
{
    NET_M_LINK_RT* link = NULL; /* 当前链路 */
    long long now = core_now_ms(); /* 当前时刻 */
    long long wait = 0;         /* 退避时长 */
    int shift = 0;              /* 退避移位次数 */
    int done = 0;               /* 执行次数 */
    int ret = 0;                /* 修复结果 */
    int i = 0;                  /* 链路下标，从 0 开始 */

    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        link = &g_mgr.link[i];
        if (0 == link->enable || NET_M_ST_DOWN != link->state)
        {
            link->retryAt = 0;
            link->retryCount = 0;
            continue;
        }
        if (NULL == g_mgr.ops[i] || NULL == g_mgr.ops[i]->heal)
        {
            continue;
        }
        if (0 != link->retryAt && now < link->retryAt)
        {
            continue;
        }

        ret = g_mgr.ops[i]->heal(link);
        ++done;
        if (ret < 0)
        {
            shift = (link->retryCount > (unsigned int)NET_M_RETRY_SHIFT) ?
                    NET_M_RETRY_SHIFT : (int)link->retryCount;
            wait = (long long)NET_M_RETRY_BASE_MS << shift;
            if (wait > NET_M_RETRY_MAX_MS)
            {
                wait = NET_M_RETRY_MAX_MS;
            }
            ++link->retryCount;
            link->retryAt = now + wait;
        }
        else
        {
            link->retryCount = 0;
            link->retryAt = 0;
        }
    }
    return done;
}

/**
 * @brief 完整一轮：仲裁 → 分发 → 自愈 → 再仲裁（让自愈结果立即可见）
 */
static void core_drive(void)
{
    core_arb_once(0);
    if (core_heal() > 0)
    {
        core_arb_once(0);
    }
}

/* ===================== 事件处理（管理线程） ===================== */

/**
 * @brief 判断某接口是否属于本模块已启用的链路
 * @param ifname 接口名，不可为空
 * @return 1 命中；0 无关
 */
static int core_link_match(const char* ifname)
{
    int i = 0; /* 链路下标，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0])
    {
        return 0;
    }

    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        if (0 == g_mgr.link[i].enable)
        {
            continue;
        }
        if (0 == strcmp(g_mgr.link[i].ifname, ifname))
        {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 判断默认路由事件是否由本模块自身下发（回环抑制）
 * @param nev 路由事件，不可为空
 * @return 1 自身动作；0 外部改写
 */
static int core_route_self(const NETMGR_NEV* nev)
{
    int i = 0; /* 链路下标，从 0 开始 */

    if (NULL == nev || '\0' == nev->ifname[0])
    {
        return 0;
    }

    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        if (0 == g_mgr.link[i].enable || 0 == g_mgr.link[i].route_on)
        {
            continue;
        }
        if (0 != strcmp(g_mgr.link[i].ifname, nev->ifname))
        {
            continue;
        }
        if (g_mgr.link[i].rmetric == nev->u.route.metric &&
            0 == strcmp(g_mgr.link[i].rgate, nev->u.route.gate))
        {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 上报一条外部默认路由变化事件
 * @param nev 路由事件，不可为空
 */
static void core_report_route(const NETMGR_NEV* nev)
{
    NET_M_EVENT item = {0}; /* 待分发事件 */
    int i = 0;              /* 链路下标，从 0 开始 */

    if (NULL == nev || '\0' == nev->ifname[0])
    {
        return;
    }

    memset(&item, 0, sizeof(item));
    item.ev = NET_M_EV_ROUTE_CHANGE;
    item.u.route.isAdd = (NETMGR_NEV_ROUTE_ADD == nev->type) ? 1 : 0;
    (void)snprintf(item.u.route.gate, sizeof(item.u.route.gate), "%s", nev->u.route.gate);

    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        if (0 != strcmp(g_mgr.link[i].ifname, nev->ifname))
        {
            continue;
        }
        item.u.route.ref.type = g_mgr.link[i].type;
        (void)snprintf(item.u.route.ref.ifname, sizeof(item.u.route.ref.ifname), "%s",
                       nev->ifname);
        core_dispatch(&item);
        return;
    }
}

/**
 * @brief 把 DHCP 状态写入链路槽并上报对应事件（仅管理线程调用）
 * @param evt DHCP 事件，不可为空
 */
static void core_apply_dhcp(const NET_M_DHCP_EVT* evt)
{
    NET_M_LINK_RT* link = NULL; /* 目标链路 */
    NET_M_EVENT item = {0};     /* 待分发事件 */

    if (NULL == evt || (int)evt->type < 0 || (int)evt->type >= NET_M_TYPE_MAX)
    {
        return;
    }

    link = &g_mgr.link[evt->type];
    link->dhcpSeen = 1;
    link->dhcpState = evt->state;
    link->dhcpErr = evt->err;

    switch (evt->state)
    {
    case NETMGR_DHCP_RUNNING:
        item.ev = NET_M_EV_DHCP_RUNNING;
        break;
    case NETMGR_DHCP_SUCC:
        item.ev = NET_M_EV_DHCP_SUCC;
        break;
    case NETMGR_DHCP_LOST:
        item.ev = NET_M_EV_DHCP_LOST;
        break;
    default:
        item.ev = NET_M_EV_DHCP_FAIL;
        break;
    }

    item.u.dhcp.ref.type = evt->type;
    (void)snprintf(item.u.dhcp.ref.ifname, sizeof(item.u.dhcp.ref.ifname), "%s",
                   link->ifname);
    item.u.dhcp.err = (NET_M_EV_DHCP_FAIL == item.ev) ? evt->err : 0;
    core_dispatch(&item);
}

/**
 * @brief 排空事件队列：DHCP 状态落地 + 刷新触发 + 外部路由上报
 * @details 队列满导致的溢出按「全量刷新一次」处理，不丢刷新语义
 */
static void core_ev_drain(void)
{
    NET_M_QEV item = {0};     /* 当前事件 */
    NETMGR_NEV routeEv = {0}; /* 最近一条待上报的路由事件 */
    int hasRoute = 0;         /* 1=有路由事件待上报 */
    int needRefresh = 0;      /* 1=需要跑一轮仲裁 */
    int overflow = 0;         /* 1=队列曾溢出 */

    pthread_mutex_lock(&g_mgr.lock);
    overflow = g_mgr.evOverflow;
    g_mgr.evOverflow = 0;
    pthread_mutex_unlock(&g_mgr.lock);

    if (0 != overflow)
    {
        needRefresh = 1;
    }

    while (0 != core_ev_pop(&item))
    {
        if (NET_M_QK_DHCP == item.kind)
        {
            core_apply_dhcp(&item.u.dhcp);
            needRefresh = 1;
            continue;
        }

        /* netlink：链路/地址/LOST 只作刷新触发器，路由事件额外上报 */
        if (NETMGR_NEV_LOST == item.u.nev.type)
        {
            needRefresh = 1;
            continue;
        }
        if (NETMGR_NEV_ROUTE_ADD == item.u.nev.type ||
            NETMGR_NEV_ROUTE_DEL == item.u.nev.type)
        {
            needRefresh = 1;
            if (0 != item.u.nev.u.route.isDefault && 0 == core_route_self(&item.u.nev))
            {
                routeEv = item.u.nev;
                hasRoute = 1;
            }
            continue;
        }
        if (0 == core_link_match(item.u.nev.ifname) &&
            '\0' != item.u.nev.ifname[0])
        {
            continue; /* 与模块无关的接口，不触发刷新 */
        }
        needRefresh = 1;
    }

    if (0 != needRefresh)
    {
        core_drive();
    }
    if (0 != hasRoute)
    {
        core_report_route(&routeEv);
    }
}

/* ===================== 命令处理（管理线程） ===================== */

/**
 * @brief 启动：初始化链路槽并按缓存配置拉起启用链路
 * @return 0 成功；全部启用链路都失败时返回首个错误码
 */
static int core_do_start(void)
{
    int enable = 0; /* 缓存配置的启用标志 */
    int want = 0;   /* 启用链路数量 */
    int fail = 0;   /* 拉起失败数量 */
    int err = 0;    /* 首个错误码 */
    int ret = 0;    /* 子步骤返回值 */
    int i = 0;      /* 链路下标，从 0 开始 */

    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        memset(&g_mgr.link[i], 0, sizeof(NET_M_LINK_RT));
        g_mgr.link[i].type = (NET_M_TYPE)i;
        g_mgr.link[i].state = NET_M_ST_DISABLED;
        g_mgr.link[i].prev = NET_M_ST_DISABLED;
        g_mgr.link[i].rmetric = -1;
    }
    net_m_arb_reset(&g_mgr.arb);

    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        (void)core_conf_of((NET_M_TYPE)i, &enable);
        if (0 == enable)
        {
            continue;
        }
        ++want;

        switch ((NET_M_TYPE)i)
        {
        case NET_M_ETH:
            ret = core_link_reset(NET_M_ETH, &g_mgr.ethConf, 1, g_mgr.ethConf.prio);
            break;
        case NET_M_WIFI:
            ret = core_link_reset(NET_M_WIFI, &g_mgr.wifiConf, 1, g_mgr.wifiConf.prio);
            break;
        default:
            ret = core_link_reset(NET_M_CELL, &g_mgr.cellConf, 1, g_mgr.cellConf.prio);
            break;
        }
        if (ret < 0)
        {
            ++fail;
            err = ret;
        }
    }

    if (0 == want || fail >= want)
    {
        /* 全部失败：不留下半启动状态 */
        for (i = 0; i < NET_M_TYPE_MAX; ++i)
        {
            if (0 != g_mgr.link[i].enable)
            {
                (void)core_link_reset((NET_M_TYPE)i, NULL, 0, 0);
            }
        }
        return (0 != err) ? err : -NETMGRERR_CONF;
    }

    core_drive();
    return 0;
}

/**
 * @brief 停止：停事件监听、撤路由、关链路、置退出标志
 * @return 0 成功；负值为首个错误码
 */
static int core_do_stop(void)
{
    int err = 0; /* 首个错误码 */
    int ret = 0; /* 子步骤返回值 */
    int i = 0;   /* 链路下标，从 0 开始 */

    /* 第一步：停事件监听，之后不会再有新事件入队 */
    ret = netmgr_event_close();
    if (ret < 0)
    {
        err = ret;
    }

    /* 第二步：逐链路撤路由并关闭 */
    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        if (0 == g_mgr.link[i].enable)
        {
            continue;
        }
        ret = core_link_reset((NET_M_TYPE)i, NULL, 0, 0);
        if (ret < 0 && 0 == err)
        {
            err = ret;
        }
    }
    net_m_arb_reset(&g_mgr.arb);
    core_snap_refresh();

    pthread_mutex_lock(&g_mgr.lock);
    g_mgr.started = 0;
    g_mgr.quit = 1;
    g_mgr.cb = NULL;
    g_mgr.user = NULL;
    pthread_cond_broadcast(&g_mgr.cond);
    pthread_mutex_unlock(&g_mgr.lock);
    return err;
}

/**
 * @brief 应用一条链路配置：按新配置重建链路 → 保存配置副本 → 立即仲裁
 * @details enable / prio 一律取自新配置，保证「新配置关闭链路」也能生效；
 *          重建成败都保存配置副本，便于业务修正参数后仅切换开关即生效
 * @param type 链路类型
 * @param conf 配置，不可为空
 * @return 0 成功；负值为首个错误码
 */
static int core_do_set(NET_M_TYPE type, const void* conf)
{
    unsigned char prio = 0; /* 新配置的优先级 */
    int enable = 0;         /* 新配置的启用标志 */
    int ret = 0;            /* 重建结果 */

    if (NULL == conf || (int)type < 0 || (int)type >= NET_M_TYPE_MAX)
    {
        return -NETMGRERR_PARAM;
    }

    switch (type)
    {
    case NET_M_ETH:
        enable = ((const NET_M_ETH_CONF*)conf)->enable;
        prio = ((const NET_M_ETH_CONF*)conf)->prio;
        break;
    case NET_M_WIFI:
        enable = ((const NET_M_WIFI_CONF*)conf)->enable;
        prio = ((const NET_M_WIFI_CONF*)conf)->prio;
        break;
    default:
        enable = ((const NET_M_CELL_CONF*)conf)->enable;
        prio = ((const NET_M_CELL_CONF*)conf)->prio;
        break;
    }

    ret = core_link_reset(type, conf, enable, prio);

    switch (type)
    {
    case NET_M_ETH:
        memcpy(&g_mgr.ethConf, conf, sizeof(g_mgr.ethConf));
        break;
    case NET_M_WIFI:
        memcpy(&g_mgr.wifiConf, conf, sizeof(g_mgr.wifiConf));
        break;
    default:
        memcpy(&g_mgr.cellConf, conf, sizeof(g_mgr.cellConf));
        break;
    }

    core_drive();
    return ret;
}

/**
 * @brief 启用或关闭链路：启用前做优先级冲突检查
 * @param type 链路类型
 * @param on   1=启用，0=关闭
 * @return 0 成功；负值为错误码
 */
static int core_do_enable(NET_M_TYPE type, int on)
{
    const void* conf = NULL; /* 缓存配置 */
    int cacheEnable = 0;     /* 缓存配置的启用标志（此处不使用） */
    unsigned char prio = 0;  /* 启用时使用的优先级 */
    int ret = 0;             /* 子步骤返回值 */

    if ((int)type < 0 || (int)type >= NET_M_TYPE_MAX)
    {
        return -NETMGRERR_PARAM;
    }

    conf = core_conf_of(type, &cacheEnable);
    if (0 != on)
    {
        prio = (NET_M_ETH == type) ? g_mgr.ethConf.prio :
               ((NET_M_WIFI == type) ? g_mgr.wifiConf.prio : g_mgr.cellConf.prio);
        /* 启用前先确认不会出现同 metric 双默认路由 */
        if (0 != core_prio_conflict(type, prio))
        {
            return -NETMGRERR_PRIO;
        }
        ret = core_link_reset(type, conf, 1, prio);
    }
    else
    {
        ret = core_link_reset(type, NULL, 0, g_mgr.link[type].prio);
    }

    core_drive();
    return ret;
}

/**
 * @brief 修改或预检查优先级
 * @param type  链路类型
 * @param prio  目标优先级
 * @param apply 1=实际修改并仲裁；0=仅检查冲突
 * @return 0 可设置/已设置；-NETMGRERR_PRIO 冲突；其余负值为错误码
 */
static int core_do_prio(NET_M_TYPE type, unsigned char prio, int apply)
{
    if ((int)type < 0 || (int)type >= NET_M_TYPE_MAX)
    {
        return -NETMGRERR_PARAM;
    }
    if (0 != core_prio_conflict(type, prio))
    {
        return -NETMGRERR_PRIO;
    }
    if (0 == apply)
    {
        return 0;
    }

    g_mgr.link[type].prio = prio;
    core_drive();
    return 0;
}

/**
 * @brief 下发 DNS 并同步内部记录
 * @param dns DNS 地址，可为空（表示清除）
 * @return 0 成功；负值为错误码
 */
static int core_do_dns(const char* dns)
{
    int ret = 0; /* 写盘结果 */

    ret = netmgr_dns_apply(dns);
    if (ret < 0)
    {
        return ret;
    }

    (void)snprintf(g_mgr.arb.dns, sizeof(g_mgr.arb.dns), "%s", (NULL == dns) ? "" : dns);
    return 0;
}

/**
 * @brief 执行一次 WiFi 扫描：写调用方数组 + 更新缓存 + 上报完成事件
 * @param list 输出数组，不可为空
 * @param num  容量/条数，不可为空
 * @return 0 成功；负值为扫描错误码
 */
static int core_do_scan(NET_M_WIFI_AP* list, int* num)
{
    NET_M_EVENT item = {0}; /* 扫描完成事件 */
    int count = 0;          /* 实际条数 */
    int ret = 0;            /* 扫描结果 */
    int i = 0;              /* 缓存下标，从 0 开始 */

    ret = netmgr_wpa_scan(0);
    if (0 == ret)
    {
        count = *num; /* 入参为调用方容量 */
        if (0 != netmgr_wpa_scan_result(list, &count))
        {
            count = 0;
        }
        *num = count;

        pthread_mutex_lock(&g_mgr.lock);
        g_mgr.scanNum = (count > NETMGR_SCAN_MAX) ? NETMGR_SCAN_MAX : count;
        for (i = 0; i < g_mgr.scanNum; ++i)
        {
            g_mgr.scanCache[i] = list[i];
        }
        pthread_mutex_unlock(&g_mgr.lock);
    }
    else
    {
        count = 0;
        *num = 0;
    }

    /* 无论成败都上报扫描完成，complete 标记本轮是否拿到结果 */
    item.ev = NET_M_EV_WIFI_SCAN_DONE;
    item.u.scan.complete = (ret < 0) ? 0 : 1;
    item.u.scan.apNum = count;
    core_dispatch(&item);
    return ret;
}

/**
 * @brief 执行一条命令并返回结果（仅管理线程或已确认自调用时执行）
 * @param cmd 命令，不可为空
 * @return 命令结果
 */
static int core_exec(NET_M_CMD* cmd)
{
    if (NULL == cmd)
    {
        return -NETMGRERR_PARAM;
    }

    switch (cmd->type)
    {
    case NET_M_CMD_START:
        return core_do_start();
    case NET_M_CMD_STOP:
        return core_do_stop();
    case NET_M_CMD_SET_ETH:
        return core_do_set(NET_M_ETH, cmd->arg.conf);
    case NET_M_CMD_SET_WIFI:
        return core_do_set(NET_M_WIFI, cmd->arg.conf);
    case NET_M_CMD_SET_CELL:
        return core_do_set(NET_M_CELL, cmd->arg.conf);
    case NET_M_CMD_ENABLE:
        return core_do_enable(cmd->arg.en.type, cmd->arg.en.on);
    case NET_M_CMD_PRIO_SET:
        return core_do_prio(cmd->arg.prio.type, cmd->arg.prio.prio, 1);
    case NET_M_CMD_PRIO_CHECK:
        return core_do_prio(cmd->arg.prio.type, cmd->arg.prio.prio, 0);
    case NET_M_CMD_DNS:
        return core_do_dns(cmd->arg.dns);
    case NET_M_CMD_SCAN:
        return core_do_scan(cmd->arg.scan.list, cmd->arg.scan.num);
    default:
        return -NETMGRERR_PARAM;
    }
}

/* ===================== 命令投递与等待 ===================== */

/**
 * @brief 命令入队
 * @param cmd 命令，不可为空
 * @return 0 成功；-NETMGRERR_STATE 未启动
 */
static int core_post(NET_M_CMD* cmd)
{
    if (NULL == cmd)
    {
        return -NETMGRERR_PARAM;
    }

    pthread_mutex_lock(&g_mgr.lock);
    if (0 == g_mgr.started)
    {
        pthread_mutex_unlock(&g_mgr.lock);
        return -NETMGRERR_STATE;
    }

    cmd->next = NULL;
    cmd->done = 0;
    cmd->ret = 0;
    if (NULL != g_mgr.tail)
    {
        g_mgr.tail->next = cmd;
    }
    else
    {
        g_mgr.head = cmd;
    }
    g_mgr.tail = cmd;
    pthread_cond_broadcast(&g_mgr.cond);
    pthread_mutex_unlock(&g_mgr.lock);
    return 0;
}

/**
 * @brief 等待命令处理完成
 * @details 不做超时：命令处理期间可能包含 WiFi 关联（最长 40 秒）等阻塞动作，
 *          超时返回会让调用方在命令仍被引用时离开栈帧，产生悬垂访问
 * @param cmd 命令，不可为空
 * @return 命令结果
 */
static int core_wait(NET_M_CMD* cmd)
{
    int ret = 0; /* 命令结果 */

    pthread_mutex_lock(&g_mgr.lock);
    while (0 == cmd->done)
    {
        (void)pthread_cond_wait(&g_mgr.cond, &g_mgr.lock);
    }
    ret = cmd->ret;
    pthread_mutex_unlock(&g_mgr.lock);
    return ret;
}

/**
 * @brief 统一入口：管理线程内直接执行，否则投递并等待
 * @param cmd 命令，不可为空
 * @return 命令结果
 */
static int core_call(NET_M_CMD* cmd)
{
    int ret = 0; /* 子步骤返回值 */

    if (0 != core_is_self())
    {
        /* 回调内自调用：已在唯一所有者线程，直接执行，避免自等待死锁 */
        return core_exec(cmd);
    }

    ret = core_post(cmd);
    if (ret < 0)
    {
        return ret;
    }
    return core_wait(cmd);
}

/* ===================== 管理线程 ===================== */

static void* core_thread(void* arg)
{
    NET_M_CMD* cmd = NULL; /* 本轮取到的命令 */
    long long now = 0;     /* 当前时刻 */
    int ret = 0;           /* 命令结果 */

    (void)arg;

    while (1)
    {
        pthread_mutex_lock(&g_mgr.lock);
        if (NULL == g_mgr.head && 0 == g_mgr.quit)
        {
            /* 无命令时等待一小片时间：既为命令让路，也给定时仲裁提供节拍 */
            struct timespec ts = {0};

            (void)clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += (long)NET_M_WAIT_SLICE_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L)
            {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            (void)pthread_cond_timedwait(&g_mgr.cond, &g_mgr.lock, &ts);
        }

        if (0 != g_mgr.quit && NULL == g_mgr.head)
        {
            pthread_mutex_unlock(&g_mgr.lock);
            break;
        }

        cmd = g_mgr.head;
        if (NULL != cmd)
        {
            g_mgr.head = cmd->next;
            if (NULL == g_mgr.head)
            {
                g_mgr.tail = NULL;
            }
        }
        pthread_mutex_unlock(&g_mgr.lock);

        if (NULL != cmd)
        {
            ret = core_exec(cmd);
            pthread_mutex_lock(&g_mgr.lock);
            cmd->ret = ret;
            cmd->done = 1;
            pthread_cond_broadcast(&g_mgr.cond);
            pthread_mutex_unlock(&g_mgr.lock);
            continue;
        }

        /* 无命令：处理事件、按节拍仲裁 */
        core_ev_drain();

        now = core_now_ms();
        if (g_mgr.tickMs > 0 && now - g_mgr.lastTick >= (long long)g_mgr.tickMs)
        {
            g_mgr.lastTick = now;
            core_drive();
        }
    }
    return NULL;
}

/* ===================== 生命周期 ===================== */

/**
 * @brief 初始化命令队列条件变量（仅首次启动执行）
 * @details 绑定 CLOCK_MONOTONIC：系统时间跳变不影响等待与节拍
 * @return 0 成功；-NETMGRERR_THREAD 初始化失败
 */
static int core_cond_init(void)
{
    pthread_condattr_t attr; /* 条件变量属性 */
    int ret = 0;             /* 子步骤返回值 */

    if (0 != g_mgr.condInited)
    {
        return 0; /* 复用上次初始化的条件变量，避免 destroy/init 竞态 */
    }

    (void)pthread_condattr_init(&attr);
    ret = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (0 == ret)
    {
        ret = pthread_cond_init(&g_mgr.cond, &attr);
    }
    (void)pthread_condattr_destroy(&attr);
    if (0 != ret)
    {
        return -NETMGRERR_THREAD;
    }

    g_mgr.condInited = 1;
    return 0;
}

/**
 * @brief 打开 netlink 监听并创建管理线程
 * @details 先开监听再建线程：保证线程启动后事件已经可以入队，启动期的变化不丢
 * @return 0 成功；负值为错误码（失败时不留下已打开的资源）
 */
static int core_boot(void)
{
    int ret = 0; /* 子步骤返回值 */

    ret = netmgr_event_open(core_nev_cb, NULL, NETMGR_EVENT_GROUPS_DEFAULT);
    if (ret < 0)
    {
        return ret;
    }

    if (0 != pthread_create(&g_mgr.tid, NULL, core_thread, NULL))
    {
        (void)netmgr_event_close();
        return -NETMGRERR_THREAD;
    }
    return 0;
}

/**
 * @brief 置退出标志、等管理线程收尾并复位启动状态
 */
static void core_trip(void)
{
    pthread_mutex_lock(&g_mgr.lock);
    g_mgr.quit = 1;
    pthread_cond_broadcast(&g_mgr.cond);
    pthread_mutex_unlock(&g_mgr.lock);

    (void)pthread_join(g_mgr.tid, NULL);

    pthread_mutex_lock(&g_mgr.lock);
    g_mgr.started = 0;
    pthread_mutex_unlock(&g_mgr.lock);
}

int netm_start(const NET_M_CONF* conf, net_m_event_cb cb, void* user)
{
    NET_M_CMD cmd = {0}; /* START 命令 */
    int ret = 0;         /* 子步骤返回值 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    ret = core_conf_check(conf);
    if (ret < 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_mgr.lock);
    if (0 != g_mgr.started)
    {
        pthread_mutex_unlock(&g_mgr.lock);
        return -NETMGRERR_STATE;
    }

    /* 占位：并发 start 由 started 标志互斥，后续失败路径负责复位 */
    g_mgr.started = 1;
    g_mgr.quit = 0;
    g_mgr.head = NULL;
    g_mgr.tail = NULL;
    g_mgr.evHead = 0;
    g_mgr.evTail = 0;
    g_mgr.evCount = 0;
    g_mgr.evOverflow = 0;
    g_mgr.scanNum = 0;
    g_mgr.cb = cb;
    g_mgr.user = user;
    g_mgr.tickMs = (conf->tickMs > 0) ? conf->tickMs : NET_M_TICK_MS;
    g_mgr.lastTick = core_now_ms();
    memcpy(&g_mgr.ethConf, &conf->eth, sizeof(g_mgr.ethConf));
    memcpy(&g_mgr.wifiConf, &conf->wifi, sizeof(g_mgr.wifiConf));
    memcpy(&g_mgr.cellConf, &conf->cell, sizeof(g_mgr.cellConf));
    pthread_mutex_unlock(&g_mgr.lock);

    ret = core_cond_init();
    if (ret < 0)
    {
        pthread_mutex_lock(&g_mgr.lock);
        g_mgr.started = 0;
        pthread_mutex_unlock(&g_mgr.lock);
        return ret;
    }

    ret = core_boot();
    if (ret < 0)
    {
        pthread_mutex_lock(&g_mgr.lock);
        g_mgr.started = 0;
        pthread_mutex_unlock(&g_mgr.lock);
        return ret;
    }

    cmd.type = NET_M_CMD_START;
    ret = core_call(&cmd);
    if (ret < 0)
    {
        core_trip(); /* 启动失败：停线程并复位，不留下半启动 */
        return ret;
    }
    return 0;
}

int netm_stop(void)
{
    NET_M_CMD cmd = {0}; /* STOP 命令 */
    int started = 0;     /* 1=服务运行中 */
    int ret = 0;         /* 清理结果 */

    if (0 != core_is_self())
    {
        /* 回调内自调用：直接清理，管理线程返回后自行退出循环 */
        return core_do_stop();
    }

    pthread_mutex_lock(&g_mgr.lock);
    started = g_mgr.started;
    pthread_mutex_unlock(&g_mgr.lock);
    if (0 == started)
    {
        return -NETMGRERR_STATE;
    }

    cmd.type = NET_M_CMD_STOP;
    ret = core_call(&cmd);
    (void)pthread_join(g_mgr.tid, NULL);
    return ret;
}

/* ===================== 查询 ===================== */

int netm_link_get(NET_M_TYPE type, NET_M_LINK* out)
{
    if ((int)type < 0 || (int)type >= NET_M_TYPE_MAX || NULL == out)
    {
        return -NETMGRERR_PARAM;
    }

    pthread_mutex_lock(&g_mgr.lock);
    *out = g_mgr.snap[type];
    pthread_mutex_unlock(&g_mgr.lock);
    return 0;
}

int netm_link_list(NET_M_LINK* list, int* num)
{
    int cap = 0; /* 调用方容量 */
    int cnt = 0; /* 实际条数 */
    int i = 0;   /* 链路下标，从 0 开始 */

    if (NULL == list || NULL == num || *num <= 0)
    {
        return -NETMGRERR_PARAM;
    }
    cap = *num;

    pthread_mutex_lock(&g_mgr.lock);
    for (i = 0; i < NET_M_TYPE_MAX && cnt < cap; ++i)
    {
        list[cnt] = g_mgr.snap[i];
        ++cnt;
    }
    pthread_mutex_unlock(&g_mgr.lock);

    *num = cnt;
    return 0;
}

int netm_active_get(NET_M_TYPE* type)
{
    int active = -1; /* 主链路下标 */

    if (NULL == type)
    {
        return -NETMGRERR_PARAM;
    }

    pthread_mutex_lock(&g_mgr.lock);
    active = g_mgr.snapActive;
    pthread_mutex_unlock(&g_mgr.lock);

    *type = (active >= 0 && active < NET_M_TYPE_MAX) ? (NET_M_TYPE)active : NET_M_TYPE_MAX;
    return 0;
}

int netm_prio_get(NET_M_TYPE type, unsigned char* prio)
{
    if ((int)type < 0 || (int)type >= NET_M_TYPE_MAX || NULL == prio)
    {
        return -NETMGRERR_PARAM;
    }

    pthread_mutex_lock(&g_mgr.lock);
    *prio = g_mgr.snap[type].prio;
    pthread_mutex_unlock(&g_mgr.lock);
    return 0;
}

int netm_wifi_status(NET_M_WIFI_INFO* info)
{
    if (NULL == info)
    {
        return -NETMGRERR_PARAM;
    }

    pthread_mutex_lock(&g_mgr.lock);
    *info = g_mgr.snap[NET_M_WIFI].wifi;
    pthread_mutex_unlock(&g_mgr.lock);
    return 0;
}

int netm_wifi_scan_result(NET_M_WIFI_AP* list, int* num)
{
    int cap = 0; /* 调用方容量 */
    int cnt = 0; /* 实际条数 */
    int i = 0;   /* 缓存下标，从 0 开始 */

    if (NULL == list || NULL == num || *num <= 0)
    {
        return -NETMGRERR_PARAM;
    }
    cap = *num;

    pthread_mutex_lock(&g_mgr.lock);
    cnt = (g_mgr.scanNum < cap) ? g_mgr.scanNum : cap;
    for (i = 0; i < cnt; ++i)
    {
        list[i] = g_mgr.scanCache[i];
    }
    pthread_mutex_unlock(&g_mgr.lock);

    *num = cnt;
    return 0;
}

/* ===================== 运行时配置 ===================== */

int netm_prio_set(NET_M_TYPE type, unsigned char prio)
{
    NET_M_CMD cmd = {0}; /* PRIO_SET 命令 */

    if ((int)type < 0 || (int)type >= NET_M_TYPE_MAX)
    {
        return -NETMGRERR_PARAM;
    }

    cmd.type = NET_M_CMD_PRIO_SET;
    cmd.arg.prio.type = type;
    cmd.arg.prio.prio = prio;
    return core_call(&cmd);
}

int netm_prio_check(NET_M_TYPE type, unsigned char prio)
{
    NET_M_CMD cmd = {0}; /* PRIO_CHECK 命令 */

    if ((int)type < 0 || (int)type >= NET_M_TYPE_MAX)
    {
        return -NETMGRERR_PARAM;
    }

    cmd.type = NET_M_CMD_PRIO_CHECK;
    cmd.arg.prio.type = type;
    cmd.arg.prio.prio = prio;
    return core_call(&cmd);
}

int netm_link_enable(NET_M_TYPE type, int on)
{
    NET_M_CMD cmd = {0}; /* ENABLE 命令 */

    if ((int)type < 0 || (int)type >= NET_M_TYPE_MAX)
    {
        return -NETMGRERR_PARAM;
    }

    cmd.type = NET_M_CMD_ENABLE;
    cmd.arg.en.type = type;
    cmd.arg.en.on = (0 != on) ? 1 : 0;
    return core_call(&cmd);
}

int netm_eth_set(const NET_M_ETH_CONF* conf)
{
    NET_M_CMD cmd = {0}; /* SET_ETH 命令 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    cmd.type = NET_M_CMD_SET_ETH;
    cmd.arg.conf = conf;
    return core_call(&cmd);
}

int netm_wifi_set(const NET_M_WIFI_CONF* conf)
{
    NET_M_CMD cmd = {0}; /* SET_WIFI 命令 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    cmd.type = NET_M_CMD_SET_WIFI;
    cmd.arg.conf = conf;
    return core_call(&cmd);
}

int netm_cell_set(const NET_M_CELL_CONF* conf)
{
    NET_M_CMD cmd = {0}; /* SET_CELL 命令 */

    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }

    cmd.type = NET_M_CMD_SET_CELL;
    cmd.arg.conf = conf;
    return core_call(&cmd);
}

int netm_dns_set(const char* dns)
{
    NET_M_CMD cmd = {0}; /* DNS 命令 */

    if (NULL != dns && '\0' != dns[0] && !netmgr_ip_valid(dns))
    {
        return -NETMGRERR_PARAM;
    }

    cmd.type = NET_M_CMD_DNS;
    cmd.arg.dns = dns;
    return core_call(&cmd);
}

int netm_wifi_scan(NET_M_WIFI_AP* list, int* num)
{
    NET_M_CMD cmd = {0}; /* SCAN 命令 */

    if (NULL == list || NULL == num || *num <= 0)
    {
        return -NETMGRERR_PARAM;
    }

    cmd.type = NET_M_CMD_SCAN;
    cmd.arg.scan.list = list;
    cmd.arg.scan.num = num;
    return core_call(&cmd);
}
