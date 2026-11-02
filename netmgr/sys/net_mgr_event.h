/**
 * @file net_mgr_event.h
 * @brief netmgr 系统适配层：netlink 事件监听（sys 层，可独立使用）
 * @details 采用「接收线程 + 唤醒回调」模型：事件只触发一次回调，
 *          具体状态判定由调用方（core 层）自行完成，本层不含任何状态机。
 */
#ifndef __NET_MGR_EVENT_H__
#define __NET_MGR_EVENT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 操作 ===== */

/**
 * @brief 启动 netlink 事件监听（链路、地址、路由变化）
 * @param wake 事件到达时的唤醒回调，可为空
 * @return 0 成功（已在运行时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_event_open(void (*wake)(void));

/**
 * @brief 停止 netlink 事件监听并回收线程
 * @return 0 成功（未启动时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_event_close(void);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_EVENT_H__ */
