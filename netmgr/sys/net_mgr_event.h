/**
 * @file net_mgr_event.h
 * @brief netmgr 系统适配层：netlink 事件监听（sys 层，可独立使用）
 * @details 采用「接收线程 + 事件回调」模型：本层负责解析 netlink 报文并给出
 *          结构化事件（类别 + 接口名，按类别附带地址/掩码或路由信息），
 *          具体状态判定由调用方完成，本层不含任何链路状态机。
 *          链路方向由接口标志解析得出（LINK_UP / LINK_DOWN），失效删除为 LINK_DEL，
 *          不做“消息类型与事件类别一一对应”的机械映射。
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

/**
 * netlink 事件类别
 * @note 链路方向按**真实接口标志**解析，而非仅看内核消息类型：插线与拔线内核都发
 *       RTM_NEWLINK，解析时用 IFF_UP + IFF_RUNNING 判定为 LINK_UP / LINK_DOWN；
 *       只有接口被真正删除（RTM_DELLINK）才是 LINK_DEL
 */
typedef enum {
    NETMGR_NEV_LINK_UP = 0,  /**< 链路可用（管理态 UP 且 carrier 在位） */
    NETMGR_NEV_LINK_DOWN,    /**< 链路不可用（插线前/拔线后/管理态关闭） */
    NETMGR_NEV_LINK_DEL,     /**< 链路删除（RTM_DELLINK），接口已从系统移除 */
    NETMGR_NEV_ADDR_ADD,     /**< IPv4 地址新增或变更（RTM_NEWADDR） */
    NETMGR_NEV_ADDR_DEL,     /**< IPv4 地址删除（RTM_DELADDR） */
    NETMGR_NEV_ROUTE_ADD,    /**< 路由新增或变更（RTM_NEWROUTE） */
    NETMGR_NEV_ROUTE_DEL,    /**< 路由删除（RTM_DELROUTE） */
    NETMGR_NEV_LOST          /**< 内核事件缓冲溢出，已有事件丢失，建议做一次全量刷新 */
} NETMGR_NEV_TYPE;

/** IPv4 地址事件载荷：NETMGR_NEV_ADDR_ADD / ADDR_DEL */
typedef struct {
    char ip[NETMGR_IP_LEN];   /**< 地址（IFA_LOCAL 优先） */
    char mask[NETMGR_IP_LEN]; /**< 由前缀长度换算的掩码 */
} NETMGR_NEVP_ADDR;

/** 路由事件载荷：NETMGR_NEV_ROUTE_ADD / ROUTE_DEL */
typedef struct {
    int metric;               /**< 路由 metric（RTA_PRIORITY），无该属性为 0 */
    int isDefault;            /**< 1=主表默认路由（RT_TABLE_MAIN 且目的前缀长度为 0） */
    char gate[NETMGR_IP_LEN]; /**< 下一跳网关（RTA_GATEWAY），直连路由为空串 */
} NETMGR_NEVP_ROUTE;

/**
 * 单条 netlink 事件内容：type / ifname 为公共字段，其余按类型取 u
 * @note 载荷映射（禁止跨类型读取 u）：
 *       LINK_UP / LINK_DOWN / LINK_DEL → 无载荷（方向已由类别表达，接口名见 ifname）
 *       ADDR_ADD / ADDR_DEL            → u.addr（ip、mask）
 *       ROUTE_ADD / ROUTE_DEL          → u.route（metric、isDefault、gate）
 *       LOST                           → 仅 type 有效
 */
typedef struct {
    NETMGR_NEV_TYPE type;       /**< 事件类别（判别符） */
    char ifname[NETMGR_IF_LEN]; /**< 相关接口名，无法解析时为空串（LOST 与多路径路由可能为空） */
    union {
        NETMGR_NEVP_ADDR  addr;  /**< 地址事件载荷 */
        NETMGR_NEVP_ROUTE route; /**< 路由事件载荷 */
    } u; /**< 按 type 取用的载荷；链路事件无载荷 */
} NETMGR_NEV;

/**
 * @brief netlink 事件回调，在事件接收线程上下文执行（只做通知，无返回值）
 * @param ev   事件内容，仅在回调期间有效，不可保存指针
 * @param user 注册回调时传入的用户指针，可为空
 */
typedef void (*netmgr_nev_cb)(const NETMGR_NEV* ev, void* user);

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
