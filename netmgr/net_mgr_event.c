/**
 * @file net_mgr_event.c
 * @brief netlink 事件监听：链路、IPv4 地址与路由变化
 * @details 采用「接收线程 + 唤醒回调」模型，事件只负责唤醒仲裁线程提前刷新，
 *          具体状态判定统一在仲裁层完成，避免事件处理与状态机纠缠。
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "net_mgr_def.h"

/** 事件接收缓冲长度 */
#define EVENT_RSP_LEN 4096
/** 接收超时时间（微秒），用于周期性检查退出标志 */
#define EVENT_WAIT_US 500000

/** 事件监听上下文 */
typedef struct {
    int fd;             /**< netlink socket，-1 表示未打开 */
    volatile int running; /**< 1=接收线程运行中 */
    pthread_t tid;      /**< 接收线程 */
    void (*wake)(void); /**< 事件到达唤醒回调，可为空 */
} NETMGR_EVENT;

/** 事件监听上下文，模块内单实例 */
static NETMGR_EVENT g_event = {-1, 0, 0, NULL};

/**
 * @brief 事件接收线程：把内核事件转换为一次唤醒
 * @param arg 指向事件监听上下文
 * @return 恒定返回空指针
 */
static void* event_thread(void* arg)
{
    NETMGR_EVENT* ev = (NETMGR_EVENT*)arg; /* 事件监听上下文 */
    struct nlmsghdr* nlh = NULL;           /* 消息视图 */
    char buf[EVENT_RSP_LEN];               /* 接收缓冲 */
    int len = 0;                           /* 本轮有效长度 */
    ssize_t n = 0;                         /* 接收字节数 */

    if (NULL == ev)
        return NULL;

    while (ev->running)
    {
        n = recv(ev->fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            /* 超时用于周期性检查退出标志，其余错误记录后继续 */
            if (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno)
                continue;
            if (!ev->running)
                break;
            NETMGR_ERR("接收 netlink 事件失败, errno=%d", errno);
            usleep(EVENT_WAIT_US);
            continue;
        }

        len = (int)n;
        for (nlh = (struct nlmsghdr*)buf; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len))
        {
            switch (nlh->nlmsg_type)
            {
            case RTM_NEWLINK:
            case RTM_DELLINK:
            case RTM_NEWADDR:
            case RTM_DELADDR:
            case RTM_NEWROUTE:
            case RTM_DELROUTE:
                if (NULL != ev->wake)
                    ev->wake();
                break;
            default:
                break;
            }
        }
    }
    return NULL;
}

/**
 * @brief 启动 netlink 事件监听
 * @param wake 事件到达时的唤醒回调，可为空
 * @return 0 成功（已在运行时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_event_open(void (*wake)(void))
{
    struct sockaddr_nl addr; /* 本地 netlink 地址 */
    struct timeval tv;       /* 接收超时 */
    int fd = -1;             /* netlink socket */

    if (g_event.running)
        return 0;

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
    {
        NETMGR_ERR("创建事件 socket 失败, errno=%d", errno);
        return -NETMGRERR_SOCKET;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    /* 监听链路、IPv4 地址与路由变化：路由变化用于感知外部改写默认路由 */
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE;
    if (0 != bind(fd, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)))
    {
        NETMGR_ERR("绑定事件 socket 失败, errno=%d", errno);
        close(fd);
        return -NETMGRERR_SOCKET;
    }

    /* 设置接收超时，使接收线程能周期性检查退出标志 */
    tv.tv_sec = 0;
    tv.tv_usec = EVENT_WAIT_US;
    if (0 != setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv)))
        NETMGR_ERR("设置事件接收超时失败, errno=%d", errno);

    g_event.fd = fd;
    g_event.wake = wake;
    g_event.running = 1;
    if (0 != pthread_create(&g_event.tid, NULL, event_thread, &g_event))
    {
        NETMGR_ERR("创建事件线程失败, errno=%d", errno);
        g_event.running = 0;
        close(fd);
        g_event.fd = -1;
        return -NETMGRERR_THREAD;
    }
    return 0;
}

/**
 * @brief 停止 netlink 事件监听并回收线程
 * @return 0 成功（未启动时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_event_close(void)
{
    if (!g_event.running)
        return 0;

    g_event.running = 0;
    /* 线程最长在一个接收超时周期内退出，join 保证 socket 关闭时不再被使用 */
    if (0 != pthread_join(g_event.tid, NULL))
    {
        NETMGR_ERR("等待事件线程退出失败, errno=%d", errno);
        return -NETMGRERR_THREAD;
    }
    if (g_event.fd >= 0)
    {
        close(g_event.fd);
        g_event.fd = -1;
    }
    g_event.wake = NULL;
    return 0;
}
