/**
 * @file net_mgr.h
 * @brief 网络管理服务（netmgr）对外接口（api 层，宿主工程只包含本文件）
 * @details 统一管理以太网、WiFi（STA/AP）、4G 三类链路：
 *          1) 按配置拉起或关闭链路；
 *          2) 以优先级作为默认路由 metric 下发到内核，实现主备自动切换；
 *          3) 链路状态变化通过回调通知应用；
 *          4) 提供 DNS 下发。
 *          模块按下述层次组织，上层依赖下层，下层可单独编入宿主工程使用：
 *          api -> core -> linktype -> sys -> common
 *          本模块内部不打印任何日志：所有异常均以 NETMGRERR_* 负值返回，
 *          日志由调用方按错误码自行打印；链路侧异常原因随 NETMGR_LINK.err 上报。
 */
#ifndef __NET_MGR_H__
#define __NET_MGR_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_core.h"
#include "net_mgr_err.h"

/* ===== 生命周期 ===== */

/**
 * @brief 启动网络管理服务，按配置拉起全部启用的链路
 * @details 全部事件统一经 cb 返回：链路状态、网络信息变化、默认路由变化、
 *          DHCP 过程与 WiFi 状态；
 *          启动后需由业务周期调用 netmgr_poll 驱动刷新（建议 200~1000ms）
 * @param conf 总配置，不可为空；配置内容在函数内复制，调用后外部可释放
 * @param cb   事件回调，可为空（不需要通知时）
 * @param user 回调透传的用户指针，可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_start(const NETMGR_CONF* conf, netmgr_event_cb cb, void* user);

/**
 * @brief 停止网络管理服务，关闭全部链路并删除本模块下发的默认路由
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_stop(void);

/**
 * @brief 驱动一轮链路状态刷新与仲裁
 * @details 本模块不创建管理线程：netlink 事件变化由事件回调即时驱动，
 *          而 AP 进程退出、DHCP 异步失败、静态地址首次就绪、优先级变更等
 *          不产生 netlink 事件的变化，需由业务周期调用本接口补齐
 * @return 0 成功；负值为本轮首个失败步骤的错误码；未启动返回 -NETMGRERR_STATE
 */
int netmgr_poll(void);

/* ===== 查询 ===== */

/**
 * @brief 读取指定类型链路的当前优先级
 * @param type 链路类型
 * @param prio 输出当前优先级，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_prio_get(NETMGR_TYPE type, unsigned char* prio);

/**
 * @brief 查询全部链路的状态快照
 * @param list 输出数组，容量由 *num 传入
 * @param num  入参为数组容量，出参为实际写入条数
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_stat_get(NETMGR_LINK* list, int* num);

/* ===== 运行时配置 ===== */

/**
 * @brief 运行时修改指定类型链路的优先级
 * @param type 链路类型
 * @param prio 新优先级（0-255，小者优先）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_prio_set(NETMGR_TYPE type, unsigned char prio);

/**
 * @brief 启用或关闭指定链路
 * @details 启用时按最近一次配置重新拉起；关闭时撤掉默认路由并释放链路资源，
 *          链路状态置为 DISABLED；变化通过 ENABLED / DISABLED 事件体现
 * @param type 链路类型
 * @param on   1=启用，0=关闭
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_enable(NETMGR_TYPE type, int on);

/**
 * @brief 重新配置以太网链路（会先关闭再按新配置拉起）
 * @param conf 以太网配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_eth_set(const NETMGR_ETH_CONF* conf);

/**
 * @brief 重新配置 WiFi 链路（STA 切换 SSID 或取址方式、AP 重载均走此接口）
 * @param conf WiFi 配置，不可为空；STA 静态地址需填 use_dhcp=0 与 ip/mask/gate
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wifi_set(const NETMGR_WIFI_CONF* conf);

/**
 * @brief 重新配置 4G 链路
 * @param conf 4G 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_cell_set(const NETMGR_CELL_CONF* conf);

/**
 * @brief 下发 DNS 服务器地址（写入系统 DNS 配置并作用于 AP 模式下的客户端）
 * @param dns 单个 IPv4 地址字符串，为空或空串表示清除已下发的 DNS
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_dns_set(const char* dns);

/* ===== WiFi 扫描 ===== */

/**
 * @brief 触发一次 WiFi 扫描并等待结束，结果直接写入调用方数组
 * @details 需要 WiFi 链路已启用；扫描完成后通过事件回调上报 WIFI_SCAN_DONE，
 *          事件 apNum 与写入 list 的条数一致；超过 *num 容量的条目被丢弃
 * @param list 输出数组，不可为空
 * @param num  不可为空，入参为数组容量，出参为实际写入条数（扫描失败时为 0）
 * @return 0 扫描完成；负值为 NETMGRERR_* 错误码（超时返回 -NETMGRERR_TIMEOUT）
 */
int netmgr_wifi_scan(NETMGR_WIFI_AP* list, int* num);

/**
 * @brief 读取最近一次扫描结果
 * @param list 输出数组，不可为空，容量由 *num 传入
 * @param num  不可为空，入参为数组容量，出参为实际条数
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wifi_scan_result(NETMGR_WIFI_AP* list, int* num);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_H__ */
