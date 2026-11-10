/**
 * @file net_mgr_route.c
 * @brief netmgr 系统适配层：默认路由管理实现（rtnetlink）
 * @details 主备切换依赖内核按 metric 选路，因此路由必须带 RTA_PRIORITY。
 *          收发缓冲为文件级单实例，调用方需保证串行调用（core 层以 g_core.lock 保证）。
 */
#include "net_mgr_route.h"

#include "net_mgr_err.h"
#include "net_mgr_util.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

/** 路由层上下文，承载本层全部状态 */
typedef struct {
    char buf[4096]; /**< RTM_GETROUTE 请求与内核 dump 回复的收发缓冲 */
} NETMGR_RT_CTX;

/**
 * 路由层上下文（模块内单实例，本层状态全部收敛于此）
 * 用途：保存路由 request/reply 收发缓冲，供 route_send 与 route_dump 复用
 * 单位/范围：缓冲固定 4096 字节（单次 dump 分片小于该值）
 * 默认值依据：静态零初始化；4096 为 netlink 单次读取常用长度，且超栈上限故下沉
 * 写入方/读取方：仅 core 仲裁线程经 route_send/route_dump 串行读写（持 g_core.lock）
 * 生命周期：仅内存态，随进程存在，不持久化
 */
static NETMGR_RT_CTX g_rt_ctx;

/** 清理默认路由的防御性上限（条）：防止异常情况下反复删同一条导致死循环 */
#define ROUTE_DEF_CLEAR_MAX 8

/** netlink 路由请求布局：消息头 + 路由消息体 + 属性区 */
typedef struct {
    struct nlmsghdr nlh;      /**< netlink 消息头 */
    struct rtmsg rtm;         /**< 路由消息体 */
    char attr[256]; /**< 属性区，按需追加网关/接口/metric */
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
        return -NETMGRERR_PARAM;
    }

    need = (int)RTA_ALIGN(nlh->nlmsg_len) + (int)RTA_ALIGN(RTA_LENGTH(len));
    if (need > cap)
    {
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
 * @param fd      netlink socket
 * @param nlh     请求消息，不可为空
 * @param lenient 1=删除类请求，ENOENT（路由已不存在）视为成功
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int route_send(int fd, struct nlmsghdr* nlh, int lenient)
{
    struct sockaddr_nl addr = {0};   /* 内核 netlink 地址 */
    struct nlmsghdr* msg = NULL;     /* 回复消息视图 */
    struct nlmsgerr* err = NULL;     /* 错误消息视图 */
    int len = 0;                     /* 本轮有效长度 */
    ssize_t n = 0;                   /* 接收字节数 */

    if (NULL == nlh)
    {
        return -NETMGRERR_PARAM;
    }

    addr.nl_family = AF_NETLINK;
    if (0 > sendto(fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)))
    {
        return -NETMGRERR_SOCKET;
    }

    /* 等待 ACK：NLMSG_ERROR 中 error 为 0 表示内核执行成功 */
    n = recv(fd, g_rt_ctx.buf, sizeof(g_rt_ctx.buf), 0);
    if (n <= 0)
    {
        return -NETMGRERR_SOCKET;
    }

    len = (int)n;
    for (msg = (struct nlmsghdr*)g_rt_ctx.buf; NLMSG_OK(msg, len); msg = NLMSG_NEXT(msg, len))
    {
        if (NLMSG_ERROR != msg->nlmsg_type)
            continue;
        err = (struct nlmsgerr*)NLMSG_DATA(msg);
        if (0 == err->error)
            return 0;
        /* 幂等删除：目标路由本就不存在（ENOENT / ESRCH），视为已达成，
           避免切换过程中误报失败 */
        if (0 != lenient && (-ENOENT == err->error || -ESRCH == err->error))
            return 0;
        errno = -err->error;
        return -NETMGRERR_ROUTE;
    }

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
    struct rtmsg* rtm = NULL;         /* 路由消息体 */
    struct rtattr* rta = NULL;        /* 属性视图 */
    struct rtnexthop* nh = NULL;      /* 多路径下一跳视图 */
    struct rtattr* mta = NULL;        /* 多路径下一跳的属性视图 */
    struct in_addr gw = {0};          /* 网关地址 */
    char ifname[16] = {0};            /* 出口接口名 */
    int rta_len = 0;                  /* 剩余属性长度 */
    int nh_len = 0;                   /* 剩余下一跳长度 */
    int mta_len = 0;                  /* 剩余下一跳属性长度 */
    int metric = 0;                   /* 路由 metric */
    int table = 0;                    /* 路由表号：RTA_TABLE 优先，缺省取 rtm_table */
    int oif = 0;                      /* 出口接口索引 */
    int has_gw = 0;                   /* 1=已解析到网关 */

    rtm = (struct rtmsg*)NLMSG_DATA(nlh);
    /* 只处理 IPv4 默认路由；表号在解析属性后再判定（老内核用 rtm_table，
       新内核对非 main 表会把表号放进 RTA_TABLE 而 rtm_table 为 UNSPEC） */
    if (AF_INET != rtm->rtm_family || 0 != rtm->rtm_dst_len)
        return;
    table = (int)rtm->rtm_table;

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
        case RTA_TABLE:
            if (RTA_PAYLOAD(rta) >= (int)sizeof(table))
                memcpy(&table, RTA_DATA(rta), sizeof(table));
            break;
        case RTA_MULTIPATH:
            /* 等价多路径条目：网关与出口在下一跳里，顶层没有 RTA_GATEWAY/RTA_OIF；
               取第一个带网关的下一跳作为该条目的代表（与 ip route 显示顺序一致） */
            nh = (struct rtnexthop*)RTA_DATA(rta);
            nh_len = (int)RTA_PAYLOAD(rta);
            for (; 0 == has_gw && RTNH_OK(nh, nh_len); nh = RTNH_NEXT(nh))
            {
                mta = RTNH_DATA(nh);
                mta_len = (int)(nh->rtnh_len - RTNH_LENGTH(0));
                for (; RTA_OK(mta, mta_len); mta = RTA_NEXT(mta, mta_len))
                {
                    if (RTA_GATEWAY == mta->rta_type && RTA_PAYLOAD(mta) >= (int)sizeof(gw))
                    {
                        memcpy(&gw, RTA_DATA(mta), sizeof(gw));
                        oif = nh->rtnh_ifindex;
                        has_gw = 1;
                        break;
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    /* 只认主路由表：表号可能来自 rtm_table，也可能来自 RTA_TABLE */
    if (RT_TABLE_MAIN != table)
        return;

    if (!has_gw || 0 == oif)
        return;
    if (NULL == if_indextoname((unsigned int)oif, ifname))
    {
        return;
    }
    if (NULL != filter && 0 != strcmp(filter, ifname))
        return;
    /* 已有更优（metric 更小）的路由时不再更新 */
    if (best->metric >= 0 && metric >= best->metric)
        return;

    if (NULL == inet_ntop(AF_INET, &gw, best->gate, (socklen_t)sizeof(best->gate)))
    {
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
    struct sockaddr_nl addr = {0}; /* 内核 netlink 地址 */
    struct nlmsghdr* nlh = NULL;   /* 请求消息视图 */
    struct rtmsg* rtm = NULL;      /* 请求路由消息体 */
    int fd = -1;                   /* netlink socket */
    int len = 0;                   /* 本轮有效长度 */
    int done = 0;                  /* 1=dump 结束 */
    ssize_t n = 0;                 /* 接收字节数 */
    int ret = 0;                   /* 返回值 */

    if (NULL == best)
    {
        return -NETMGRERR_PARAM;
    }
    best->metric = -1;

    fd = route_sock_open();
    if (fd < 0)
        return fd;

    /* 构造 RTM_GETROUTE dump 请求，一次性取回主路由表；
       必须先清零请求区：本缓冲同时用于接收应答，残留字节会成为 rtmsg 的
       过滤字段（如 rtm_dst_len/rtm_table），导致内核返回空结果、查询永远失败 */
    nlh = (struct nlmsghdr*)g_rt_ctx.buf;
    memset(nlh, 0, NLMSG_LENGTH(sizeof(struct rtmsg)));
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nlh->nlmsg_type = RTM_GETROUTE;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = 1;
    rtm = (struct rtmsg*)NLMSG_DATA(nlh);
    rtm->rtm_family = AF_INET;

    addr.nl_family = AF_NETLINK;
    if (0 > sendto(fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)))
    {
        close(fd);
        return -NETMGRERR_SOCKET;
    }

    /* 持续接收直到 NLMSG_DONE，逐条筛选默认路由 */
    while (!done)
    {
        n = recv(fd, g_rt_ctx.buf, sizeof(g_rt_ctx.buf), 0);
        if (n <= 0)
        {
            close(fd);
            return -NETMGRERR_SOCKET;
        }

        len = (int)n;
        for (nlh = (struct nlmsghdr*)g_rt_ctx.buf; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len))
        {
            if (NLMSG_DONE == nlh->nlmsg_type)
            {
                done = 1;
                break;
            }
            if (NLMSG_ERROR == nlh->nlmsg_type)
            {
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
 * @brief 构造并发送一条默认路由请求（新增/设为默认/删除共用）
 * @details RTA_PRIORITY 决定路由形态：
 *          withMetric=1 → 带 metric 的备用默认路由（可与其它链路共存）；
 *          withMetric=0 → 不带 metric 的系统默认网关（同一 key 下仅一条，替换语义）
 * @param ifname     出口接口名，不可为空
 * @param gate       网关地址，不可为空
 * @param metric     路由 metric（withMetric=0 时忽略）
 * @param withMetric 1=携带 RTA_PRIORITY，0=不携带
 * @param del        1=删除，0=新增或替换
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int route_apply(const char* ifname, const char* gate, int metric,
                       int withMetric, int del)
{
    NETMGR_RREQ req = {0};          /* 请求缓冲 */
    struct in_addr gw = {0};        /* 网关地址 */
    unsigned int ifindex = 0;       /* 出口接口索引 */
    int fd = -1;                    /* netlink socket */
    int ret = 0;                    /* 子步骤返回值 */

    if (NULL == ifname || NULL == gate || !netmgr_ip_valid(gate) || metric < 0)
    {
        return -NETMGRERR_PARAM;
    }

    ifindex = if_nametoindex(ifname);
    if (0 == ifindex)
    {
        return -NETMGRERR_NOTFOUND;
    }

    /* 构造默认路由：dst 长度为 0 即 default，metric 由 RTA_PRIORITY 承载 */
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nlh.nlmsg_type = (0 != del) ? RTM_DELROUTE : RTM_NEWROUTE;
    req.nlh.nlmsg_flags = (0 != del) ? (NLM_F_REQUEST | NLM_F_ACK) :
                                       (NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK);
    req.nlh.nlmsg_seq = 1;
    req.rtm.rtm_family = AF_INET;
    req.rtm.rtm_table = RT_TABLE_MAIN;
    /* 删除时不限定协议：DHCP 客户端装的路由 proto 为 boot/dhcp，
       若指定 RTPROT_STATIC 内核按协议不匹配返回 ESRCH，正常条目也删不掉 */
    req.rtm.rtm_protocol = (0 != del) ? 0 : RTPROT_STATIC;
    req.rtm.rtm_scope = RT_SCOPE_UNIVERSE;
    req.rtm.rtm_type = RTN_UNICAST;
    req.rtm.rtm_dst_len = 0;

    if (1 != inet_pton(AF_INET, gate, &gw))
    {
        return -NETMGRERR_PARAM;
    }

    ret = route_attr(&req.nlh, RTA_GATEWAY, &gw, (int)sizeof(gw), (int)sizeof(req));
    if (ret < 0)
        return ret;
    ret = route_attr(&req.nlh, RTA_OIF, &ifindex, (int)sizeof(ifindex), (int)sizeof(req));
    if (ret < 0)
        return ret;
    if (0 != withMetric)
    {
        ret = route_attr(&req.nlh, RTA_PRIORITY, &metric, (int)sizeof(metric), (int)sizeof(req));
        if (ret < 0)
            return ret;
    }

    fd = route_sock_open();
    if (fd < 0)
        return fd;

    ret = route_send(fd, &req.nlh, (0 != del) ? 1 : 0);
    close(fd);
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
    return route_apply(ifname, gate, metric, 1, 0);
}

/**
 * @brief 设为系统默认网关（不带 metric，替换现有默认网关）
 * @details 与 netmgr_route_add 的区别：不带 RTA_PRIORITY，主路由表中同一默认目的
 *          只有一条这样的路由，故对已存在的默认网关是替换语义
 * @param ifname 出口接口名，不可为空
 * @param gate   网关地址，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_set_default(const char* ifname, const char* gate)
{
    return route_apply(ifname, gate, 0, 0, 0);
}

/**
 * @brief 删除默认路由（精确匹配接口、网关与 metric）
 * @details 目标路由本就消失时视为成功（幂等），供链路切换时清理残留使用
 * @param ifname 出口接口名，不可为空
 * @param gate   网关地址，不可为空
 * @param metric 路由 metric
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_del(const char* ifname, const char* gate, int metric)
{
    return route_apply(ifname, gate, metric, 1, 1);
}

/**
 * @brief 删除系统默认网关（不带 metric 的那条默认路由）
 * @details 用于主链路降级：撤掉"替换默认网关"形成的无 metric 路由
 * @param ifname 出口接口名，不可为空
 * @param gate   网关地址，不可为空
 * @return 0 成功（路由已不存在亦为成功）；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_del_default(const char* ifname, const char* gate)
{
    return route_apply(ifname, gate, 0, 0, 1);
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
        return -NETMGRERR_PARAM;
    }
    return route_dump(ifname, out);
}

/**
 * @brief 清空主路由表中所有 IPv4 默认路由
 * @details 反复查询当前最优默认路由并逐条删除，直到再无默认路由或达到防御性上限；
 *          供上层成为默认路由的唯一所有者：启动前清掉外部脚本/手工配置的条目，
 *          避免它们与按 metric 下发的条目抢占或阻塞新条目安装
 * @note 无网关的默认路由（如 `default dev ppp0`）不在本函数处理范围内
 * @return 0 成功（本就没有默认路由同样是成功）；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_clear_defaults(void)
{
    NETMGR_ROUTE act = {0}; /* 当前最优默认路由 */
    int ret = 0;            /* 子步骤返回值 */
    int i = 0;              /* 已删除条数，从 0 开始 */

    for (i = 0; i < ROUTE_DEF_CLEAR_MAX; ++i)
    {
        memset(&act, 0, sizeof(act));
        ret = netmgr_route_act(&act);
        if (-NETMGRERR_NOTFOUND == ret)
        {
            return 0; /* 已无默认路由，目标达成 */
        }
        if (ret < 0)
        {
            return ret;
        }
        ret = netmgr_route_del(act.ifname, act.gate, act.metric);
        if (ret < 0)
        {
            return ret;
        }
    }
    return 0;
}
