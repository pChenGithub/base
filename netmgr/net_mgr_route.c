/**
 * @file net_mgr_route.c
 * @brief 路由管理：基于 rtnetlink 下发、删除与查询带 metric 的 IPv4 默认路由
 * @details 主备切换依赖内核按 metric 选路，因此路由必须带 RTA_PRIORITY，
 *          这是 ioctl(SIOCADDRT) 无法可靠表达的能力。
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "net_mgr_def.h"

/** netlink 请求属性区缓冲长度 */
#define ROUTE_REQ_LEN 256
/** netlink 回复缓冲长度 */
#define ROUTE_RSP_LEN 4096

/** netlink 路由请求布局：消息头 + 路由消息体 + 属性区 */
typedef struct {
    struct nlmsghdr nlh;      /**< netlink 消息头 */
    struct rtmsg rtm;         /**< 路由消息体 */
    char attr[ROUTE_REQ_LEN]; /**< 属性区，按需追加网关/接口/metric */
} NETMGR_RREQ;

/**
 * @brief 打开路由管理用 netlink socket
 * @return 成功返回描述符；失败返回负的 NETMGRERR_* 错误码
 */
static int route_sock_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE); /* 路由控制 socket */

    if (fd < 0)
    {
        NETMGR_ERR("创建路由 socket 失败, errno=%d", errno);
        return -NETMGRERR_SOCKET;
    }
    return fd;
}

/**
 * @brief 向 netlink 消息追加一个路由属性
 * @param nlh  消息头，不可为空
 * @param type 属性类型（RTA_*）
 * @param data 属性内容，不可为空
 * @param len  属性内容长度
 * @param cap  消息缓冲总容量
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int route_attr(struct nlmsghdr* nlh, int type, const void* data, int len, int cap)
{
    struct rtattr* rta = NULL; /* 追加位置的属性视图 */
    int need = 0;              /* 追加后的消息总长度 */

    if (NULL == nlh || NULL == data || len <= 0)
    {
        NETMGR_ERR("入参非法");
        return -NETMGRERR_PARAM;
    }

    need = (int)RTA_ALIGN(nlh->nlmsg_len) + (int)RTA_ALIGN(RTA_LENGTH(len));
    if (need > cap)
    {
        NETMGR_ERR("属性区不足, 需要 %d 字节, 容量 %d", need, cap);
        return -NETMGRERR_BUFF;
    }

    rta = (struct rtattr*)((char*)nlh + RTA_ALIGN(nlh->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)RTA_LENGTH(len);
    memcpy(RTA_DATA(rta), data, (size_t)len);
    nlh->nlmsg_len = (unsigned int)need;
    return 0;
}

/**
 * @brief 发送路由请求并等待内核确认
 * @param fd  netlink socket
 * @param nlh 请求消息，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int route_send(int fd, struct nlmsghdr* nlh)
{
    struct sockaddr_nl addr;   /* 内核 netlink 地址 */
    char rsp[ROUTE_RSP_LEN];   /* 回复缓冲 */
    struct nlmsghdr* msg = NULL; /* 回复消息视图 */
    struct nlmsgerr* err = NULL; /* 错误消息视图 */
    int len = 0;               /* 本轮有效长度 */
    ssize_t n = 0;             /* 接收字节数 */

    if (NULL == nlh)
    {
        NETMGR_ERR("入参 nlh 为空");
        return -NETMGRERR_PARAM;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    if (0 > sendto(fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)))
    {
        NETMGR_ERR("发送路由请求失败, errno=%d", errno);
        return -NETMGRERR_SOCKET;
    }

    /* 等待 ACK：NLMSG_ERROR 中 error 为 0 表示内核执行成功 */
    n = recv(fd, rsp, sizeof(rsp), 0);
    if (n <= 0)
    {
        NETMGR_ERR("接收路由确认失败, errno=%d", errno);
        return -NETMGRERR_SOCKET;
    }

    len = (int)n;
    for (msg = (struct nlmsghdr*)rsp; NLMSG_OK(msg, len); msg = NLMSG_NEXT(msg, len))
    {
        if (NLMSG_ERROR != msg->nlmsg_type)
            continue;
        err = (struct nlmsgerr*)NLMSG_DATA(msg);
        if (0 == err->error)
            return 0;
        errno = -err->error;
        NETMGR_ERR("路由操作被内核拒绝, errno=%d", errno);
        return -NETMGRERR_ROUTE;
    }

    NETMGR_ERR("未收到内核确认消息");
    return -NETMGRERR_ROUTE;
}

/**
 * @brief 从一条路由消息中解析默认路由，保留 metric 最小的一条
 * @param nlh    路由消息，不可为空
 * @param filter 出口接口名过滤，为空表示不过滤
 * @param best   最优路由输出，metric 为 -1 表示尚未命中
 */
static void route_parse(struct nlmsghdr* nlh, const char* filter, NETMGR_ROUTE* best)
{
    struct rtmsg* rtm = NULL;  /* 路由消息体 */
    struct rtattr* rta = NULL; /* 属性视图 */
    struct in_addr gw;         /* 网关地址 */
    char ifname[NETMGR_IF_LEN]; /* 出口接口名 */
    int rta_len = 0;           /* 剩余属性长度 */
    int metric = 0;            /* 路由 metric */
    int oif = 0;               /* 出口接口索引 */
    int has_gw = 0;            /* 1=已解析到网关 */

    rtm = (struct rtmsg*)NLMSG_DATA(nlh);
    /* 只处理主路由表中的 IPv4 默认路由，其它条目与主备无关 */
    if (AF_INET != rtm->rtm_family || RT_TABLE_MAIN != (int)rtm->rtm_table || 0 != rtm->rtm_dst_len)
        return;

    memset(&gw, 0, sizeof(gw));
    memset(ifname, 0, sizeof(ifname));
    rta = RTM_RTA(rtm);
    rta_len = (int)RTM_PAYLOAD(nlh);
    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
    {
        switch (rta->rta_type)
        {
        case RTA_GATEWAY:
            if (RTA_PAYLOAD(rta) >= (int)sizeof(gw))
            {
                memcpy(&gw, RTA_DATA(rta), sizeof(gw));
                has_gw = 1;
            }
            break;
        case RTA_OIF:
            if (RTA_PAYLOAD(rta) >= (int)sizeof(oif))
                memcpy(&oif, RTA_DATA(rta), sizeof(oif));
            break;
        case RTA_PRIORITY:
            if (RTA_PAYLOAD(rta) >= (int)sizeof(metric))
                memcpy(&metric, RTA_DATA(rta), sizeof(metric));
            break;
        default:
            break;
        }
    }

    if (!has_gw || 0 == oif)
        return;
    if (NULL == if_indextoname((unsigned int)oif, ifname))
    {
        NETMGR_ERR("接口索引 %d 转换失败, errno=%d", oif, errno);
        return;
    }
    if (NULL != filter && 0 != strcmp(filter, ifname))
        return;
    /* 已有更优（metric 更小）的路由时不再更新 */
    if (best->metric >= 0 && metric >= best->metric)
        return;

    if (NULL == inet_ntop(AF_INET, &gw, best->gate, (socklen_t)sizeof(best->gate)))
    {
        NETMGR_ERR("网关地址转换失败, errno=%d", errno);
        return;
    }
    snprintf(best->ifname, sizeof(best->ifname), "%s", ifname);
    best->metric = metric;
}

/**
 * @brief 查询默认路由并筛出 metric 最小的一条
 * @param filter 出口接口名过滤，为空表示全表
 * @param best   输出最优路由，不可为空
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
static int route_dump(const char* filter, NETMGR_ROUTE* best)
{
    struct sockaddr_nl addr;     /* 内核 netlink 地址 */
    struct nlmsghdr* nlh = NULL; /* 请求消息视图 */
    struct rtmsg* rtm = NULL;    /* 请求路由消息体 */
    char rsp[ROUTE_RSP_LEN];     /* 收发缓冲 */
    int fd = -1;                 /* netlink socket */
    int len = 0;                 /* 本轮有效长度 */
    int done = 0;                /* 1=dump 结束 */
    ssize_t n = 0;               /* 接收字节数 */
    int ret = 0;                 /* 返回值 */

    if (NULL == best)
    {
        NETMGR_ERR("入参 best 为空");
        return -NETMGRERR_PARAM;
    }
    best->metric = -1;

    fd = route_sock_open();
    if (fd < 0)
        return fd;

    /* 构造 RTM_GETROUTE dump 请求，一次性取回主路由表 */
    memset(rsp, 0, sizeof(rsp));
    nlh = (struct nlmsghdr*)rsp;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nlh->nlmsg_type = RTM_GETROUTE;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = 1;
    rtm = (struct rtmsg*)NLMSG_DATA(nlh);
    rtm->rtm_family = AF_INET;

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    if (0 > sendto(fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)))
    {
        NETMGR_ERR("发送路由查询失败, errno=%d", errno);
        close(fd);
        return -NETMGRERR_SOCKET;
    }

    /* 持续接收直到 NLMSG_DONE，逐条筛选默认路由 */
    while (!done)
    {
        n = recv(fd, rsp, sizeof(rsp), 0);
        if (n <= 0)
        {
            NETMGR_ERR("接收路由查询结果失败, errno=%d", errno);
            close(fd);
            return -NETMGRERR_SOCKET;
        }

        len = (int)n;
        for (nlh = (struct nlmsghdr*)rsp; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len))
        {
            if (NLMSG_DONE == nlh->nlmsg_type)
            {
                done = 1;
                break;
            }
            if (NLMSG_ERROR == nlh->nlmsg_type)
            {
                NETMGR_ERR("路由查询返回错误消息");
                close(fd);
                return -NETMGRERR_ROUTE;
            }
            if (RTM_NEWROUTE != nlh->nlmsg_type)
                continue;
            route_parse(nlh, filter, best);
        }
    }

    close(fd);
    ret = (best->metric >= 0) ? 0 : -NETMGRERR_NOTFOUND;
    return ret;
}

/**
 * @brief 下发默认路由（带 metric）
 * @param ifname 出口接口名，不可为空
 * @param gate   网关地址，不可为空
 * @param metric 路由 metric，越小越优先
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_add(const char* ifname, const char* gate, int metric)
{
    NETMGR_RREQ req;          /* 请求缓冲 */
    struct in_addr gw;        /* 网关地址 */
    unsigned int ifindex = 0; /* 出口接口索引 */
    int fd = -1;              /* netlink socket */
    int ret = 0;              /* 子步骤返回值 */

    if (NULL == ifname || NULL == gate || !netmgr_ip_valid(gate) || metric < 0)
    {
        NETMGR_ERR("入参非法, ifname=%s", (NULL == ifname) ? "" : ifname);
        return -NETMGRERR_PARAM;
    }

    ifindex = if_nametoindex(ifname);
    if (0 == ifindex)
    {
        NETMGR_ERR("接口 %s 不存在", ifname);
        return -NETMGRERR_NOTFOUND;
    }

    /* 构造默认路由：dst 长度为 0 即 default，metric 由 RTA_PRIORITY 承载 */
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nlh.nlmsg_type = RTM_NEWROUTE;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK;
    req.nlh.nlmsg_seq = 1;
    req.rtm.rtm_family = AF_INET;
    req.rtm.rtm_table = RT_TABLE_MAIN;
    req.rtm.rtm_protocol = RTPROT_STATIC;
    req.rtm.rtm_scope = RT_SCOPE_UNIVERSE;
    req.rtm.rtm_type = RTN_UNICAST;
    req.rtm.rtm_dst_len = 0;

    memset(&gw, 0, sizeof(gw));
    if (1 != inet_pton(AF_INET, gate, &gw))
    {
        NETMGR_ERR("转换网关 %s 失败", gate);
        return -NETMGRERR_PARAM;
    }

    ret = route_attr(&req.nlh, RTA_GATEWAY, &gw, (int)sizeof(gw), (int)sizeof(req));
    if (ret < 0)
        return ret;
    ret = route_attr(&req.nlh, RTA_OIF, &ifindex, (int)sizeof(ifindex), (int)sizeof(req));
    if (ret < 0)
        return ret;
    ret = route_attr(&req.nlh, RTA_PRIORITY, &metric, (int)sizeof(metric), (int)sizeof(req));
    if (ret < 0)
        return ret;

    fd = route_sock_open();
    if (fd < 0)
        return fd;

    ret = route_send(fd, &req.nlh);
    close(fd);
    if (ret < 0)
    {
        NETMGR_ERR("下发默认路由失败, ifname=%s, gate=%s, metric=%d", ifname, gate, metric);
        return ret;
    }
    return 0;
}

/**
 * @brief 删除默认路由（精确匹配接口、网关与 metric）
 * @param ifname 出口接口名，不可为空
 * @param gate   网关地址，不可为空
 * @param metric 路由 metric
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_del(const char* ifname, const char* gate, int metric)
{
    NETMGR_RREQ req;          /* 请求缓冲 */
    struct in_addr gw;        /* 网关地址 */
    unsigned int ifindex = 0; /* 出口接口索引 */
    int fd = -1;              /* netlink socket */
    int ret = 0;              /* 子步骤返回值 */

    if (NULL == ifname || NULL == gate || !netmgr_ip_valid(gate) || metric < 0)
    {
        NETMGR_ERR("入参非法, ifname=%s", (NULL == ifname) ? "" : ifname);
        return -NETMGRERR_PARAM;
    }

    ifindex = if_nametoindex(ifname);
    if (0 == ifindex)
    {
        NETMGR_ERR("接口 %s 不存在", ifname);
        return -NETMGRERR_NOTFOUND;
    }

    /* 删除时必须带 metric 与网关，否则可能误删其它链路的默认路由 */
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nlh.nlmsg_type = RTM_DELROUTE;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq = 1;
    req.rtm.rtm_family = AF_INET;
    req.rtm.rtm_table = RT_TABLE_MAIN;
    req.rtm.rtm_protocol = RTPROT_STATIC;
    req.rtm.rtm_scope = RT_SCOPE_UNIVERSE;
    req.rtm.rtm_type = RTN_UNICAST;
    req.rtm.rtm_dst_len = 0;

    memset(&gw, 0, sizeof(gw));
    if (1 != inet_pton(AF_INET, gate, &gw))
    {
        NETMGR_ERR("转换网关 %s 失败", gate);
        return -NETMGRERR_PARAM;
    }

    ret = route_attr(&req.nlh, RTA_GATEWAY, &gw, (int)sizeof(gw), (int)sizeof(req));
    if (ret < 0)
        return ret;
    ret = route_attr(&req.nlh, RTA_OIF, &ifindex, (int)sizeof(ifindex), (int)sizeof(req));
    if (ret < 0)
        return ret;
    ret = route_attr(&req.nlh, RTA_PRIORITY, &metric, (int)sizeof(metric), (int)sizeof(req));
    if (ret < 0)
        return ret;

    fd = route_sock_open();
    if (fd < 0)
        return fd;

    ret = route_send(fd, &req.nlh);
    close(fd);
    if (ret < 0)
    {
        NETMGR_ERR("删除默认路由失败, ifname=%s, gate=%s, metric=%d", ifname, gate, metric);
        return ret;
    }
    return 0;
}

/**
 * @brief 查询当前生效（metric 最小）的默认路由
 * @param out 输出路由信息，不可为空
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_act(NETMGR_ROUTE* out)
{
    return route_dump(NULL, out);
}

/**
 * @brief 查询指定接口上的默认路由（取 metric 最小的一条）
 * @param ifname 出口接口名，不可为空
 * @param out    输出路由信息，不可为空
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_dev(const char* ifname, NETMGR_ROUTE* out)
{
    if (NULL == ifname || '\0' == ifname[0])
    {
        NETMGR_ERR("入参 ifname 非法");
        return -NETMGRERR_PARAM;
    }
    return route_dump(ifname, out);
}
