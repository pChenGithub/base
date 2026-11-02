/**
 * @file net_mgr_core.h
 * @brief netmgr 仲裁层接口（core 层）
 * @details 链路装配、优先级仲裁、默认路由同步与事件分发的全部实现均在 core 层；
 *          api 层只做入参校验与转调。本层依赖 common + sys + linktype，不依赖 api 层。
 */
#ifndef __NET_MGR_CORE_H__
#define __NET_MGR_CORE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_types.h"

/* ===== 生命周期 ===== */

/**
 * @brief 启动仲裁核心，按配置拉起全部启用的链路并开始周期仲裁
 * @param conf 总配置，不可为空；配置内容在函数内复制，调用后外部可释放
 * @param cb   链路状态回调，可为空（不需要通知时）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_start(const NETMGR_CONF* conf, netmgr_ev_cb cb);

/**
 * @brief 停止仲裁核心，关闭全部链路并删除本模块下发的默认路由
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_stop(void);

/* ===== 查询 ===== */

/**
 * @brief 读取指定类型链路的当前优先级
 * @param type 链路类型
 * @param prio 输出当前优先级，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_prio_get(NETMGR_TYPE type, unsigned char* prio);

/**
 * @brief 查询全部链路的状态快照
 * @param list 输出数组，容量由 *num 传入
 * @param num  入参为数组容量，出参为实际写入条数
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_stat_get(NETMGR_LINK* list, int* num);

/* ===== 运行时配置 ===== */

/**
 * @brief 运行时修改指定类型链路的优先级
 * @param type 链路类型
 * @param prio 新优先级（0-255，小者优先）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_prio_set(NETMGR_TYPE type, unsigned char prio);

/**
 * @brief 重新配置以太网链路（会先关闭再按新配置拉起）
 * @param conf 以太网配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_eth_set(const NETMGR_ETH_CONF* conf);

/**
 * @brief 重新配置 WiFi 链路（STA 切换 SSID 或 AP 重载均走此接口）
 * @param conf WiFi 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_wifi_set(const NETMGR_WIFI_CONF* conf);

/**
 * @brief 重新配置 4G 链路
 * @param conf 4G 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_cell_set(const NETMGR_CELL_CONF* conf);

/**
 * @brief 下发 DNS 服务器地址（写入系统 DNS 配置并同步内部记录）
 * @param dns 单个 IPv4 地址字符串，为空或空串表示清除已下发的 DNS
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_dns_set(const char* dns);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_CORE_H__ */
