/**
 * @file net_mgr_dhcp.c
 * @brief DHCP 取址管理：按接口维护独立的 udhcpc 实例
 * @details 每个实例由「子进程 + 输出收尾线程」组成，支持多条链路并发取址；
 *          取址结果由收尾线程判定并释放槽位，仲裁层通过状态刷新感知。
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "net_mgr_def.h"

/** udhcpc 输出单行读取缓冲长度 */
#define DHCP_LINE_LEN 256
/** 停止取址时等待收尾线程释放槽位的超时（秒） */
#define DHCP_STOP_S   3
/** 子进程重定向输出失败时的退出码 */
#define DHCP_EXIT_DUP 126
/** 子进程执行 udhcpc 失败时的退出码 */
#define DHCP_EXIT_EXEC 127

/** 单个 DHCP 实例 */
typedef struct {
    int used;                   /**< 1=实例被占用 */
    int fd;                     /**< 子进程输出管道读端，-1 表示无效 */
    pid_t pid;                  /**< udhcpc 子进程号，-1 表示无 */
    pthread_t tid;              /**< 输出收尾线程 */
    char ifname[NETMGR_IF_LEN]; /**< 接口名 */
} NETMGR_DHCP;

/** DHCP 实例表，按接口占用 */
static NETMGR_DHCP g_dhcp[NETMGR_DHCP_MAX];
/** 实例表互斥锁，管理线程与收尾线程共同访问 */
static pthread_mutex_t g_dhcp_lock = PTHREAD_MUTEX_INITIALIZER;
/** 实例表状态变化条件，用于停止时等待收尾线程释放槽位 */
static pthread_cond_t g_dhcp_cond = PTHREAD_COND_INITIALIZER;

/**
 * @brief DHCP 输出收尾线程：排空子进程输出、回收子进程并释放槽位
 * @param arg 指向占用的 NETMGR_DHCP 实例
 * @return 恒定返回空指针
 */
static void* dhcp_reap(void* arg)
{
    NETMGR_DHCP* item = (NETMGR_DHCP*)arg; /* 当前实例 */
    char line[DHCP_LINE_LEN];              /* 单行输出缓冲 */
    FILE* fp = NULL;                       /* 管道流视图 */
    int status = 0;                        /* 子进程退出状态 */
    int no_lease = 0;                      /* 1=已判定未取得租约 */

    if (NULL == item)
        return NULL;

    fp = fdopen(item->fd, "r");
    if (NULL == fp)
    {
        NETMGR_ERR("%s 输出管道打开失败, errno=%d", item->ifname, errno);
        close(item->fd);
        item->fd = -1;
    }
    else
    {
        /* 排空输出：既判断取址结果，也避免管道写满阻塞 udhcpc */
        while (NULL != fgets(line, (int)sizeof(line), fp))
        {
            if (NULL != strstr(line, "No lease"))
            {
                no_lease = 1;
                NETMGR_ERR("%s 未取得租约: %s", item->ifname, line);
            }
        }
        fclose(fp);
        item->fd = -1;
    }

    if (item->pid > 0)
    {
        if (item->pid != waitpid(item->pid, &status, 0))
            NETMGR_ERR("%s 回收 udhcpc(%d) 失败, errno=%d", item->ifname, (int)item->pid, errno);
        else if (!no_lease && !(WIFEXITED(status) && 0 == WEXITSTATUS(status)))
            NETMGR_ERR("%s udhcpc 异常结束, status=0x%x", item->ifname, status);
    }

    /* 收尾：释放槽位并通知等待方，后续同接口可再次取址 */
    pthread_mutex_lock(&g_dhcp_lock);
    item->used = 0;
    item->pid = -1;
    item->fd = -1;
    pthread_cond_broadcast(&g_dhcp_cond);
    pthread_mutex_unlock(&g_dhcp_lock);
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
        NETMGR_ERR("入参非法");
        return -NETMGRERR_PARAM;
    }

    /* 第 1 步：查重并占位，占位后立即释放锁，避免阻塞其它链路取址 */
    pthread_mutex_lock(&g_dhcp_lock);
    for (i = 0; i < NETMGR_DHCP_MAX; ++i)
    {
        if (g_dhcp[i].used &&
            0 == strncmp(g_dhcp[i].ifname, ifname, sizeof(g_dhcp[i].ifname)))
        {
            pthread_mutex_unlock(&g_dhcp_lock);
            return 0;
        }
        if (!g_dhcp[i].used && idx < 0)
            idx = i;
    }
    if (idx < 0)
    {
        pthread_mutex_unlock(&g_dhcp_lock);
        NETMGR_ERR("DHCP 实例已满, 上限=%d", NETMGR_DHCP_MAX);
        return -NETMGRERR_DHCP;
    }
    item = &g_dhcp[idx];
    item->used = 1;
    snprintf(item->ifname, sizeof(item->ifname), "%s", ifname);
    pthread_mutex_unlock(&g_dhcp_lock);

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
        NETMGR_ERR("%s 创建管道失败, errno=%d", ifname, errno);
        goto fail;
    }

    /* 第 3 步：派生 udhcpc，子进程把输出并入管道后替换自身 */
    pid = fork();
    if (pid < 0)
    {
        NETMGR_ERR("%s 派生 udhcpc 失败, errno=%d", ifname, errno);
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
        NETMGR_ERR("%s 初始化线程属性失败, errno=%d", ifname, errno);
        close(pfd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        goto fail;
    }
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (0 != pthread_create(&item->tid, &attr, dhcp_reap, item))
    {
        pthread_attr_destroy(&attr);
        NETMGR_ERR("%s 创建收尾线程失败, errno=%d", ifname, errno);
        close(pfd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        goto fail;
    }
    pthread_attr_destroy(&attr);
    return 0;

fail:
    /* 任一步骤失败都回收到同一出口，先释放槽位再返回，避免槽位泄漏 */
    NETMGR_ERR("%s DHCP 启动失败, 回收实例槽位", ifname);
    pthread_mutex_lock(&g_dhcp_lock);
    item->used = 0;
    item->fd = -1;
    item->pid = -1;
    pthread_cond_broadcast(&g_dhcp_cond);
    pthread_mutex_unlock(&g_dhcp_lock);
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
    struct timespec ts;       /* 等待槽位释放的截止时间 */
    pid_t pid = -1;           /* 待结束的子进程号 */
    int found = 0;            /* 1=找到运行中的实例 */
    int i = 0;                /* 实例表下标，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0])
    {
        NETMGR_ERR("入参 ifname 非法");
        return -NETMGRERR_PARAM;
    }

    /* 先在锁内取出实例与子进程号，结束进程时不能持锁，否则收尾线程无法释放槽位 */
    pthread_mutex_lock(&g_dhcp_lock);
    for (i = 0; i < NETMGR_DHCP_MAX; ++i)
    {
        if (!g_dhcp[i].used)
            continue;
        if (0 != strncmp(g_dhcp[i].ifname, ifname, sizeof(g_dhcp[i].ifname)))
            continue;
        item = &g_dhcp[i];
        pid = item->pid;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_dhcp_lock);

    if (!found)
        return 0;

    if (pid > 0 && 0 != kill(pid, SIGTERM))
    {
        NETMGR_ERR("%s 结束 udhcpc(%d) 失败, errno=%d", ifname, (int)pid, errno);
        return -NETMGRERR_PROC;
    }

    /* 等待收尾线程释放槽位：线程已 detach，只能以其占位状态判断是否收尾完成 */
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += DHCP_STOP_S;
    pthread_mutex_lock(&g_dhcp_lock);
    while (item->used)
    {
        if (0 != pthread_cond_timedwait(&g_dhcp_cond, &g_dhcp_lock, &ts))
        {
            pthread_mutex_unlock(&g_dhcp_lock);
            NETMGR_ERR("%s 等待 DHCP 收尾超时", ifname);
            return -NETMGRERR_TIMEOUT;
        }
    }
    pthread_mutex_unlock(&g_dhcp_lock);
    return 0;
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
        NETMGR_ERR("入参 ifname 非法");
        return 0;
    }

    pthread_mutex_lock(&g_dhcp_lock);
    for (i = 0; i < NETMGR_DHCP_MAX; ++i)
    {
        if (!g_dhcp[i].used)
            continue;
        if (0 != strncmp(g_dhcp[i].ifname, ifname, sizeof(g_dhcp[i].ifname)))
            continue;
        busy = 1;
        break;
    }
    pthread_mutex_unlock(&g_dhcp_lock);
    return busy;
}
