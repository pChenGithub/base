/**
 * @file net_mgr_ctx.h
 * @brief netmgr 内部契约：链路上下文、链路操作表与路由条目（common 层）
 * @details 本文件为 linktype 层与 core 层之间的共享契约，供模块内部使用；
 *          宿主工程不应直接包含。链路层只读写 priv 与状态字段，其余字段归 core 层独占。
 */
#ifndef __NET_MGR_CTX_H__
#define __NET_MGR_CTX_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_types.h"

/** 单一链路的运行时上下文 */
typedef struct {
    NETMGR_TYPE type;               /**< 链路类型 */
    NETMGR_STATE state;             /**< 当前状态（仅 DOWN/READY，ACTIVE 由仲裁结果得出） */
    NETMGR_STATE prev;              /**< 上一轮刷新得到的状态，用于判定状态翻转 */
    unsigned char prio;             /**< 当前优先级 */
    int enable;                     /**< 1=该链路已启用 */
    int route_on;                   /**< 1=本模块已为该链路下发默认路由 */
    int rmetric;                    /**< 已下发路由使用的 metric，-1 表示未下发 */
    char rgate[NETMGR_IP_LEN];      /**< 已下发路由使用的网关 */
    char ifname[NETMGR_IF_LEN];     /**< 接口名 */
    char ip[NETMGR_IP_LEN];         /**< 当前 IP */
    char mask[NETMGR_IP_LEN];       /**< 当前掩码 */
    char gate[NETMGR_IP_LEN];       /**< 当前网关 */
    char dns[NETMGR_IP_LEN];        /**< DNS 服务器 */
    void* priv;                     /**< 各链路私有数据，由各实现自行分配释放 */
} NETMGR_CTX;

/** 链路操作表，每类链路提供一份实现 */
typedef struct {
    /**
     * @brief 拉起链路：配置地址、启动外部进程、开始取址
     * @param ctx  链路上下文
     * @param conf 链路配置，由调用方保证类型与链路匹配
     * @return 0 成功；负值为 NETMGRERR_* 或底层错误码
     */
    int (*open)(NETMGR_CTX* ctx, const void* conf);

    /**
     * @brief 关闭链路并释放其占用的资源
     * @param ctx 链路上下文
     * @return 0 成功；负值为 NETMGRERR_* 或底层错误码
     */
    int (*close)(NETMGR_CTX* ctx);

    /**
     * @brief 刷新链路实时状态（IP、网关、就绪标志）
     * @param ctx 链路上下文
     * @return 0 成功；负值为 NETMGRERR_* 或底层错误码
     */
    int (*refresh)(NETMGR_CTX* ctx);
} NETMGR_OPS;

/** 默认路由条目 */
typedef struct {
    char ifname[NETMGR_IF_LEN]; /**< 出口接口名 */
    char gate[NETMGR_IP_LEN];   /**< 网关地址 */
    int metric;                 /**< 路由 metric */
} NETMGR_ROUTE;

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_CTX_H__ */
