/**
 * @file net_mgr_dhcp.h
 * @brief netmgr 系统适配层：DHCP 取址（sys 层，可独立使用）
 * @details 按接口维护独立的 udhcpc 实例，每个实例由「子进程 + 输出收尾线程」组成，
 *          支持多条链路并发取址；取址结果由收尾线程判定并释放槽位。
 */
#ifndef __NET_MGR_DHCP_H__
#define __NET_MGR_DHCP_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 查询 ===== */

/**
 * @brief 查询指定接口的 DHCP 是否仍在运行
 * @param ifname 接口名，不可为空
 * @return 1 运行中；0 已结束或未启动
 */
int netmgr_dhcp_busy(const char* ifname);

/* ===== 操作 ===== */

/**
 * @brief 在指定接口上启动 DHCP 取址
 * @param ifname  接口名，不可为空
 * @param timeout 单次请求超时秒数，0 表示使用 udhcpc 默认值
 * @return 0 成功（该接口已在取址中时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_start(const char* ifname, int timeout);

/**
 * @brief 停止指定接口上的 DHCP 取址，并回收最近一次取址的结果
 * @param ifname 接口名，不可为空
 * @return 0 成功（未启动时同样返回 0）；负值为 NETMGRERR_* 错误码，
 *         其中包含该接口最近一次异步取址失败的错误码
 */
int netmgr_dhcp_stop(const char* ifname);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_DHCP_H__ */
