/**
 * 获取以太网ip，网关，掩码的操作
*/
#ifndef _NET_OPT_H_
#define _NET_OPT_H_
#ifdef __cplusplus
extern "C" {
#endif
#include "net_def.h"
// 获取指定ifname的ip地址
int get_ip(const char* ifname, char *ip, int len);
// 设置指定ifname的ip地址
int set_ip(const char* ifname, const char* ip);
// 获取指定ifname的网关（使用netlink方式获取，这里不提供这个接口）
//int get_gateway(const char* ifname, char *ip, const int len);
// 设置指定ifname网关
int set_gateway(const char* ifname, const char* ip, const char* mask, const char* dist);
// 设置指定ifname网关，如果存在就删除
int replace_gateway(const char* ifname, const char* ip, const char* mask, const char* dist);
// 指定ifname删除网关
int del_gateway(const char* ifname, const char* dist, const char* mask);
// 获取网关，见 net_netlink.h nl_getGateways
// 获取指定ifname的掩码
int get_mask(const char* ifname, char *ip, int len);
// 设置指定ifname的掩码
int set_mask(const char* ifname, const char *ip);
// 获取指定ifname的mac地址
int get_mac(const char* ifname, char* mac, int maclen);
// 设置指定ifname的mac地址
int set_mac(const char* ifname, const char* mac);
// 获取指定ifname的link状态
int get_link(const char* ifname);
// 设置指定ifname的状态
int set_enable(const char* ifname, int opt);

// 批量设置
// 检查ip合法性
int isIpValid(const char* ip);
#ifdef __cplusplus
}
#endif
#endif
