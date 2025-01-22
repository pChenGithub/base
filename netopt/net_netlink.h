/** 
 * 实现监听内核事件的模块
 * 监听开启一个独立线程，并提供回调函数返回结果
 * 并且提供跟内核通信的接口
 * 
*/
#ifndef __NET_NETLINK_H__
#define __NET_NETLINK_H__
#ifdef __cplusplus
extern "C" {
#endif
#include "net_def.h"
// link事件的回调参数
typedef struct {
    const char *ifname;
    int link;
} NETLINK_CB_PARAM;

// netlink的监听事件类型
typedef enum {
    NLEV_LINK = 0,
    NLEV_MAX,
} NLEV_TYPE;
typedef int (*hand_netlink_ev)(void* arg);

int start_netlink_listen();
int stop_netlink_listen();
int set_netlink_listen_ev(NLEV_TYPE ev, hand_netlink_ev cb);
int rm_netlink_listen_ev(NLEV_TYPE ev);

// 基于netlink获取网络信息
typedef struct {
    char dist[IP_STR_MAXLEN];
    char gate[IP_STR_MAXLEN];
    char mask[IP_STR_MAXLEN];
    char devname[IP_STR_MAXLEN];
} ROUTE_LIST;

// routeslen 带入ROUTE_LIST的长度，并返回最终的实际数量
// dist，devname，gate都是过滤条件，可以为空
int nl_getGateways(const char* dist, const char* devname, const char* gate, ROUTE_LIST* routes, int* routeslen);
#ifdef __cplusplus
}
#endif
#endif



