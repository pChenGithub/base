/**
 * @file net_mgr_dhcp.c
 * @brief netmgr 系统适配层：DHCP 取址实现
 * @details 结构高度对齐 netopt 的 net_dhcp：
 *          1) 命令字符串 + popen 执行 + 逐行判定（对应 exeShellWait + hand_retstr）；
 *          2) 每个接口一个线程，线程内回调 RUNNING -> SUCC/FAIL，FAIL 时可按回调要求重试；
 *          3) 槽位表 + 脏标志表示占用，取址线程退出时清脏标志并广播。
 *          与 netopt 的差异：本层不打日志、失败以错误码返回；停止不使用 pthread_cancel
 *          （会泄漏 popen 管道与子进程），改为按 udhcpc pidfile 结束取址进程后等线程自收尾。
 */
/* 行判定需用到 strcasestr，属 GNU 扩展，须在包含系统头之前打开 */
#define _GNU_SOURCE

#include "net_mgr_dhcp.h"

#include "net_mgr_err.h"
#include "net_mgr_limits.h"

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
/** udhcpc pidfile 路径前缀，用于停止时精确结束取址进程 */
#define DHCP_PIDFILE_PRE    "/tmp/.netmgr_udhcpc_"

/** 行判定结果，语义与 netopt 的 SHELL_RET_TYPE 对应 */
typedef enum {
    DHCP_LINE_IGNORE = 0, /**< 与本层无关，继续读下一行 */
    DHCP_LINE_FAIL,       /**< 判定为取址失败，停止读取 */
    DHCP_LINE_OK          /**< 判定为取址成功，停止读取 */
} DHCP_LINE_RET;

/** 行判定回调，对应 netopt 的 shell_ret_line */
typedef DHCP_LINE_RET (*dhcp_line_cb)(const char* linestr);

/** 单个 DHCP 实例，字段与 netopt 的 DHCP_PTHREAD 对应 */
typedef struct {
    int flag;                   /**< 槽位标志，DIRTY_FLAG 置位表示线程占用中 */
    pthread_t tid;              /**< 取址线程 */
    int timeout;                /**< 单次请求超时秒数，0 表示由 udhcpc 自行决定 */
    netmgr_dhcp_cb hand;        /**< 取址状态回调，对应 netopt 的 hand */
    void* user;                 /**< 回调透传的用户指针，可为空 */
    volatile int stopReq;       /**< 1=已请求停止，线程不再回调取址结果 */
    char ifname[NETMGR_IF_LEN]; /**< 接口名 */
    char cmdstr[CMDSTR_LEN];    /**< 执行命令，对应 netopt 的 cmdstr */
    char pidfile[CMDSTR_LEN];   /**< udhcpc pidfile 路径，停止时按此精确定位进程 */
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
 * @brief 输出行判定：识别 udhcpc 的失败输出
 * @details 对应 netopt 的 hand_retstr，只判失败、不判成功，
 *          未命中即继续读取，读完后按「未捕获失败」视为成功
 * @param linestr 单行输出，不可为空
 * @return DHCP_LINE_FAIL 判定失败；其余忽略
 */
static DHCP_LINE_RET dhcp_line_check(const char* linestr)
{
    if (NULL == linestr)
        return DHCP_LINE_IGNORE;

    if (NULL != strcasestr(linestr, "No lease, failing"))
        return DHCP_LINE_FAIL;

    return DHCP_LINE_IGNORE;
}

/**
 * @brief 执行命令并逐行判定结果，对应 netopt 的 exeShellWait
 * @param cmd     待执行命令，不可为空
 * @param handret 行判定回调，可为空
 * @return 0 未捕获失败；负值为 NETMGRERR_* 错误码（一行未读到视为超时）
 */
static int dhcp_shell_wait(const char* cmd, dhcp_line_cb handret)
{
    FILE* fp = NULL;                           /* 管道流 */
    char line[RETSTR_BUFF_LEN] = {0};          /* 单行输出缓冲 */
    int ret = -NETMGRERR_TIMEOUT;              /* 未读到任何输出时按超时处理 */

    if (NULL == cmd || '\0' == cmd[0])
        return -NETMGRERR_PARAM;

    /* 命令内已合并 stderr，此处只读标准输出 */
    fp = popen(cmd, "r");
    if (NULL == fp)
        return -NETMGRERR_PROC;

    if (NULL != handret)
    {
        while (NULL != fgets(line, (int)sizeof(line) - 1, fp))
        {
            if (DHCP_LINE_FAIL == handret(line))
            {
                ret = -NETMGRERR_DHCP;
                break;
            }
        }
    }

    (void)pclose(fp);
    return ret;
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
 * @brief 拼装取址命令，对应 netopt 的 cmdstr 组装
 * @param item 实例，不可为空
 */
static void dhcp_build_cmd(NETMGR_DHCP* item)
{
    int len = 0; /* 已写入长度 */

    if (NULL == item)
        return;

    len = snprintf(item->cmdstr, sizeof(item->cmdstr), "udhcpc -i %s -p %s",
                   item->ifname, item->pidfile);
    if (len > 0 && len < (int)sizeof(item->cmdstr) && item->timeout > 0)
    {
        len += snprintf(item->cmdstr + len, sizeof(item->cmdstr) - (size_t)len,
                        " -nq -T %d", item->timeout);
    }
    if (len > 0 && len < (int)sizeof(item->cmdstr))
        (void)snprintf(item->cmdstr + len, sizeof(item->cmdstr) - (size_t)len, " 2>&1");

    item->cmdstr[sizeof(item->cmdstr) - 1] = '\0';
}

/**
 * @brief 取址线程：执行命令、回调状态、按回调要求重试，退出时清脏标志
 * @param arg 指向占用的 NETMGR_DHCP 实例
 * @return 恒定返回空指针
 */
static void* dhcp_run(void* arg)
{
    NETMGR_DHCP* item = (NETMGR_DHCP*)arg; /* 当前实例 */
    NETMGR_DHCP_RET answer = NETMGR_DHCP_RET_OK; /* 回调处理结果 */
    int ret = 0;                                 /* 本次执行结果 */

    if (NULL == item)
        return NULL;

    /* 命令与「已发起取址」状态只与接口配置有关：超时等参数在启动时一次性确定，
       运行期不存在调整入口，因此命令只构建一次、RUNNING 只上报一次，重试直接复用 */
    dhcp_build_cmd(item);
    if (NULL != item->hand)
        (void)item->hand(NETMGR_DHCP_RUNNING, item->ifname, 0, item->user);

    do
    {
        ret = dhcp_shell_wait(item->cmdstr, dhcp_line_check);

        /* 主动停止：结果无意义，不再回调 */
        if (item->stopReq)
            break;

        if (ret < 0 && -NETMGRERR_TIMEOUT != ret)
        {
            /* 失败原因只经回调上报；是否需要重取、重取多少次均由调用方决定 */
            answer = (NULL != item->hand) ?
                     item->hand(NETMGR_DHCP_FAIL, item->ifname, ret, item->user) :
                     NETMGR_DHCP_RET_OK;
            if (NETMGR_DHCP_RET_AGAIN == answer && !item->stopReq)
                continue;
            break;
        }

        /* 未捕获失败输出（含 udhcpc 后台化后 shell 正常返回）视为取址成功 */
        if (NULL != item->hand)
            (void)item->hand(NETMGR_DHCP_SUCC, item->ifname, 0, item->user);
        break;
    } while (1);

    /* 退出：清脏标志并唤醒等待方，接口名保留供下次复用 */
    pthread_mutex_lock(&g_dhcp_mgr.lock);
    item->flag &= ~DIRTY_FLAG;
    pthread_cond_broadcast(&g_dhcp_mgr.cond);
    pthread_mutex_unlock(&g_dhcp_mgr.lock);
    return NULL;
}

/**
 * @brief 在指定接口上启动 DHCP 取址，结果经回调返回
 * @param ifname  接口名，不可为空
 * @param timeout 单次请求超时秒数，0 表示由 udhcpc 自行决定
 * @param cb      取址状态回调，可为空
 * @param user    回调透传的用户指针，可为空
 * @return 0 已受理；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_start(const char* ifname, int timeout, netmgr_dhcp_cb cb, void* user)
{
    NETMGR_DHCP* item = NULL; /* 本次占用的实例 */
    int idx = -1;             /* 首个空闲槽位下标 */
    int i = 0;                /* 实例表下标，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0] || timeout < 0)
        return -NETMGRERR_PARAM;

    /* 第 1 步：查重并挑槽位，对应 netopt 的脏标志遍历 */
    pthread_mutex_lock(&g_dhcp_mgr.lock);
    for (i = 0; i < g_dhcpMax; ++i)
    {
        if ((DIRTY_FLAG & g_dhcp_mgr.item[i].flag) &&
            0 == strncmp(g_dhcp_mgr.item[i].ifname, ifname, sizeof(g_dhcp_mgr.item[i].ifname)))
        {
            pthread_mutex_unlock(&g_dhcp_mgr.lock);
            return -NETMGRERR_DHCP; /* 该接口已有取址线程 */
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
    item->timeout = timeout;
    item->hand = cb;
    item->user = user;
    item->stopReq = 0;
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
 * @brief 停止指定接口上的 DHCP 取址，并回收最近一次取址结果
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
 * @brief 查询指定接口的 DHCP 是否仍在取址中
 * @param ifname 接口名，不可为空
 * @return 1 取址线程运行中；0 已结束或未启动
 */
int netmgr_dhcp_busy(const char* ifname)
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
