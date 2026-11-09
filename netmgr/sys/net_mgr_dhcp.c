/**
 * @file net_mgr_dhcp.c
 * @brief netmgr 系统适配层：DHCP 取址实现
 * @details 采用「长驻会话」模型：DHCP 不是一次性取址，取不到要重试、取到后要续租，故
 *          1) 每个接口对应一个取址线程，线程内用 popen 执行 udhcpc（前台）并逐行判定输出；
 *          2) 取到租约即回调 SUCC，此后 udhcpc 常驻自行续租，续租成功不重复回调；
 *          3) 取不到租约由 udhcpc 自身持续重试且不退出，故不产生 FAIL；仅当其异常退出
 *             且本次从未取到租约时回调 FAIL，并由本层节流重建会话（回调无返回值）；
 *          4) 会话在 stop 时结束（按 pidfile 结束 udhcpc 后线程自行收尾）。
 *          本层不读取接口地址，调用方按回调给出的接口名自行读取，保持 sys 层各能力相互独立；
 *          本层不打日志，失败以负错误码返回。
 */
/* 行判定需用到 strcasestr，属 GNU 扩展，须在包含系统头之前打开 */
#define _GNU_SOURCE

#include "net_mgr_dhcp.h"

#include "net_mgr_err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

/** 命令缓冲长度：需容纳接口名、超时参数与 pidfile 路径 */
#define CMDSTR_LEN          128
/** 单行输出缓冲长度，与 netopt 的行读取长度保持一致 */
#define RETSTR_BUFF_LEN     128
/** 槽位占用标志位 */
#define DIRTY_FLAG          0x01
/** 停止取址时等待线程收尾的超时（秒） */
#define DHCP_STOP_S         3
/** 会话失败/异常结束后的重试节流间隔（毫秒），避免 udhcpc 立刻退出时高频重建 */
#define DHCP_RETRY_MS       1000
/** 取址失败判定时长（秒）：会话内超过该时长仍未拿到租约即上报一次 FAIL；
 *  取值依据：udhcpc 默认约 9 秒完成一轮重试（-t 3 次 × -T 3 秒），留余量避免误判 */
#define DHCP_FAIL_REPORT_S  10
/** udhcpc pidfile 路径前缀，用于停止时精确结束取址进程 */
#define DHCP_PIDFILE_PRE    "/tmp/.netmgr_udhcpc_"

/** 单个 DHCP 实例（一个接口对应一个常驻取址会话） */
typedef struct {
    int flag;                   /**< 槽位标志，DIRTY_FLAG 置位表示线程占用中 */
    pthread_t tid;              /**< 取址线程 */
    netmgr_dhcp_cb hand;        /**< 取址状态回调 */
    void* user;                 /**< 回调透传的用户指针，可为空 */
    volatile int stopReq;       /**< 1=已请求停止，线程不再回调取址结果 */
    char ifname[16];            /**< 接口名 */
    char cmdstr[CMDSTR_LEN];    /**< 执行命令 */
    char pidfile[CMDSTR_LEN];   /**< udhcpc pidfile 路径，停止时按此精确定位进程 */
    char leaseIp[16];           /**< 最近一次租约地址，无租约为空串 */
    int leaseSec;               /**< 最近一次租约时长（秒），未知为 0 */
    time_t leaseAt;             /**< 租约获取时刻，0 表示当前无租约 */
} NETMGR_DHCP;

/** DHCP 管理上下文，承载本层全部状态 */
typedef struct {
    NETMGR_DHCP item[4];  /**< 实例表，按接口占用槽位（数量等于链路类型上限） */
    pthread_mutex_t lock; /**< 实例表互斥锁 */
    pthread_cond_t cond;  /**< 槽位变化条件，清除脏标志时广播 */
} NETMGR_DHCP_MGR;

/**
 * DHCP 管理上下文（模块内单实例，本层状态全部收敛于此）
 * 用途：保存 DHCP 实例表及其同步原语，供取址申请、停止与线程收尾共用
 * 单位/范围：实例表 4 个槽位；flag 取 0/DIRTY_FLAG；timeout 取 0~3600 秒
 * 默认值依据：静态零初始化（全部槽位空闲），同步原语用静态初始化器
 * 写入方/读取方：申请与停止由调用线程在 lock 保护下读写，取址线程只清脏标志并写 err
 * 生命周期：仅内存态，随进程存在；槽位释放后保留接口名以便复用
 */
static NETMGR_DHCP_MGR g_dhcp_mgr = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

/** 实例槽位数量：由实例表维度决定，供申请/停止/查询三处循环共用 */
static const int g_dhcpMax = (int)(sizeof(g_dhcp_mgr.item) / sizeof(g_dhcp_mgr.item[0]));

/**
 * @brief 拼装取址命令
 * @details -f 前台执行（popen 才能持续读到输出）；
 *          不加 -n：取不到租约不退出，由 udhcpc 自身持续重试，避免高频重建进程；
 *          不加 -q：取到租约后常驻并由它自身完成续租，避免租约到期后无人续租；
 *          不加 -T：发包节奏由 udhcpc 自身默认值决定，本层不暴露无效调节钮
 * @param item 当前实例，不可为空
 */
static void dhcp_build_cmd(NETMGR_DHCP* item)
{
    int len = 0; /* 已写入长度 */

    if (NULL == item)
        return;

    len = snprintf(item->cmdstr, sizeof(item->cmdstr), "udhcpc -i %s -p %s -f 2>&1",
                   item->ifname, item->pidfile);
    if (len <= 0 || len >= (int)sizeof(item->cmdstr))
        item->cmdstr[0] = '\0'; /* 拼接失败时置空，由执行阶段报错 */

    item->cmdstr[sizeof(item->cmdstr) - 1] = '\0';
}

/**
 * @brief 按 udhcpc pidfile 结束取址进程并清理该文件
 * @param path pidfile 路径，可为空
 */
static void dhcp_kill_by_pidfile(const char* path)
{
    FILE* fp = NULL;            /* pidfile 句柄 */
    char buf[16] = {0};         /* pid 文本 */
    int pid = 0;                /* 取址进程号 */

    if (NULL == path || '\0' == path[0])
        return;

    fp = fopen(path, "r");
    if (NULL != fp)
    {
        if (NULL != fgets(buf, (int)sizeof(buf), fp))
            pid = atoi(buf);
        fclose(fp);
    }

    if (pid > 0)
        (void)kill(pid, SIGTERM);

    (void)unlink(path);
}

/**
 * @brief 执行一次取址会话：逐行判定输出，取到租约即回调 SUCC
 * @details 会话内 udhcpc 常驻：取不到租约由它自身持续重试、取到后由它自身续租，
 *          因此本函数只会在 stop 请求或 udhcpc 异常退出时返回；续租输出不重复上报 SUCC
 * @param item     当前实例，不可为空
 * @param gotLease 输出 1=本次会话曾取到租约，0=未取到，不可为空
 * @return 0 会话结束；负值为 NETMGRERR_* 错误码（管道建立失败等执行异常）
 */
/**
 * @brief 取单调时钟秒数，供失败判定计时
 * @details 用 CLOCK_MONOTONIC 而非 time()：系统时间被回拨时不会导致判定时长失效
 * @return 单调时钟秒数
 */
static time_t dhcp_mono_s(void)
{
    struct timespec ts = {0}; /* 单调时钟读数 */

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/**
 * @brief 从 udhcpc 输出行解析租约地址与租约时长
 * @details 目标形如 "udhcpc: lease of 192.168.1.10 obtained from <server>, lease time 86400"；
 *          该行给出的是服务器标识而非服务器 IP，故不解析服务器地址
 * @param line     单行输出，不可为空
 * @param ip       地址输出缓冲，不可为空
 * @param ipLen    地址缓冲长度
 * @param leaseSec 租约时长输出，可为空（不需要时传空）
 * @return 1 命中租约行；0 非租约行
 */
static int dhcp_parse_lease(const char* line, char* ip, int ipLen, int* leaseSec)
{
    const char* pos = NULL; /* 关键字位置 */
    const char* end = NULL; /* 值结尾 */
    int n = 0;              /* 值长度 */

    if (NULL == line || NULL == ip || ipLen <= 0)
        return 0;

    pos = strcasestr(line, "lease of ");
    if (NULL == pos)
        return 0;

    pos += strlen("lease of ");
    end = strchr(pos, ' ');
    n = (NULL == end) ? (int)strlen(pos) : (int)(end - pos);
    if (n <= 0 || n >= ipLen)
        return 0;

    (void)memcpy(ip, pos, (size_t)n);
    ip[n] = '\0';

    if (NULL != leaseSec)
    {
        pos = strcasestr(line, "lease time ");
        *leaseSec = (NULL == pos) ? 0 : atoi(pos + strlen("lease time "));
    }
    return 1;
}

/**
 * @brief 执行一次取址会话：逐行判定输出，维护租约快照并按状态变化回调
 * @details 会话内 udhcpc 常驻：取不到租约由它自身持续重试、取到后由它自身续租，
 *          因此本函数只会在 stop 请求或 udhcpc 异常退出时返回
 * @param item     当前实例，不可为空
 * @param gotLease 输出 1=本次会话曾取到租约，0=未取到，不可为空
 * @return 0 会话结束；负值为 NETMGRERR_* 错误码（管道建立失败等执行异常）
 */
static int dhcp_session(NETMGR_DHCP* item, int* gotLease)
{
    FILE* fp = NULL;                  /* 管道流 */
    char line[RETSTR_BUFF_LEN] = {0}; /* 单行输出缓冲 */
    char ip[16] = {0};                /* 解析出的租约地址 */
    time_t begin = 0;                 /* 本轮取址计时起点 */
    int leaseSec = 0;                 /* 解析出的租约时长 */
    int succ = 0;                     /* 1=当前处于持有租约状态 */
    int failReported = 0;             /* 1=本轮取址已上报过 FAIL */

    if (NULL == item || NULL == gotLease)
        return -NETMGRERR_PARAM;

    *gotLease = 0;
    fp = popen(item->cmdstr, "r");
    if (NULL == fp)
        return -NETMGRERR_PROC;

    begin = dhcp_mono_s();
    while (NULL != fgets(line, (int)sizeof(line) - 1, fp))
    {
        if (item->stopReq)
            break;

        /* 租约行：刷新租约快照；从"未持有"变为"持有"时上报 SUCC（LOST 后重新取到同样上报） */
        if (0 != dhcp_parse_lease(line, ip, (int)sizeof(ip), &leaseSec))
        {
            pthread_mutex_lock(&g_dhcp_mgr.lock);
            (void)snprintf(item->leaseIp, sizeof(item->leaseIp), "%s", ip);
            item->leaseSec = leaseSec;
            item->leaseAt = time(NULL);
            pthread_mutex_unlock(&g_dhcp_mgr.lock);

            *gotLease = 1;
            if (0 == succ)
            {
                succ = 1;
                failReported = 0;
                begin = dhcp_mono_s();
                if (NULL != item->hand)
                    item->hand(NETMGR_DHCP_SUCC, item->ifname, 0, item->user);
            }
            continue;
        }

        /* 已持有租约却再次进入发现阶段：续租失败，上报一次 LOST 并重新开始计时 */
        if (0 != succ && NULL != strcasestr(line, "sending discover"))
        {
            succ = 0;
            failReported = 0;
            begin = dhcp_mono_s();
            if (NULL != item->hand)
                item->hand(NETMGR_DHCP_LOST, item->ifname, 0, item->user);
            continue;
        }

        /* 本轮超过判定时长仍未取得租约：说明对端无响应，上报一次 FAIL；
           udhcpc 会在会话内继续重试，成功后按上面的分支上报 SUCC 形成状态翻转 */
        if (0 == succ && 0 == failReported && NULL != item->hand &&
            (dhcp_mono_s() - begin) >= DHCP_FAIL_REPORT_S)
        {
            failReported = 1;
            item->hand(NETMGR_DHCP_FAIL, item->ifname, -NETMGRERR_DHCP, item->user);
        }
    }

    (void)pclose(fp);
    return 0;
}

/**
 * @brief 会话重建/重试前的节流等待，分片休眠以便 stop 及时生效
 * @param item 当前实例，不可为空
 */
static void dhcp_throttle(const NETMGR_DHCP* item)
{
    int i = 0; /* 休眠分片下标，从 0 开始 */

    for (i = 0; i < DHCP_RETRY_MS / 100; ++i)
    {
        if (item->stopReq)
            return;
        usleep(100 * 1000);
    }
}

/**
 * @brief 取址线程：维持取址会话，会话结束时自动节流重建，退出时清脏标志
 * @details 会话（udhcpc 进程）结束时按节流间隔重建，直到 stop 为止；
 *          本轮从未取到租约时先回调一次 FAIL；重试策略完全由本层决定，与回调无关
 * @param arg 指向占用的 NETMGR_DHCP 实例
 * @return 恒定返回空指针
 */
static void* dhcp_run(void* arg)
{
    NETMGR_DHCP* item = (NETMGR_DHCP*)arg; /* 当前实例 */
    int gotLease = 0;                      /* 本次会话是否曾取到租约 */
    int ret = 0;                           /* 本次会话结果 */

    if (NULL == item)
        return NULL;

    /* 命令与「已发起取址」状态只与接口配置有关：启动时一次性确定，
       运行期不存在调整入口，因此命令只构建一次、RUNNING 只上报一次，重建直接复用 */
    dhcp_build_cmd(item);
    if (NULL != item->hand)
        item->hand(NETMGR_DHCP_RUNNING, item->ifname, 0, item->user);

    while (!item->stopReq)
    {
        gotLease = 0;
        ret = dhcp_session(item, &gotLease);
        if (item->stopReq)
            break;

        /* 从未取到租约即结束（管道建立失败或 udhcpc 异常退出，如命令不存在）：
           先回调上报；无服务器属进程内部重试，走不到这里 */
        if (0 == gotLease && NULL != item->hand)
            item->hand(NETMGR_DHCP_FAIL, item->ifname,
                       (ret < 0) ? ret : -NETMGRERR_DHCP, item->user);

        /* 会话结束一律节流后重建：已取到租约的属续租中断（尽量短），
           异常退出的保持重试，直到 stop 为止 */
        dhcp_throttle(item);
    }

    /* 退出：清脏标志并唤醒等待方，接口名保留供下次复用 */
    pthread_mutex_lock(&g_dhcp_mgr.lock);
    item->flag &= ~DIRTY_FLAG;
    pthread_cond_broadcast(&g_dhcp_mgr.cond);
    pthread_mutex_unlock(&g_dhcp_mgr.lock);
    return NULL;
}

/**
 * @brief 在指定接口上启动 DHCP 取址会话，结果经回调返回
 * @details 发包节奏取 udhcpc 默认值，本层不提供调节入口
 * @param ifname 接口名，不可为空
 * @param cb     取址状态回调，可为空
 * @param user   回调透传的用户指针，可为空
 * @return 0 已受理；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_start(const char* ifname, netmgr_dhcp_cb cb, void* user)
{
    NETMGR_DHCP* item = NULL; /* 本次占用的实例 */
    int idx = -1;             /* 首个空闲槽位下标 */
    int i = 0;                /* 实例表下标，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0])
        return -NETMGRERR_PARAM;

    /* 第 1 步：查重并挑槽位，对应 netopt 的脏标志遍历 */
    pthread_mutex_lock(&g_dhcp_mgr.lock);
    for (i = 0; i < g_dhcpMax; ++i)
    {
        if ((DIRTY_FLAG & g_dhcp_mgr.item[i].flag) &&
            0 == strncmp(g_dhcp_mgr.item[i].ifname, ifname, sizeof(g_dhcp_mgr.item[i].ifname)))
        {
            pthread_mutex_unlock(&g_dhcp_mgr.lock);
            return -NETMGRERR_DHCP; /* 该接口已有取址会话 */
        }
        if (idx < 0 && !(DIRTY_FLAG & g_dhcp_mgr.item[i].flag))
            idx = i;
    }
    if (idx < 0)
    {
        pthread_mutex_unlock(&g_dhcp_mgr.lock);
        return -NETMGRERR_DHCP; /* 槽位已满 */
    }

    /* 第 2 步：占用槽位并登记本次取址参数 */
    item = &g_dhcp_mgr.item[idx];
    item->hand = cb;
    item->user = user;
    item->stopReq = 0;
    item->leaseIp[0] = '\0';
    item->leaseSec = 0;
    item->leaseAt = 0;
    (void)snprintf(item->ifname, sizeof(item->ifname), "%s", ifname);
    (void)snprintf(item->pidfile, sizeof(item->pidfile), DHCP_PIDFILE_PRE "%s.pid", ifname);

    /* 先置脏再创建线程，避免线程极快结束时脏标志被重复置位 */
    item->flag |= DIRTY_FLAG;
    pthread_mutex_unlock(&g_dhcp_mgr.lock);

    if (0 != pthread_create(&item->tid, NULL, dhcp_run, item))
    {
        pthread_mutex_lock(&g_dhcp_mgr.lock);
        item->flag &= ~DIRTY_FLAG;
        pthread_cond_broadcast(&g_dhcp_mgr.cond);
        pthread_mutex_unlock(&g_dhcp_mgr.lock);
        return -NETMGRERR_THREAD;
    }

    /* 线程以 detach 方式运行，退出时自行清脏标志 */
    (void)pthread_detach(item->tid);
    return 0;
}

/**
 * @brief 停止指定接口上的 DHCP 取址会话
 * @param ifname 接口名，不可为空
 * @return 0 成功（未启动时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_stop(const char* ifname)
{
    NETMGR_DHCP* item = NULL; /* 命中的实例 */
    struct timespec ts = {0}; /* 等待线程收尾的截止时间 */
    int i = 0;                /* 实例表下标，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0])
        return -NETMGRERR_PARAM;

    pthread_mutex_lock(&g_dhcp_mgr.lock);
    for (i = 0; i < g_dhcpMax; ++i)
    {
        if ('\0' == g_dhcp_mgr.item[i].ifname[0])
            continue;
        if (0 != strncmp(g_dhcp_mgr.item[i].ifname, ifname, sizeof(g_dhcp_mgr.item[i].ifname)))
            continue;
        item = &g_dhcp_mgr.item[i];
        break;
    }
    if (NULL == item)
    {
        pthread_mutex_unlock(&g_dhcp_mgr.lock);
        return 0;
    }
    item->stopReq = 1; /* 主动停止，线程不再回调取址结果 */
    item->leaseIp[0] = '\0';
    item->leaseSec = 0;
    item->leaseAt = 0;
    /* ------------------------------------------------------------------
     * 自线程检测：业务在 DHCP 回调里关链路时，stop 会在取址线程内被调用
     * ------------------------------------------------------------------
     * 可达调用链（不是误用，回调契约明确允许）：
     *   取址线程 → item->hand() = core_dhcp_cb → core_dispatch
     *            → 业务 netmgr_event_cb 回调
     *            → netmgr_core_link_enable(type, 0) / netmgr_core_stop()
     *            → core_link_reset → ops->close → netmgr_dhcp_stop(ifname)
     * 此时若继续执行下面的“等待 DIRTY_FLAG 清零”，等的是本线程自己的收尾：
     *   · 若用 pthread_join：自己 join 自己 → 必然死锁；
     *   · 若用条件变量等待：白等满 DHCP_STOP_S(3 秒) 后返回
     *     -NETMGRERR_TIMEOUT —— 这是假错误（stopReq 已置、udhcpc 已杀），
     *     业务会误判“关闭失败”，可能触发无意义的重试或错误上报。
     * 正确处理：置 stopReq 后立即返回 0（“停止请求已受理”），
     *   收尾交给本线程从回调返回后、主循环 while (!stopReq) 退出时的自然清理
     *   （清 DIRTY_FLAG + 广播唤醒）。
     * 仍然要 kill：本线程此刻多半阻塞在 popen 读管道上，不结束 udhcpc
     *   就等不到 EOF，退出会被拖到下一次输出，收尾反而更慢。
     * 约束：本路径返回时槽位尚未释放，同一接口立即重启会话会失败（预期行为）。
     * ------------------------------------------------------------------ */
    if (0 != pthread_equal(pthread_self(), item->tid))
    {
        pthread_mutex_unlock(&g_dhcp_mgr.lock);
        dhcp_kill_by_pidfile(item->pidfile);
        return 0;
    }
    pthread_mutex_unlock(&g_dhcp_mgr.lock);

    /* 按 pidfile 结束取址进程：popen 拿不到子进程号，pidfile 是 udhcpc 提供的定位方式 */
    dhcp_kill_by_pidfile(item->pidfile);

    /* 等取址线程读到输出结束、自行清脏标志 */
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += DHCP_STOP_S;
    pthread_mutex_lock(&g_dhcp_mgr.lock);
    while (DIRTY_FLAG & item->flag)
    {
        if (0 != pthread_cond_timedwait(&g_dhcp_mgr.cond, &g_dhcp_mgr.lock, &ts))
        {
            pthread_mutex_unlock(&g_dhcp_mgr.lock);
            return -NETMGRERR_TIMEOUT;
        }
    }
    pthread_mutex_unlock(&g_dhcp_mgr.lock);
    return 0;
}

/**
 * @brief 查询指定接口的 DHCP 会话是否在运行
 * @details 会话从 start 建立到 stop 结束期间恒为运行中，续租阶段同样计入
 * @param ifname 接口名，不可为空
 * @return 1 会话运行中；0 未启动或会话已结束
 */
int netmgr_dhcp_running(const char* ifname)
{
    int busy = 0; /* 1=运行中 */
    int i = 0;    /* 实例表下标，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0])
        return 0;

    pthread_mutex_lock(&g_dhcp_mgr.lock);
    for (i = 0; i < g_dhcpMax; ++i)
    {
        if (!(DIRTY_FLAG & g_dhcp_mgr.item[i].flag))
            continue;
        if (0 != strncmp(g_dhcp_mgr.item[i].ifname, ifname, sizeof(g_dhcp_mgr.item[i].ifname)))
            continue;
        busy = 1;
        break;
    }
    pthread_mutex_unlock(&g_dhcp_mgr.lock);
    return busy;
}

/**
 * @brief 查询指定接口当前租约信息
 * @details 剩余租期按「获取时刻 + 租约时长 - 当前时刻」推算，过期后 valid 置 0
 * @param ifname 接口名，不可为空
 * @param info   输出租约信息，不可为空
 * @return 0 成功（无会话或无租约时 valid=0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_lease_get(const char* ifname, NETMGR_DHCP_LEASE* info)
{
    NETMGR_DHCP* item = NULL; /* 命中的实例 */
    time_t now = 0;           /* 查询时刻 */
    time_t expire = 0;        /* 租约到期时刻 */
    int i = 0;                /* 实例表下标，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0] || NULL == info)
        return -NETMGRERR_PARAM;

    memset(info, 0, sizeof(*info));

    pthread_mutex_lock(&g_dhcp_mgr.lock);
    for (i = 0; i < g_dhcpMax; ++i)
    {
        if (0 == strncmp(g_dhcp_mgr.item[i].ifname, ifname, sizeof(g_dhcp_mgr.item[i].ifname)))
        {
            item = &g_dhcp_mgr.item[i];
            break;
        }
    }

    if (NULL != item && 0 != item->leaseAt)
    {
        now = time(NULL);
        (void)snprintf(info->ip, sizeof(info->ip), "%s", item->leaseIp);
        info->leaseSec = item->leaseSec;
        if (item->leaseSec <= 0)
        {
            /* 租期未知（输出未给 lease time）：地址在位即视为持有，剩余期数置 0 */
            info->valid = 1;
            info->remainSec = 0;
        }
        else
        {
            expire = item->leaseAt + (time_t)item->leaseSec;
            if (expire > now)
            {
                info->remainSec = (int)(expire - now);
                info->valid = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_dhcp_mgr.lock);
    return 0;
}
