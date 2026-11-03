/**
 * @file net_mgr_event.h
 * @brief netmgr 系统适配层：netlink 事件监听（sys 层，可独立使用）
 * @details 采用「接收线程 + 事件回调」模型：本层负责解析 netlink 报文并给出
 *          结构化事件（类别、地址族、标志位、接口名、地址/掩码、网关、metric），
 *          具体状态判定由调用方完成，本层不含任何链路状态机。
 *          事件类别与内核消息类型一一对应，新增事件只需增加枚举值与解析分支。
 *
 * 使用约束（独立调用方须知）：
 * 1) 单实例：进程内只维护一份监听上下文，重复 open 直接返回成功；
 * 2) 线程：起 1 个 netlink 接收线程，事件回调在该线程上下文执行，回调内不可长时间阻塞；
 * 3) 权限：订阅路由多播组无需特权；
 * 4) 外部依赖：内核 NETLINK_ROUTE。
 */
#ifndef __NET_MGR_EVENT_H__
#define __NET_MGR_EVENT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_limits.h"

/** 订阅组掩码：0 表示使用默认组（链路 + IPv4 地址 + IPv4 路由） */
#define NETMGR_EVENT_GROUPS_DEFAULT  0u

/** netlink 事件类别，与内核消息类型一一对应 */
typedef enum {
    NETMGR_NEV_LINK_ADD = 0, /**< 链路新增或状态刷新（RTM_NEWLINK） */
    NETMGR_NEV_LINK_DEL,     /**< 链路删除（RTM_DELLINK） */
    NETMGR_NEV_ADDR_ADD,     /**< IPv4 地址新增（RTM_NEWADDR） */
    NETMGR_NEV_ADDR_DEL,     /**< IPv4 地址删除（RTM_DELADDR） */
    NETMGR_NEV_ROUTE_ADD,    /**< 路由新增（RTM_NEWROUTE） */
    NETMGR_NEV_ROUTE_DEL,    /**< 路由删除（RTM_DELROUTE） */
    NETMGR_NEV_LOST          /**< 内核事件缓冲溢出，已有事件丢失，建议做一次全量刷新 */
} NETMGR_NEV_TYPE;

/**
 * @brief 判断事件方向是否为「新增/上线」
 * @param type 事件类别
 * @return 1 新增/上线；0 删除/下线或事件丢失
 */
#define NETMGR_NEV_IS_ADD(type) \
    (NETMGR_NEV_LINK_ADD == (type) || NETMGR_NEV_ADDR_ADD == (type) || \
     NETMGR_NEV_ROUTE_ADD == (type))

/**
 * 单条 netlink 事件内容
 * @note 字段按事件类别有效，未涉及的字段为 0 或空串：
 *       LINK_ADD/LINK_DEL  ：family、flags（接口标志 IFF_*）
 *       ADDR_ADD/ADDR_DEL  ：family、flags（地址标志 IFA_F_*）、ip、mask
 *       ROUTE_ADD/ROUTE_DEL：family、metric、isDefault、gate
 *       LOST               ：仅 type 有效
 */
typedef struct {
    NETMGR_NEV_TYPE type;        /**< 事件类别，方向由类别本身表达 */
    int family;                  /**< 地址族，取 AF_UNSPEC / AF_INET / AF_INET6 */
    int flags;                   /**< LINK：接口标志位（IFF_*）；ADDR：地址标志（IFA_F_*）；其余为 0 */
    int metric;                  /**< 路由 metric（RTA_PRIORITY）；非路由事件为 0 */
    int isDefault;               /**< 1=默认路由（仅路由事件有效） */
    char ifname[NETMGR_IF_LEN];  /**< 相关接口名，无法解析时为空串 */
    char ip[NETMGR_IP_LEN];      /**< IPv4 地址事件中的地址，其余事件为空串 */
    char mask[NETMGR_IP_LEN];    /**< IPv4 地址事件中的接口掩码（由前缀长度换算），其余事件为空串 */
    char gate[NETMGR_IP_LEN];    /**< 路由事件中的下一跳网关（RTA_GATEWAY），其余事件为空串 */
} NETMGR_NEV;

/**
 * @brief netlink 事件回调，在事件接收线程上下文执行
 * @param ev   事件内容，仅在回调期间有效，不可保存指针
 * @param user 注册回调时传入的用户指针，可为空
 * @return 0 继续监听；返回值当前被忽略，不影响接收线程
 */
typedef int (*netmgr_nev_cb)(const NETMGR_NEV* ev, void* user);

/* ===== 操作 ===== */

/**
 * @brief 启动 netlink 事件监听
 * @param cb     事件回调，可为空（只需要订阅不关心内容时）
 * @param user   回调透传的用户指针，可为空
 * @param groups 订阅组掩码（内核 RTMGRP_* 的组合），传 NETMGR_EVENT_GROUPS_DEFAULT
 *               表示使用默认组：链路 + IPv4 地址 + IPv4 路由
 * @return 0 成功（已在运行时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_event_open(netmgr_nev_cb cb, void* user, unsigned int groups);

/**
 * @brief 停止 netlink 事件监听并回收线程
 * @return 0 成功（未启动时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_event_close(void);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_EVENT_H__ */
