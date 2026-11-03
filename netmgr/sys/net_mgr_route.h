/**
 * @file net_mgr_route.h
 * @brief netmgr 系统适配层：默认路由管理（sys 层，可独立使用）
 * @details 基于 rtnetlink 下发、删除与查询带 metric 的 IPv4 默认路由；
 *          metric 由 RTA_PRIORITY 承载，ioctl(SIOCADDRT) 无法可靠表达该能力。
 *
 * 使用约束（独立调用方须知）：
 * 1) 单实例：查询共用一份接收缓冲，多线程并发调用需由调用方串行化；
 * 2) 线程：本层不起线程，add/del 为同步 netlink 请求；
 * 3) 权限：add/del 需 CAP_NET_ADMIN，act/dev 查询无需特权；
 * 4) 外部依赖：内核 NETLINK_ROUTE。
 */
#ifndef __NET_MGR_ROUTE_H__
#define __NET_MGR_ROUTE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_types.h"

/** 默认路由条目 */
typedef struct {
    char ifname[NETMGR_IF_LEN]; /**< 出口接口名 */
    char gate[NETMGR_IP_LEN];   /**< 网关地址 */
    int metric;                 /**< 路由 metric，越小越优先 */
} NETMGR_ROUTE;

/* ===== 查询 ===== */

/**
 * @brief 查询当前生效（metric 最小）的默认路由
 * @param out 输出路由信息，不可为空
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_act(NETMGR_ROUTE* out);

/**
 * @brief 查询指定接口上的默认路由（取 metric 最小的一条）
 * @param ifname 出口接口名，不可为空
 * @param out    输出路由信息，不可为空
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_dev(const char* ifname, NETMGR_ROUTE* out);

/* ===== 操作 ===== */

/**
 * @brief 下发默认路由（带 metric）
 * @param ifname 出口接口名，不可为空
 * @param gate   网关地址，不可为空
 * @param metric 路由 metric，越小越优先
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_add(const char* ifname, const char* gate, int metric);

/**
 * @brief 删除默认路由（精确匹配接口、网关与 metric）
 * @param ifname 出口接口名，不可为空
 * @param gate   网关地址，不可为空
 * @param metric 路由 metric
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_del(const char* ifname, const char* gate, int metric);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_ROUTE_H__ */
