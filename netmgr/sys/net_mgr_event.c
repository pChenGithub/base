/**
 * @file net_mgr_event.c
 * @brief netmgr 系统适配层：netlink 事件监听实现
 * @details 订阅链路、IPv4 地址与路由变化，把报文解析为结构化事件后交给回调；
 *          本层不做链路状态判定，避免事件处理与上层状态机纠缠。
 */
#include "net_mgr_event.h"

#include "net_mgr_err.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>

/** 接收超时时间（微秒），用于周期性检查退出标志 */
#define EVENT_WAIT_US 500000
/** netlink 接收缓冲（字节），事件密集时避免内核侧丢弃事件 */
#define EVENT_RCVBUF  131072

/** 事件监听上下文，承载本层全部状态 */
typedef struct {
    int fd;               /**< netlink socket，-1 表示未打开 */
    volatile int running; /**< 1=接收线程运行中 */
    pthread_t tid;        /**< 接收线程 */
    netmgr_nev_cb cb;     /**< 事件回调，可为空 */
    void* user;           /**< 回调透传的用户指针，可为空 */
    char buf[8192];       /**< 事件接收缓冲，承载 netlink 单次事件消息（属性多时 4KB 偏紧） */
} NETMGR_EV_CTX;

/**
 * 事件监听上下文（模块内单实例，本层状态全部收敛于此）
 * 用途：保存 netlink socket、接收线程句柄、运行标志、事件回调与接收缓冲
 * 单位/范围：fd 为 socket 描述符或 -1；running 取 0/1；buf 固定 8192 字节
 * 默认值依据：未启动态（fd=-1、running=0、无回调）；8192 覆盖常见链路/路由事件长度
 * 写入方/读取方：open/close 写控制字段，事件线程写 buf 并解析；仅本文件访问
 * 生命周期：仅内存态，随进程存在，close 后控制字段复位为未启动态
 */
static NETMGR_EV_CTX g_event = {.fd = -1};

/**
 * @brief 按接口索引取接口名
 * @param index 接口索引，0 表示未知
 * @param out   输出缓冲，不可为空
 * @param len   输出缓冲长度，需不小于 IF_NAMESIZE
 */
static void event_ifname(unsigned int index, char* out, int len)
{
    if (NULL == out || len <= 0)
        return;
    out[0] = '\0';
    if (0 == index)
        return;
    if (NULL == if_indextoname(index, out))
        out[0] = '\0';
}

/**
 * @brief 解析链路事件：取接口名（属性中的 IFLA_IFNAME 优先）
 * @param nlh 消息头，不可为空
 * @param ev  输出事件，不可为空
 * @return 恒返回 1（已识别）
 */
static int event_link(struct nlmsghdr* nlh, NETMGR_NEV* ev)
{
    struct ifinfomsg* ifi = (struct ifinfomsg*)NLMSG_DATA(nlh); /* 链路消息体 */
    struct rtattr* rta = NULL;                                  /* 属性视图 */
    int rta_len = 0;                                            /* 剩余属性长度 */

    /* 链路上/下线方向由 event_parse 依据 ifi_flags 判定，本函数只负责接口名 */

    event_ifname((unsigned int)ifi->ifi_index, ev->ifname, (int)sizeof(ev->ifname));

    /* 属性中带接口名时以属性为准，覆盖重命名后的名称 */
    rta_len = (int)IFLA_PAYLOAD(nlh);
    for (rta = IFLA_RTA(ifi); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
    {
        if (IFLA_IFNAME != rta->rta_type)
            continue;
        snprintf(ev->ifname, sizeof(ev->ifname), "%s", (const char*)RTA_DATA(rta));
        break;
    }
    return 1;
}

/**
 * @brief 由 IPv4 前缀长度换算点分十进制掩码
 * @param prefixlen 前缀长度，取 0~32
 * @param out       输出缓冲，不可为空
 * @param len       输出缓冲长度
 */
static void event_mask_from_prefix(int prefixlen, char* out, int len)
{
    struct in_addr addr = {0}; /* 掩码二进制值 */

    if (NULL == out || len <= 0)
        return;

    out[0] = '\0';
    if (prefixlen < 0 || prefixlen > 32)
        return;

    /* 32 位全 1 左移，前缀为 0 时单独处理以避免移位越界 */
    addr.s_addr = (0 == prefixlen) ? 0 : htonl(0xFFFFFFFFu << (32 - prefixlen));
    (void)inet_ntop(AF_INET, &addr, out, (socklen_t)len);
}

/**
 * @brief 解析地址事件：取接口名、IPv4 地址（IFA_LOCAL 优先）与接口掩码
 * @param nlh 消息头，不可为空
 * @param ev  输出事件，不可为空
 * @return 1 已识别（IPv4）；0 非 IPv4，忽略
 */
static int event_addr(struct nlmsghdr* nlh, NETMGR_NEV* ev)
{
    struct ifaddrmsg* ifa = (struct ifaddrmsg*)NLMSG_DATA(nlh); /* 地址消息体 */
    struct rtattr* rta = NULL;                                  /* 属性视图 */
    int rta_len = 0;                                            /* 剩余属性长度 */

    event_ifname((unsigned int)ifa->ifa_index, ev->ifname, (int)sizeof(ev->ifname));

    /* 本层只处理 IPv4 地址事件（订阅组也仅 IPv4），其它族直接忽略 */
    if (AF_INET != ifa->ifa_family)
        return 0;

    /* 掩码来自前缀长度，与地址同一条消息，必然可取 */
    event_mask_from_prefix((int)ifa->ifa_prefixlen, ev->u.addr.mask, (int)sizeof(ev->u.addr.mask));

    rta_len = (int)IFA_PAYLOAD(nlh);
    for (rta = IFA_RTA(ifa); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
    {
        if (RTA_PAYLOAD(rta) < (int)sizeof(struct in_addr))
            continue;
        if (IFA_LOCAL == rta->rta_type)
        {
            /* 本端地址优先，但不能提前退出：属性区顺序不保证 */
            (void)inet_ntop(AF_INET, RTA_DATA(rta), ev->u.addr.ip, (socklen_t)sizeof(ev->u.addr.ip));
            continue;
        }
        if (IFA_ADDRESS == rta->rta_type && '\0' == ev->u.addr.ip[0])
            (void)inet_ntop(AF_INET, RTA_DATA(rta), ev->u.addr.ip, (socklen_t)sizeof(ev->u.addr.ip));
    }
    return 1;
}

/**
 * @brief 解析路由事件：取出口接口名、下一跳网关与「是否默认路由」
 * @param nlh 消息头，不可为空
 * @param ev  输出事件，不可为空
 * @return 恒返回 1（已识别）
 */
static int event_route(struct nlmsghdr* nlh, NETMGR_NEV* ev)
{
    struct rtmsg* rtm = (struct rtmsg*)NLMSG_DATA(nlh); /* 路由消息体 */
    struct rtattr* rta = NULL;                          /* 属性视图 */
    int rta_len = 0;                                    /* 剩余属性长度 */
    int oif = 0;                                        /* 出口接口索引 */

    /* 主路由表中目的前缀长度为 0 即为默认路由 */
    ev->u.route.isDefault = (AF_INET == rtm->rtm_family && RT_TABLE_MAIN == (int)rtm->rtm_table &&
                     0 == rtm->rtm_dst_len) ? 1 : 0;

    /* 网关、出口接口与 metric 是三条独立属性，需遍历完整属性区，不能提前退出 */
    rta_len = (int)RTM_PAYLOAD(nlh);
    for (rta = RTM_RTA(rtm); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
    {
        if (RTA_GATEWAY == rta->rta_type &&
            RTA_PAYLOAD(rta) >= (int)sizeof(struct in_addr))
        {
            (void)inet_ntop(AF_INET, RTA_DATA(rta), ev->u.route.gate, (socklen_t)sizeof(ev->u.route.gate));
            continue;
        }
        if (RTA_PRIORITY == rta->rta_type && RTA_PAYLOAD(rta) >= (int)sizeof(int))
        {
            (void)memcpy(&ev->u.route.metric, RTA_DATA(rta), sizeof(ev->u.route.metric));
            continue;
        }
        if (RTA_OIF == rta->rta_type && RTA_PAYLOAD(rta) >= (int)sizeof(int))
            (void)memcpy(&oif, RTA_DATA(rta), sizeof(oif));
    }
    event_ifname((unsigned int)oif, ev->ifname, (int)sizeof(ev->ifname));
    return 1;
}

/**
 * @brief 把一条 netlink 消息解析为结构化事件
 * @param nlh 消息头，不可为空
 * @param ev  输出事件，不可为空
 * @return 1 已识别；0 与本层无关，调用方忽略
 */
static int event_parse(struct nlmsghdr* nlh, NETMGR_NEV* ev)
{
    /* 接收缓冲溢出：内核已丢弃事件，通知调用方做一次全量刷新 */
    if (NLMSG_OVERRUN == nlh->nlmsg_type)
    {
        ev->type = NETMGR_NEV_LOST;
        return 1;
    }

    /* 内核消息只分「新建/变更」与「删除」两类，本身不含 up/down 语义：
       RTM_NEWLINK / RTM_NEWADDR / RTM_NEWROUTE 都是“新增或修改”通知，
       链路方向须按 ifi_flags（IFF_UP + IFF_RUNNING）自行判定；
       carrier 变化经 linkwatch 上报，存在约 1 秒延迟且短时多次变化可能被合并。
       下列注释给出各消息的典型触发命令（便于用 ip 命令手工复现与验证）。 */
    switch (nlh->nlmsg_type)
    {
    case RTM_NEWLINK:
    {
        struct ifinfomsg* ifi = (struct ifinfomsg*)NLMSG_DATA(nlh); /* 链路消息体 */
        /* 接口新建或属性变更，典型来源：
         *   ip link set dev eth0 up                 // 管理态 UP（IFF_UP）
         *   ip link set dev eth0 down               // 管理态 DOWN
         *   ip link set dev eth0 mtu 1500           // 属性变更（链路仍 UP 则判 LINK_UP）
         *   ip link add dev dummy0 type dummy       // 新建接口
         *   网线插拔 / 对端设备重启 / 驱动加载        // carrier 变化，硬件触发
         * 同一消息既报插线也报拔线：IFF_UP（管理态）与 IFF_RUNNING（carrier）
         * 同时置位才算 LINK_UP，其余一律 LINK_DOWN */
        if (!event_link(nlh, ev))
            return 0;
        ev->type = ((0 != (ifi->ifi_flags & IFF_UP)) && (0 != (ifi->ifi_flags & IFF_RUNNING))) ?
                   NETMGR_NEV_LINK_UP : NETMGR_NEV_LINK_DOWN;
        return 1;
    }
    case RTM_DELLINK:
        /* 接口注销，典型来源：
         *   ip link del dev dummy0                  // 删除虚拟 / 桥接口
         *   USB 网卡拔出 / 驱动卸载                  // 设备移除
         * 此时 ifi_index 已失效，ifname 只能靠 IFLA_IFNAME，取不到为空串 */
        ev->type = NETMGR_NEV_LINK_DEL;
        return event_link(nlh, ev);
    case RTM_NEWADDR:
        /* 地址新增或变更，典型来源：
         *   ip addr add 192.168.1.10/24 dev eth0
         *   ip addr change 192.168.1.10/24 dev eth0 // 仅改属性也算 NEWADDR
         *   DHCP 取址成功（udhcpc 脚本内执行 ip addr add）
         *   ip addr flush 后重新配置                        */
        ev->type = NETMGR_NEV_ADDR_ADD;
        return event_addr(nlh, ev);
    case RTM_DELADDR:
        /* 地址删除，典型来源：
         *   ip addr del 192.168.1.10/24 dev eth0
         *   ip addr flush dev eth0
         *   租约到期 / 接口置 down 后内核联动清理                  */
        ev->type = NETMGR_NEV_ADDR_DEL;
        return event_addr(nlh, ev);
    case RTM_NEWROUTE:
        /* 路由新增或变更，典型来源：
         *   ip route add default via 192.168.1.1 dev eth0 metric 100
         *   ip route replace default via 192.168.1.1 dev eth0      // 变更
         *   DHCP 下发默认路由（与地址同一批脚本动作）
         *   netmgr_route_add 本模块下发默认路由（core 侧会按自环抑制上报） */
        ev->type = NETMGR_NEV_ROUTE_ADD;
        return event_route(nlh, ev);
    case RTM_DELROUTE:
        /* 路由删除，典型来源：
         *   ip route del default via 192.168.1.1 dev eth0
         *   接口 down / 地址删除后内核联动清理默认路由
         *   netmgr_route_del 撤销本模块下发的默认路由 */
        ev->type = NETMGR_NEV_ROUTE_DEL;
        return event_route(nlh, ev);
    default:
        return 0;
    }
}

/**
 * @brief 事件接收线程：解析 netlink 报文并回调
 * @param arg 指向事件监听上下文
 * @return 恒定返回空指针
 */
static void* event_thread(void* arg)
{
    NETMGR_EV_CTX* hd = (NETMGR_EV_CTX*)arg; /* 事件监听上下文 */
    struct nlmsghdr* nlh = NULL;           /* 消息视图 */
    NETMGR_NEV nev = {0};                  /* 单条事件内容 */
    struct iovec iov = {0};                /* 接收缓冲视图 */
    struct msghdr msg = {0};               /* recvmsg 参数，用于识别截断 */
    int len = 0;                           /* 本轮有效长度 */
    ssize_t n = 0;                         /* 接收字节数 */

    if (NULL == hd)
        return NULL;

    iov.iov_base = hd->buf;
    iov.iov_len = sizeof(hd->buf);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    while (hd->running)
    {
        /* MSG_TRUNC 让返回值为报文真实长度：大于缓冲即被截断，属性可能残缺 */
        n = recvmsg(hd->fd, &msg, MSG_TRUNC);
        if (n <= 0)
        {
            /* 超时用于周期性检查退出标志，其余错误直接重试 */
            if (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno)
                continue;
            if (!hd->running)
                break;
            usleep(EVENT_WAIT_US);
            continue;
        }

        if (n > (ssize_t)sizeof(hd->buf))
        {
            /* 报文超过接收缓冲：丢弃并告知调用方做一次全量刷新，避免用残缺属性 */
            memset(&nev, 0, sizeof(nev));
            nev.type = NETMGR_NEV_LOST;
            if (NULL != hd->cb)
                hd->cb(&nev, hd->user);
            continue;
        }

        len = (int)n;
        for (nlh = (struct nlmsghdr*)hd->buf; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len))
        {
            memset(&nev, 0, sizeof(nev));
            if (!event_parse(nlh, &nev))
                continue;
            if (NULL != hd->cb)
                hd->cb(&nev, hd->user);
        }
    }
    return NULL;
}

/**
 * @brief 启动 netlink 事件监听（链路、IPv4 地址、路由变化）
 * @param cb   事件回调，可为空
 * @param user 回调透传的用户指针，可为空
 * @return 0 成功（已在运行时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_event_open(netmgr_nev_cb cb, void* user, unsigned int groups)
{
    struct sockaddr_nl addr = {0}; /* 本地 netlink 地址 */
    struct timeval tv = {0};       /* 接收超时 */
    unsigned int mask = groups;    /* 实际订阅组掩码 */
    int rcvbuf = EVENT_RCVBUF;     /* 接收缓冲大小 */
    int fd = -1;                   /* netlink socket */

    if (g_event.running)
        return 0;

    /* 未指定订阅组时使用默认组：链路 + IPv4 地址 + IPv4 路由 */
    if (NETMGR_EVENT_GROUPS_DEFAULT == mask)
        mask = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE;

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return -NETMGRERR_SOCKET;

    /* 调大接收缓冲：事件密集时避免内核侧丢事件；失败则沿用系统默认值 */
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, (socklen_t)sizeof(rcvbuf));

    addr.nl_family = AF_NETLINK;
    /* 监听链路、IPv4 地址与路由变化：路由变化用于感知外部改写默认路由 */
    addr.nl_groups = mask;
    if (0 != bind(fd, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)))
    {
        close(fd);
        return -NETMGRERR_SOCKET;
    }

    /* 设置接收超时，使接收线程能周期性检查退出标志 */
    tv.tv_sec = 0;
    tv.tv_usec = EVENT_WAIT_US;
    if (0 != setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv)))
    {
        /* 无超时则接收线程无法周期性检查退出标志，按异常返回 */
        close(fd);
        return -NETMGRERR_SOCKET;
    }

    g_event.fd = fd;
    g_event.cb = cb;
    g_event.user = user;
    g_event.running = 1;
    if (0 != pthread_create(&g_event.tid, NULL, event_thread, &g_event))
    {
        g_event.running = 0;
        close(fd);
        g_event.fd = -1;
        g_event.cb = NULL;
        g_event.user = NULL;
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
    int ret = 0; /* join 结果，负值为错误码 */

    if (!g_event.running)
        return 0;

    g_event.running = 0;
    /* 线程最长在一个接收超时周期内退出；即使 join 失败也要关闭 socket，
       否则 fd 泄漏（线程读取已关闭 fd 会立即返回并退出） */
    if (0 != pthread_join(g_event.tid, NULL))
        ret = -NETMGRERR_THREAD;

    if (g_event.fd >= 0)
    {
        close(g_event.fd);
        g_event.fd = -1;
    }
    g_event.cb = NULL;
    g_event.user = NULL;
    return ret;
}
