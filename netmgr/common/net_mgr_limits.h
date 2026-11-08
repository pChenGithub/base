/**
 * @file net_mgr_limits.h
 * @brief netmgr 全模块共用的定长缓冲尺寸（common 层）
 * @details 仅承载跨层共用的长度常量，不定义业务数据类型；
 *          数据类型随其产出模块存放：
 *          链路类型 / 状态 / 事件 / 快照 / 配置 → `core/net_mgr_core.h`，
 *          默认路由条目 → `sys/net_mgr_route.h`，
 *          netlink 事件 → `sys/net_mgr_event.h`。
 */
#ifndef __NET_MGR_LIMITS_H__
#define __NET_MGR_LIMITS_H__

#ifdef __cplusplus
extern "C" {
#endif

/** 接口名字符串长度上限（含结束符）；被多处结构体成员与长度校验共用，故具名 */
#define NETMGR_IF_LEN    16
/** IPv4 点分十进制字符串长度上限（含结束符）；被多处结构体成员与长度校验共用，故具名 */
#define NETMGR_IP_LEN    16

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_LIMITS_H__ */
