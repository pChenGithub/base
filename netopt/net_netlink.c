#include "net_netlink.h"
#include "net_errno.h"
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <net/if.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#define NL_THREAD_RUNNING   0x01
typedef struct netlink_listen {
    int flag;
    int socketfd;
    hand_netlink_ev cb[NLEV_MAX];
} NETLINK_LISTEN;

static NETLINK_LISTEN server = {
    // lock
    0,
    0,
    // 回调
    {0},
};

static int parse_linkevent(hand_netlink_ev cb, struct nlmsghdr* msg) {
    if (NULL==cb || NULL==msg)
        return -NETERR_CHECK_PARAM;

    struct ifinfomsg *ifi = NULL;
    struct rtattr* rta = NULL;
    NETLINK_CB_PARAM cbarg;
    memset(&cbarg, 0, sizeof(NETLINK_CB_PARAM));

    // 剥离消息
    if (sizeof(*ifi)>(msg->nlmsg_len-sizeof(*msg)))
    {
        printf("消息不完整\n");
        return -1;
    }

    // 获取nlmsghdr后的地址
    ifi = NLMSG_DATA(msg);
    #if 0
    if ((ifi->ifi_flags & IFF_LOOPBACK) != 0) {
        return -1;
    }
    #endif

    // 获取rtattr
    rta = (struct rtattr *)
        ((char *) ifi + NLMSG_ALIGN(sizeof(*ifi)));
    int len = NLMSG_PAYLOAD(msg, sizeof(*ifi));

    if (!RTA_OK(rta, len))
        return -1;

    cbarg.ifname = RTA_DATA(rta);
    if (ifi->ifi_flags&IFF_RUNNING) {
        cbarg.link = NET_LINK_UP; // down
        #if 0
    } else if (!(msg->nlmsg_flags&IFF_RUNNING)) {
        cbarg.link = NET_LINK_UP; // up
        #endif
    } else {
        cbarg.link = NET_LINK_DOWN; // unkown
    }

    cb(&cbarg);

    return 0;
}

// 监听线程
#define NL_RECVBUFF_LEN 2048
static char recvbuff[NL_RECVBUFF_LEN] = {0};
static void* recv_from_netlink(void* arg) {
    int ret = 0;
    NETLINK_LISTEN* sv = (NETLINK_LISTEN*)arg;
/** 
 * struct nlmsghdr
 * struct ifinfomsg
 * 这里内存4字节对齐
 * struct rtattr
 * 这里放rtattr的内容
*/
    struct nlmsghdr* mlmsg = (struct nlmsghdr*)recvbuff;
    sv->flag |= NL_THREAD_RUNNING;
    while (NL_THREAD_RUNNING&(sv->flag)) {
        //printf("开始等待事件\n");
        memset(recvbuff, 0, NL_RECVBUFF_LEN);
        // flag MSG_WAITALL
        if ((ret=recv(sv->socketfd, recvbuff, NL_RECVBUFF_LEN, 0))<=0)
        {
            printf("接收失败，返回 %d\n", ret);
            continue;
        }
        //printf("接收数据长度 %d\n", ret);
        if (!NLMSG_OK(mlmsg, ret))
        {
            // 不是合法的netlink消息
            printf("不是合法的netlink消息，返回\n");
            continue;
        }

        #if 0
        struct rtattr *rta
        RTA_OK(rta, len)
        RTA_DATA(rta)
        // 去除nlmsghdr后的长度
        NLMSG_PAYLOAD(nh, sizeof(*ifi))
        // 返回输入长度的字节对齐长度（这里是4字节对齐）
        NLMSG_ALIGN(sizeof(*ifi)))
        #endif
        
        //printf("recv %d 长度 %d\n", mlmsg->nlmsg_type, mlmsg->nlmsg_len);
        // 根据不同的事件类型，使用不同的回调
        switch (mlmsg->nlmsg_type)
        {
        case RTM_NEWLINK:
            parse_linkevent(sv->cb[NLEV_LINK], mlmsg);
            break;
        default:
            break;
        }
    }
    return NULL;
}

/**
 * @brief start_netlink_listen
 *  开始netlink事件监听
 * @return
 */
int start_netlink_listen() {
    int ret = 0;
    if (NL_THREAD_RUNNING&server.flag)
        return -NETERR_NETLINK_ISRUNNING;

    int socketfd = socket(AF_NETLINK, SOCK_RAW, 0);
    if (socketfd<0)
        return -NETERR_SOCKET_FAIL;

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    // 表示网络link状态改变，ipv4地址修改监听
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;

    if (bind(socketfd, (struct sockaddr*)&addr, sizeof(addr))<0)
        return -NETERR_BIND_FAIL;

    server.socketfd = socketfd;
    pthread_t pid = 0;
    ret = pthread_create(&pid, NULL, recv_from_netlink, &server);
    if (ret<0) {
        ret = -NETERR_PTHREADCREATE_FAIL;
        goto closefd_exit;
    }

    return 0;

closefd_exit:
    close(socketfd);
    return ret;        
}

/**
 * @brief stop_netlink_listen
 *  停止netlink事件监听
 * @return
 */
int stop_netlink_listen() {
    NETLINK_LISTEN* sv = &server;
    // 停止线程
    sv->flag &= (~NL_THREAD_RUNNING);
    pthread_cancel(sv->socketfd);
    // 等待线程退出（加超时）
    pthread_join(sv->socketfd, NULL);
    // 关闭socket
    close(sv->socketfd);
    // 复位全局变量
    memset(sv, 0, sizeof(*sv));
    return 0;
}

int set_netlink_listen_ev(NLEV_TYPE ev, hand_netlink_ev cb) {
    if (ev>=NLEV_MAX || NULL==cb)
        return -NETERR_CHECK_PARAM;

    NETLINK_LISTEN* sv = &server;
    sv->cb[ev] = cb;
    return 0;
}

int rm_netlink_listen_ev(NLEV_TYPE ev) {
    if (ev>=NLEV_MAX)
        return -NETERR_CHECK_PARAM;

    NETLINK_LISTEN* sv = &server;
    sv->cb[ev] = 0;
    return 0;
}

// struct nlmsghdr
// struct rtmsg
#define GETROUTE_BUFFSIZE   1024
int nl_getGateways(const char* dist, const char* devname, const char* gate, ROUTE_LIST* routes, int* routeslen) {
    if (NULL==routes || NULL==routeslen || *routeslen<=0)
        return -NETERR_CHECK_PARAM;
    int ret = 0;
    struct rtattr *rtAttr = NULL;
    struct nlmsghdr* nlmsg = (struct nlmsghdr*)calloc(GETROUTE_BUFFSIZE, 1);
    if (NULL==nlmsg)
        return -NETERR_REQMEM;
    struct rtmsg* rt = NLMSG_DATA(nlmsg);

    int socketfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (socketfd<0) {
        ret = -NETERR_SOCKET_FAIL;
        goto free_exit;
    }

    nlmsg->nlmsg_flags = NLM_F_DUMP|NLM_F_REQUEST;
    nlmsg->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nlmsg->nlmsg_pid = getpid();
    nlmsg->nlmsg_seq = 0;
    nlmsg->nlmsg_type = RTM_GETROUTE;

    if (send(socketfd, nlmsg, nlmsg->nlmsg_len, 0)<0) {
        ret = -NETERR_SOCKET_SENDFAIL;
        goto closefd_exit;
    }

    int recvlen = 0;
    char* p = (char*)nlmsg;

    // 循环读取返回结果
#if 0
    while (ret=recv(socketfd, p+recvlen, GETROUTE_BUFFSIZE-recvlen, 0)) {
        if (0==ret)
            break;
        else if (ret<0) {
            ret = -NETERR_SOCKET_RECVFAIL;
            goto closefd_exit;
        }
        recvlen += ret;
    }
#else
    fd_set rfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_SET(socketfd, &rfds);
    // 设置超时时间
    tv.tv_sec = 0;
    tv.tv_usec = 600000;

wait_route_data:
    ret = select(socketfd+1, &rfds, NULL, NULL, &tv);
    if (ret<0) {
        ret = -NETERR_SELECT;
        goto closefd_exit;
    } else if (ret>0){
        // 可读
        ret = recv(socketfd, p+recvlen, GETROUTE_BUFFSIZE-recvlen, 0);
        if (ret<0) {
            ret = -NETERR_SOCKET_RECVFAIL;
            goto closefd_exit;
        } else if (ret>0) {
            recvlen += ret;
            goto wait_route_data;
        }
    } else {
        ret = -NETERR_SELECT_TIMEOUT;
        goto closefd_exit;
    }
#endif

    int i = 0;
    struct nlmsghdr* pnl = nlmsg;
    // 用RT_TABLE_MAIN过滤，第一个循环，有几个网关就会循环几次
    for (;NLMSG_OK(pnl, recvlen);pnl=NLMSG_NEXT(pnl, recvlen)) {
        // 获取rt内容长度
        int rtlen = RTM_PAYLOAD(pnl);
        rt = NLMSG_DATA(pnl);
#if 1
        if((rt->rtm_family!=AF_INET)||(rt->rtm_table!=RT_TABLE_MAIN))
            continue;
#endif
        if (i>=(*routeslen)) {
            ret = -NETERR_BUFFER_NOTENOUGH;
            *routeslen = i;
            printf("ret xxx %d\n", ret);
            goto closefd_exit;
        }
        printf("处理一条数据\n");

        rtAttr = RTM_RTA(rt);
        //printf("\n\n\n");
        memcpy(routes[i].dist, "default", 8);
        memcpy(routes[i].gate, "*", 2);
        for (;RTA_OK(rtAttr, rtlen);rtAttr=RTA_NEXT(rtAttr, rtlen)) {
            //struct in_addr* in1 = (struct in_addr*)RTA_DATA(rtAttr);
            //printf("rta类型 %d, 长度 %d, rt内容长度 %d, 内容 %s\n", rtAttr->rta_type, rtAttr->rta_len, rtlen, inet_ntoa(*in1));
            
            if (RTA_GATEWAY==rtAttr->rta_type) {
                // 类型是网关，保存网关
                struct in_addr* in = (struct in_addr*)RTA_DATA(rtAttr);
                memcpy(routes[i].gate, inet_ntoa(*in), IP_STR_MAXLEN);
            } else if (RTA_OIF==rtAttr->rta_type) {
                // 类型是接口名
                if_indextoname(*(int*)RTA_DATA(rtAttr), routes[i].devname);
            } else if (RTA_DST==rtAttr->rta_type) {
                // 类型是目标
                struct in_addr* in = (struct in_addr*)RTA_DATA(rtAttr);
                memcpy(routes[i].dist, inet_ntoa(*in), IP_STR_MAXLEN);
            }
        }

        if (NULL!=dist && strncmp(dist, routes[i].dist, strlen(dist)+1))
            continue;
        if (NULL!=devname && strncmp(devname, routes[i].devname, strlen(devname)+1))
            continue;
        if (NULL!=gate && strncmp(gate, routes[i].gate, strlen(gate)+1))
            continue;
        i++;
    }
    *routeslen = i;

closefd_exit:
    close(socketfd);
    socketfd = 0;
free_exit:
    free(nlmsg);
    nlmsg = NULL;    
    return ret;
}


