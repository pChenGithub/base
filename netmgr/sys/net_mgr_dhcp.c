/**
 * @file net_mgr_dhcp.c
 * @brief netmgr 系统适配层：DHCP 取址实现（udhcpc 子进程 + 收尾线程）
 * @details 实例表由本层互斥锁保护：仲裁线程申请与停止，收尾线程只负责释放。
 */
#include "net_mgr_dhcp.h"

#include "net_mgr_err.h"
#include "net_mgr_types.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>

/** 停止取址时等待收尾线程释放槽位的超时（秒） */
#define DHCP_STOP_S     3
/** 子进程重定向输出失败时的退出码 */
#define DHCP_EXIT_DUP   126
/** 子进程执行 udhcpc 失败时的退出码 */
#define DHCP_EXIT_EXEC  127

/** 单个 DHCP 实例 */
typedef struct {
    int used;                   /**< 1=实例被占用 */
    int fd;                     /**< 子进程输出管道读端，-1 表示无效 */
    pid_t pid;                  /**< udhcpc 子进程号，-1 表示无 */
    int err;                    /**< 最近一次取址的错误码，0 表示成功或仍在进行 */
    pthread_t tid;              /**< 输出收尾线程 */
    char ifname[NETMGR_IF_LEN]; /**< 接口名 */
} NETMGR_DHCP;

/** DHCP 管理上下文，承载本层全部状态 */
typedef struct {
    NETMGR_DHCP item[4];      /**< 实例表，按接口占用槽位（数量等于链路类型上限） */
    pthread_mutex_t lock;     /**< 实例表互斥锁 */
    pthread_cond_t cond;      /**< 槽位变化条件，释放槽位时广播 */
} NETMGR_DHCP_MGR;

/**
 * DHCP 管理上下文（模块内单实例，本层状态全部收敛于此）
 * 用途：保存 DHCP 实例表及其同步原语，供取址申请、停止与收尾释放共用
 * 单位/范围：实例表 4 个槽位；used 取 0/1；pid/fd 无效时为 -1
 * 默认值依据：静态零初始化（全部槽位空闲），同步原语用静态初始化器
 * 写入方/读取方：仲裁线程申请与释放，收尾线程只释放；均在 g_dhcp_mgr.lock 保护下访问
 * 生命周期：仅内存态，随进程存在，槽位用完即回收复用
 */
static NETMGR_DHCP_MGR g_dhcp_mgr = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

/** 实例槽位数量：由实例表维度决定，供申请/停止/查询三处循环共用 */
static const int g_dhcpMax = (int)(sizeof(g_dhcp_mgr.item) / sizeof(g_dhcp_mgr.item[0]));

/**
 * @brief DHCP 输出收尾线程：排空子进程输出、回收子进程并释放槽位
 * @param arg 指向占用的 NETMGR_DHCP 实例
 * @return 恒定返回空指针
 */
static void* dhcp_reap(void* arg)
{
    NETMGR_DHCP* item = (NETMGR_DHCP*)arg; /* 当前实例 */
    char line[256] = {0};                  /* 单行输出缓冲 */
    FILE* fp = NULL;                       /* 管道流视图 */
    int status = 0;                        /* 子进程退出状态 */
    int no_lease = 0;                      /* 1=已判定未取得租约 */

    if (NULL == item)
        return NULL;

    fp = fdopen(item->fd, "r");
    if (NULL == fp)
    {
        close(item->fd);
        item->fd = -1;
        item->err = -NETMGRERR_FILE; /* 取址输出无法读取，按异常记录 */
    }
    else
    {
        /* 排空输出：既判断取址结果，也避免管道写满阻塞 udhcpc */
        while (NULL != fgets(line, (int)sizeof(line), fp))
        {
            if (NULL != strstr(line, "No lease"))
            {
                no_lease = 1;
            }
        }
        fclose(fp);
        item->fd = -1;
    }

    /* 异步失败无返回通道，统一记录到实例字段，由 stop 汇总上抛 */
    if (item->pid > 0)
    {
        if (item->pid != waitpid(item->pid, &status, 0))
            item->err = -NETMGRERR_PROC;
        else if (!no_lease && !(WIFEXITED(status) && 0 == WEXITSTATUS(status)))
            item->err = -NETMGRERR_DHCP;
    }

    /* 收尾：释放槽位并通知等待方，后续同接口可再次取址 */
    pthread_mutex_lock(&g_dhcp_mgr.lock);
    item->used = 0;
    item->pid = -1;
    item->fd = -1;
    pthread_cond_broadcast(&g_dhcp_mgr.cond);
    pthread_mutex_unlock(&g_dhcp_mgr.lock);
    return NULL;
}

/**
 * @brief 在指定接口上启动 DHCP 取址
 * @param ifname  接口名，不可为空
 * @param timeout 单次请求超时秒数，0 表示使用 udhcpc 默认值
 * @return 0 成功（该接口已在取址中时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_start(const char* ifname, int timeout)
{
    NETMGR_DHCP* item = NULL; /* 本次占用的实例 */
    int pfd[2] = {-1, -1};    /* 子进程输出管道：0=读端 1=写端 */
    pid_t pid = -1;           /* 子进程号 */
    int idx = -1;             /* 空闲槽位下标 */
    int i = 0;                /* 实例表下标，从 0 开始 */
    char tmo[16] = {0};       /* 超时秒数文本 */
    char* args[9] = {0};      /* udhcpc 参数表，最后一项为空结尾 */
    pthread_attr_t attr;      /* 收尾线程属性，设为 detach */

    if (NULL == ifname || '\0' == ifname[0] || timeout < 0)
    {
        return -NETMGRERR_PARAM;
    }

    /* 第 1 步：查重并占位，占位后立即释放锁，避免阻塞其它链路取址 */
    pthread_mutex_lock(&g_dhcp_mgr.lock);
    for (i = 0; i < g_dhcpMax; ++i)
    {
        if (g_dhcp_mgr.item[i].used &&
            0 == strncmp(g_dhcp_mgr.item[i].ifname, ifname, sizeof(g_dhcp_mgr.item[i].ifname)))
        {
            pthread_mutex_unlock(&g_dhcp_mgr.lock);
            return 0;
        }
        if (!g_dhcp_mgr.item[i].used && idx < 0)
            idx = i;
    }
    if (idx < 0)
    {
        pthread_mutex_unlock(&g_dhcp_mgr.lock);
        return -NETMGRERR_DHCP;
    }
    item = &g_dhcp_mgr.item[idx];
    item->used = 1;
    item->err = 0;
    snprintf(item->ifname, sizeof(item->ifname), "%s", ifname);
    pthread_mutex_unlock(&g_dhcp_mgr.lock);

    /* 第 2 步：组装参数，前台运行、取到或失败即退出 */
    args[0] = (char*)"udhcpc";
    args[1] = (char*)"-i";
    args[2] = (char*)ifname;
    args[3] = (char*)"-f";
    args[4] = (char*)"-q";
    args[5] = (char*)"-n";
    if (timeout > 0)
    {
        snprintf(tmo, sizeof(tmo), "%d", timeout);
        args[6] = (char*)"-T";
        args[7] = tmo;
    }

    if (0 != pipe(pfd))
    {
        goto fail;
    }

    /* 第 3 步：派生 udhcpc，子进程把输出并入管道后替换自身 */
    pid = fork();
    if (pid < 0)
    {
        close(pfd[0]);
        close(pfd[1]);
        goto fail;
    }
    if (0 == pid)
    {
        close(pfd[0]);
        if (0 > dup2(pfd[1], STDOUT_FILENO) || 0 > dup2(pfd[1], STDERR_FILENO))
            _exit(DHCP_EXIT_DUP);
        close(pfd[1]);
        execvp("udhcpc", args);
        _exit(DHCP_EXIT_EXEC);
    }

    /* 第 4 步：父进程保留读端，交由收尾线程排空并回收 */
    close(pfd[1]);
    item->fd = pfd[0];
    item->pid = pid;

    /* 收尾线程以 detach 方式运行，退出时自行释放槽位，避免线程资源残留 */
    if (0 != pthread_attr_init(&attr))
    {
        close(pfd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        goto fail;
    }
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (0 != pthread_create(&item->tid, &attr, dhcp_reap, item))
    {
        pthread_attr_destroy(&attr);
        close(pfd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        goto fail;
    }
    pthread_attr_destroy(&attr);
    return 0;

fail:
    /* 任一步骤失败都回收到同一出口，先释放槽位再返回，避免槽位泄漏 */
    pthread_mutex_lock(&g_dhcp_mgr.lock);
    item->used = 0;
    item->fd = -1;
    item->pid = -1;
    pthread_cond_broadcast(&g_dhcp_mgr.cond);
    pthread_mutex_unlock(&g_dhcp_mgr.lock);
    return -NETMGRERR_DHCP;
}

/**
 * @brief 停止指定接口上的 DHCP 取址
 * @param ifname 接口名，不可为空
 * @return 0 成功（未启动时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_stop(const char* ifname)
{
    NETMGR_DHCP* item = NULL; /* 命中的实例 */
    struct timespec ts = {0}; /* 等待槽位释放的截止时间 */
    pid_t pid = -1;           /* 待结束的子进程号 */
    int found = 0;            /* 1=找到该接口对应的实例 */
    int ret = 0;              /* 最近一次取址的错误码 */
    int i = 0;                /* 实例表下标，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0])
    {
        return -NETMGRERR_PARAM;
    }

    /* 先在锁内取出实例与子进程号，结束进程时不能持锁，否则收尾线程无法释放槽位 */
    pthread_mutex_lock(&g_dhcp_mgr.lock);
    for (i = 0; i < g_dhcpMax; ++i)
    {
        if ('\0' == g_dhcp_mgr.item[i].ifname[0])
            continue;
        if (0 != strncmp(g_dhcp_mgr.item[i].ifname, ifname, sizeof(g_dhcp_mgr.item[i].ifname)))
            continue;
        item = &g_dhcp_mgr.item[i];
        found = 1;
        /* 仅在实例仍被占用时才需要结束子进程；已收尾的实例只取错误码 */
        if (item->used)
            pid = item->pid;
        break;
    }
    pthread_mutex_unlock(&g_dhcp_mgr.lock);

    if (!found)
        return 0;

    /* 实例已收尾：直接把最近一次取址结果作为错误码上抛 */
    if (pid <= 0)
    {
        pthread_mutex_lock(&g_dhcp_mgr.lock);
        ret = item->err;
        pthread_mutex_unlock(&g_dhcp_mgr.lock);
        return ret;
    }

    if (0 != kill(pid, SIGTERM))
        return -NETMGRERR_PROC;

    /* 等待收尾线程释放槽位：线程已 detach，只能以其占位状态判断是否收尾完成 */
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += DHCP_STOP_S;
    pthread_mutex_lock(&g_dhcp_mgr.lock);
    while (item->used)
    {
        if (0 != pthread_cond_timedwait(&g_dhcp_mgr.cond, &g_dhcp_mgr.lock, &ts))
        {
            pthread_mutex_unlock(&g_dhcp_mgr.lock);
            return -NETMGRERR_TIMEOUT;
        }
    }
    ret = item->err;
    pthread_mutex_unlock(&g_dhcp_mgr.lock);
    return ret;
}

/**
 * @brief 查询指定接口的 DHCP 是否仍在运行
 * @param ifname 接口名，不可为空
 * @return 1 运行中；0 已结束或未启动
 */
int netmgr_dhcp_busy(const char* ifname)
{
    int busy = 0; /* 1=运行中 */
    int i = 0;    /* 实例表下标，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0])
    {
        return 0;
    }

    pthread_mutex_lock(&g_dhcp_mgr.lock);
    for (i = 0; i < g_dhcpMax; ++i)
    {
        if (!g_dhcp_mgr.item[i].used)
            continue;
        if (0 != strncmp(g_dhcp_mgr.item[i].ifname, ifname, sizeof(g_dhcp_mgr.item[i].ifname)))
            continue;
        busy = 1;
        break;
    }
    pthread_mutex_unlock(&g_dhcp_mgr.lock);
    return busy;
}
