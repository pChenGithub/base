#include "net_opt.h"
#include "net_errno.h"
#include "net_netlink.h"
#include <stdio.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/route.h>
#include <errno.h>
#include <stdlib.h>

int get_ip(const char *ifname, char* ip, const int len)
{
    int ret = 0;
    int socketfd = 0;
    // struct ifreq，保存了网络接口的信息，包括名字、ip、掩码、广播。链路状态、mac地址
    // struct ifconf 理解为ifreq的集合，但是ifreq的内存需要用户指定
    struct ifreq ifcu_req;
    // 检查参数
    if (NULL==ifname || NULL==ip || len<=0)
        return -NETERR_CHECK_PARAM;
    
    // 获取socket，
    socketfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketfd<0)
        return -NETERR_SOCKET_FAIL;

    memcpy(ifcu_req.ifr_ifrn.ifrn_name, ifname, strlen(ifname)+1);
    if (ioctl(socketfd, SIOCGIFADDR, &ifcu_req)<0) {
        ret = -NETERR_SOCKET_GIFADDR_FAIL;
        goto socket_close_exit;
    }

    // 返回ip地址
    struct sockaddr* ip_addr = &ifcu_req.ifr_ifru.ifru_addr;
    snprintf(ip, len, "%u.%u.%u.%u",
            (unsigned char)ip_addr->sa_data[2],
            (unsigned char)ip_addr->sa_data[3],
            (unsigned char)ip_addr->sa_data[4],
            (unsigned char)ip_addr->sa_data[5]);
socket_close_exit:
    close(socketfd);
    return ret;
}

int set_ip(const char* ifname, const char* ip) {
    int ret = 0;
    if (NULL==ifname || NULL==ip)
        return -NETERR_CHECK_PARAM;

    int socketfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketfd<0)
        return -NETERR_SOCKET_FAIL;

    struct ifreq ifcu_req;
    // 添加清0
    memset(&ifcu_req, 0, sizeof(ifcu_req));
    memcpy(ifcu_req.ifr_ifrn.ifrn_name, ifname, strlen(ifname)+1);
#if 0
    // 这部分读的时候，有时候会报错，取消获取，上面添加清0
    if (ioctl(socketfd, SIOCGIFADDR, &ifcu_req)<0) {
        perror("设置ip。。。");
        ret = -NETERR_SOCKET_GIFADDR_FAIL;
        goto socket_close_exit;
    }
#endif

    // 修改ip地址
    struct sockaddr_in* addr = (struct sockaddr_in*)&ifcu_req.ifr_ifru.ifru_addr;
    inet_aton(ip, &addr->sin_addr);
    addr->sin_family = AF_INET;
    addr->sin_port = 0;
#if 0
    // 这里测试过了，这样修改mask函数结束后不能修改调mask
    addr = (struct sockaddr_in*)&ifcu_req.ifr_ifru.ifru_netmask;
    inet_aton(ip, &addr->sin_addr);
#endif
    
    if (ioctl(socketfd, SIOCSIFADDR, &ifcu_req)<0)
        ret = -NETERR_SOCKET_SIFADDR_FAIL;

//socket_close_exit:
    close(socketfd);
    return ret;
}

int get_gateway(const char *ifname, char *ip, const int len)
{
    (void)ifname;
    (void)ip;
    (void)len;
    int ret = 0;
    struct rtentry rt;
    struct sockaddr_in* sockaddr = NULL;
    memset(&rt, 0, sizeof(rt));
    int socketfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketfd<0)
        return -NETERR_SOCKET_FAIL;
    
    if (ioctl(socketfd, SIOCRTMSG, &rt)<0) {
        ret = -NETERR_SOCKET_RTMSG;
        printf("xxxxxxx4 %d, %s\n", errno, strerror(errno));
        goto socket_close_exit;
    }

    char* devname = rt.rt_dev;
    sockaddr = (struct sockaddr_in*)(&rt.rt_gateway);
    printf("名称 %s，ip %s\n", devname, inet_ntoa(sockaddr->sin_addr));

socket_close_exit:
    close(socketfd);
    return ret;
}

int set_gateway(const char* ifname, const char* ip, const char* mask, const char* dist) {
    if (NULL==ifname || NULL==ip)
        return -NETERR_CHECK_PARAM;
    int ret = 0;
    struct rtentry rt;
    struct sockaddr_in* sockaddr = NULL;
    memset(&rt, 0, sizeof(rt));

    int socketfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketfd<0)
        return -NETERR_SOCKET_FAIL;
        
    sockaddr = (struct sockaddr_in*)(&rt.rt_gateway);
    //memset(sockaddr, 0, sizeof(*sockaddr));
    sockaddr->sin_family = AF_INET;
    sockaddr->sin_port = 0;
    if (inet_aton(ip, &sockaddr->sin_addr)<0) {
        ret = -NETERR_INVALID_GATEWAY;
        goto socket_close_exit;
    }
        
    sockaddr = (struct sockaddr_in *)&rt.rt_dst;
    sockaddr->sin_family = AF_INET;
    if (NULL==dist)
        sockaddr->sin_addr.s_addr = INADDR_ANY;
    else
        ret = inet_aton(dist, &sockaddr->sin_addr);
        //sockaddr->sin_addr.s_addr = inet_addr(dist);

    if (ret<0) {
        ret = -NETERR_INVALID_GATEDIST;
        goto socket_close_exit;
    }

    sockaddr = (struct sockaddr_in *)&rt.rt_genmask;
    sockaddr->sin_family = AF_INET;
    
    if (NULL==mask)
        sockaddr->sin_addr.s_addr = INADDR_ANY;
    else 
        ret = inet_aton(mask, &sockaddr->sin_addr);
        //sockaddr->sin_addr.s_addr = inet_addr(mask);
    
    if (ret<0) {
        ret = -NETERR_INVALID_MASK;
        goto socket_close_exit;
    }

    rt.rt_flags = RTF_UP |RTF_GATEWAY;
    rt.rt_dev = (char*)ifname;

    if ((ret = ioctl(socketfd, SIOCADDRT, &rt))==0) 
        goto socket_close_exit;

    printf("设置网关失败 %d, %s\n", errno, strerror(errno));
    ret = -NETERR_SOCKET_ADDRT;
    if (17==errno)
        ret = -NETERR_SOCKET_ADDRT_EXSIT;

socket_close_exit:
    close(socketfd);
    return ret;    
}

int replace_gateway(const char* ifname, const char* ip, const char* mask, const char* dist) {
    int ret = 0;
    if (NULL==ifname || NULL==ip)
        return -NETERR_CHECK_PARAM;

    if ((ret = set_gateway(ifname, ip, mask, dist))==0)
        goto exit;
    if (-NETERR_SOCKET_ADDRT_EXSIT!=ret)
        goto exit;
    
    const char* tmpdist = dist;
    if (NULL==dist)
        tmpdist = "default";
    // 清空dist相同的网关
    ROUTE_LIST list[10];
    int routenum = 10;
    memset(&list, 0, sizeof(list));
    ret = nl_getGateways((const char*)tmpdist, ifname, NULL, list, &routenum);
    if (ret<0 && -NETERR_BUFFER_NOTENOUGH!=ret) {
        printf("ret xxx111 %d\n", ret);
        goto exit;
    }
    printf("获取相同dist网关个数 %d\n", routenum);

    // 清空gate相同的网关
    int count = 10-routenum;
    ret = nl_getGateways(NULL, ifname, ip, list+routenum, &count);
    if (ret<0 && -NETERR_BUFFER_NOTENOUGH!=ret) {
        printf("ret xxx111 %d\n", ret);
        goto exit;
    }

    routenum += count;

    printf("获取相同gate网关个数 %d\n", routenum);
    // 删除网关
    if (0==routenum) {
        ret = -NETERR_SOCKET_ADDRT;
        goto exit;
    }

    int i = 0;
    for (i=0;i<routenum;i++) {
        printf("s 获取到网关 dist %s, gate %s, mask %s， devname %s\n",
            list[i].dist, list[i].gate, list[i].mask, list[i].devname);
        if ((ret=del_gateway(ifname, list[i].dist, "0.0.0.0"))==0)continue;
        if ((ret=del_gateway(ifname, list[i].dist, "255.0.0.0"))==0)continue;
        if ((ret=del_gateway(ifname, list[i].dist, "255.255.0.0"))==0)continue;
        if ((ret=del_gateway(ifname, list[i].dist, "255.255.255.0"))==0)continue;
        if ((ret=del_gateway(ifname, list[i].dist, "255.255.255.255"))==0)continue;
        printf("e 获取到网关 dist %s, gate %s, mask %s， devname %s\n",
               list[i].dist, list[i].gate, list[i].mask, list[i].devname);
    }

//set_new_gate:
    // 重新设置网关
    ret = set_gateway(ifname, ip, mask, dist);
exit:
    return ret;
}

int del_gateway(const char* ifname, const char* dist, const char* mask) {

    int ret = 0;
    struct rtentry rt;
    struct sockaddr_in* sockaddr = NULL;
    memset(&rt, 0, sizeof(rt));

    int socketfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketfd<0)
        return -NETERR_SOCKET_FAIL;
        
    sockaddr = (struct sockaddr_in*)(&rt.rt_dst);
    sockaddr->sin_family = AF_INET;

    if (NULL==dist)
        sockaddr->sin_addr.s_addr = INADDR_ANY;
    else
        ret = inet_aton(dist, &sockaddr->sin_addr);

    if (ret<0) {
        ret = -NETERR_INVALID_GATEDIST;
        goto socket_close_exit;
    }

    sockaddr = (struct sockaddr_in *)&rt.rt_genmask;
    sockaddr->sin_family = AF_INET;
    
    if (NULL==mask)
        sockaddr->sin_addr.s_addr = INADDR_ANY;
    else 
        ret = inet_aton(mask, &sockaddr->sin_addr);
    
    if (ret<0) {
        ret = -NETERR_INVALID_MASK;
        goto socket_close_exit;
    }

    if (NULL!=ifname)
        rt.rt_dev = (char*)ifname;
    if ((ret = ioctl(socketfd, SIOCDELRT, &rt))<0) {
        printf("删除网关失败 %d, %s\n", errno, strerror(errno));
        printf("mask %s\n", mask);
        ret = -NETERR_SOCKET_ADDRT;
        goto socket_close_exit;
    }

socket_close_exit:  
    close(socketfd);
    return 0;
}

int get_mask(const char *ifname, char *ip, const int len)
{
    int ret = 0;
    int socketfd = 0;
    // struct ifreq，保存了网络接口的信息，包括名字、ip、掩码、广播。链路状态、mac地址
    // struct ifconf 理解为ifreq的集合，但是ifreq的内存需要用户指定
    struct ifreq ifcu_req;
    // 检查参数
    if (NULL==ifname || ip==NULL || len<16)
        return -NETERR_CHECK_PARAM;
    // 获取socket，
    socketfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketfd<0)
        return -NETERR_SOCKET_FAIL;

    memcpy(ifcu_req.ifr_ifrn.ifrn_name, ifname, strlen(ifname)+1);
    if (ioctl(socketfd, SIOCGIFNETMASK, &ifcu_req)<0) {
        ret = -NETERR_SOCKET_GIFNETMASK_FAIL;
        goto socket_close_exit;
    }

    // 返回ip地址
    struct sockaddr* ip_addr = &ifcu_req.ifr_ifru.ifru_netmask;
    snprintf(ip, len, "%u.%u.%u.%u",
            (unsigned char)ip_addr->sa_data[2],
            (unsigned char)ip_addr->sa_data[3],
            (unsigned char)ip_addr->sa_data[4],
            (unsigned char)ip_addr->sa_data[5]);
socket_close_exit:
    close(socketfd);
    return ret;
}

int set_mask(const char* ifname, const char *ip) {
    int ret = 0;
    if (NULL==ifname || NULL==ip)
        return -NETERR_CHECK_PARAM;

    int socketfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketfd<0)
        return -NETERR_SOCKET_FAIL;

    struct ifreq ifcu_req;
    memset(&ifcu_req, 0, sizeof(ifcu_req));
    memcpy(ifcu_req.ifr_ifrn.ifrn_name, ifname, strlen(ifname)+1);
#if 0
    if (ioctl(socketfd, SIOCGIFNETMASK, &ifcu_req)<0) {
        ret = -NETERR_SOCKET_GIFNETMASK_FAIL;
        goto socket_close_exit;
    }
#endif

    // 修改ip地址
    struct sockaddr_in* addr = (struct sockaddr_in*)&ifcu_req.ifr_ifru.ifru_netmask;
    inet_aton(ip, &addr->sin_addr);
    addr->sin_family = AF_INET;
    addr->sin_port = 0;
 
    if (ioctl(socketfd, SIOCSIFNETMASK, &ifcu_req)<0)
        ret = -NETERR_SOCKET_SIFNETMASK_FAIL;

//socket_close_exit:
    close(socketfd);
    return ret;
}

int get_link(const char* ifname) {
    int ret = 0;
    int socketfd = 0;
    // struct ifreq，保存了网络接口的信息，包括名字、ip、掩码、广播。链路状态、mac地址
    // struct ifconf 理解为ifreq的集合，但是ifreq的内存需要用户指定
    struct ifreq ifcu_req;
    // 检查参数
    if (NULL==ifname)
        return -NETERR_CHECK_PARAM;
    // 获取socket，
    socketfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketfd<0)
        return -NETERR_SOCKET_FAIL;

    memcpy(ifcu_req.ifr_ifrn.ifrn_name, ifname, strlen(ifname)+1);
    if (ioctl(socketfd, SIOCGIFFLAGS, &ifcu_req)<0) {
        ret = -NETERR_SOCKET_GIFFLAGS_FAIL;
        goto socket_close_exit;
    }

    // 返回flag
    if (ifcu_req.ifr_ifru.ifru_flags&IFF_RUNNING) {
        ret = NET_LINK_UP; // up
    } else {
        ret = NET_LINK_DOWN; // down
    }

socket_close_exit:
    close(socketfd);
    return ret;
}

int set_enable(const char* ifname, int opt) {
    int ret = 0;
    int socketfd = 0;
    // struct ifreq，保存了网络接口的信息，包括名字、ip、掩码、广播。链路状态、mac地址
    // struct ifconf 理解为ifreq的集合，但是ifreq的内存需要用户指定
    struct ifreq ifcu_req;
    // 检查参数
    if (NULL==ifname)
        return -NETERR_CHECK_PARAM;
    // 获取socket，
    socketfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketfd<0)
        return -NETERR_SOCKET_FAIL;

    switch (opt)
    {
    case NET_LINK_UP:
        ifcu_req.ifr_ifru.ifru_flags |= (IFF_RUNNING|IFF_UP);
        break;
    case NET_LINK_DOWN:
        ifcu_req.ifr_ifru.ifru_flags &= (~IFF_RUNNING);
        break;
    default:
        break;
    }
    
    memcpy(ifcu_req.ifr_ifrn.ifrn_name, ifname, strlen(ifname)+1);
    if (ioctl(socketfd, SIOCSIFFLAGS, &ifcu_req)<0) {
        ret = -NETERR_SOCKET_SIFFLAGS_FAIL;
        //goto socket_close_exit;
    }

//socket_close_exit:
    close(socketfd);
    return ret;
}

#include <regex.h>
static const char* pattern = "^((2[0-4]\\d|25[0-5]|[01]?\\d\\d?)\\.){3}(2[0-4]\\d|25[0-5]|[01]?\\d\\d?)$";
int isIpValid(const char* ip) {
    if (NULL==ip)
        return -NETERR_CHECK_PARAM;
    int ret = 0;
    regex_t reg;
    regmatch_t match[1] = {0};

    memset(&reg, 0, sizeof(reg));
    if (0!=regcomp(&reg, pattern, REG_EXTENDED))
        return -NETERR_REGCOMP_FAIL;

    if (REG_NOMATCH==regexec(&reg, ip, 1, match, 0))
        ret = -NETERR_REGEXEC_FAIL;

    regfree(&reg);
    return ret;
}

int set_gateway_shell(const char *ifname, const char *ip, const char *mask, const char *dist) {
    (void)mask;
    char cmd[64] = {0};
    const char* pdist = dist;
    if (NULL==pdist)
        pdist = "default";
    snprintf(cmd, sizeof(cmd), "route add %s gw %s dev %s", pdist, ip, ifname);
    printf("cmd %s\n", cmd);
    system(cmd);
    return 0;
}
