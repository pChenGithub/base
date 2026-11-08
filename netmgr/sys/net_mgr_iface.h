/**
 * @file net_mgr_iface.h
 * @brief netmgr 系统适配层：网络接口操作（sys 层，可独立使用）
 * @details 全部基于 ioctl(SIOCGIFFLAGS/SIOCGIFADDR/SIOCSIFADDR/SIOCSIFNETMASK) 实现，
 *          仅依赖内核，不引用 netmgr 上层与 base 内其它自研模块。
 *
 * 使用约束（独立调用方须知）：
 * 1) 单实例：无文件级状态，可并发调用；
 * 2) 线程：本层不起线程，全部接口同步返回；
 * 3) 权限：enable/setip/setmask 需 CAP_NET_ADMIN，查询接口无需特权；
 * 4) 外部依赖：仅内核 ioctl。
 */
#ifndef __NET_MGR_IFACE_H__
#define __NET_MGR_IFACE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 查询 ===== */

/**
 * @brief 判断网络接口是否存在
 * @param ifname 接口名，不可为空
 * @return 1 存在；0 不存在或入参非法
 */
int netmgr_iface_exist(const char* ifname);

/**
 * @brief 判断接口是否可用（就绪）
 * @details 判定为合取：管理态 IFF_UP 与 IFF_RUNNING 同时置位才算就绪；
 *          内核自 2.6.36 起在 IFF_RUNNING 中反映 carrier（operstate），不再另读 sysfs
 * @note 前提：目标内核 ≥ 2.6.36。更老内核的 IFF_RUNNING 仅表示管理态已 UP、
 *       不反映 carrier，拔线将无法感知；若需支持老内核，应改为「carrier 优先读取，
 *       不可读时退回 IFF_RUNNING」，而不在本函数内混用两种判据
 * @param ifname 接口名，不可为空
 * @return 1 可用；0 不可用（管理态关闭 / 无载波 / 查询失败）
 */
int netmgr_iface_ready(const char* ifname);

/**
 * @brief 读取接口当前 IPv4 地址
 * @param ifname 接口名，不可为空
 * @param ip     输出缓冲，不可为空
 * @param len    输出缓冲长度
 * @return 0 成功（接口尚无地址时返回 0 且输出空串）；负值为 NETMGRERR_* 错误码
 */
int netmgr_iface_ip(const char* ifname, char* ip, int len);

/**
 * @brief 读取接口当前 IPv4 掩码
 * @param ifname 接口名，不可为空
 * @param mask   输出缓冲，不可为空
 * @param len    输出缓冲长度
 * @return 0 成功（接口尚无地址时返回 0 且输出空串）；负值为 NETMGRERR_* 错误码
 */
int netmgr_iface_mask(const char* ifname, char* mask, int len);

/**
 * @brief 在候选接口名中查找第一个存在的接口
 * @param names 候选接口名数组，元素不可为空
 * @param count 候选数量
 * @param out   输出接口名缓冲，不可为空
 * @param len   输出缓冲长度
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
int netmgr_iface_find(const char* const* names, int count, char* out, int len);

/* ===== 操作 ===== */

/**
 * @brief 使能或关闭接口
 * @param ifname 接口名，不可为空
 * @param on     1=启用（置 IFF_UP），0=关闭（清 IFF_UP）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_iface_enable(const char* ifname, int on);

/**
 * @brief 设置接口 IPv4 地址
 * @param ifname 接口名，不可为空
 * @param ip     点分十进制地址，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_iface_setip(const char* ifname, const char* ip);

/**
 * @brief 设置接口 IPv4 掩码
 * @param ifname 接口名，不可为空
 * @param mask   点分十进制掩码，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_iface_setmask(const char* ifname, const char* mask);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_IFACE_H__ */
