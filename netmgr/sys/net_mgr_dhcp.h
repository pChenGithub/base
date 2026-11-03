/**
 * @file net_mgr_dhcp.h
 * @brief netmgr 系统适配层：DHCP 取址（sys 层，可独立使用）
 * @details 实现方式与 netopt 的 net_dhcp 保持一致：
 *          1) 每个接口对应一个取址线程，线程内用 popen 执行 udhcpc 并逐行判定输出；
 *          2) 取址状态经回调返回（RUNNING / SUCC / FAIL），失败时回调可要求重试；
 *          3) 线程以脏标志占用槽位，槽位表由本层互斥锁保护。
 *          本层不读取接口地址，调用方按回调给出的接口名自行读取，保持 sys 层各能力相互独立。
 */
#ifndef __NET_MGR_DHCP_H__
#define __NET_MGR_DHCP_H__

#ifdef __cplusplus
extern "C" {
#endif

/** DHCP 取址状态 */
typedef enum {
    NETMGR_DHCP_RUNNING = 0, /**< 已发起取址，正在等待租约 */
    NETMGR_DHCP_SUCC,        /**< 取址成功（未捕获到失败输出） */
    NETMGR_DHCP_FAIL         /**< 取址失败（捕获到失败输出或执行异常） */
} NETMGR_DHCP_STATE;

/** 回调处理结果 */
typedef enum {
    NETMGR_DHCP_RET_OK = 0, /**< 正常返回，本次取址流程结束 */
    NETMGR_DHCP_RET_AGAIN   /**< 重新执行一次取址（仅 FAIL 状态有效，重试次数由调用方控制） */
} NETMGR_DHCP_RET;

/**
 * @brief DHCP 取址状态回调，在取址线程上下文执行
 * @param state  取址状态
 * @param ifname 接口名，回调期间有效，不可保存指针
 * @param err    状态为 FAIL 时的错误码（负值），其余状态恒为 0
 * @param user   注册回调时传入的用户指针，可为空
 * @return NETMGR_DHCP_RET_OK 正常返回；状态为 FAIL 时返回 NETMGR_DHCP_RET_AGAIN
 *         表示重新执行一次取址。重试次数完全由调用方控制（本层不设上限），
 *         持续返回 AGAIN 会一直重试，调用方需自行限制次数或条件
 */
typedef NETMGR_DHCP_RET (*netmgr_dhcp_cb)(NETMGR_DHCP_STATE state, const char* ifname,
                                          int err, void* user);

/* ===== 查询 ===== */

/**
 * @brief 查询指定接口的 DHCP 是否仍在取址中
 * @param ifname 接口名，不可为空
 * @return 1 取址线程运行中；0 已结束或未启动
 */
int netmgr_dhcp_busy(const char* ifname);

/* ===== 操作 ===== */

/**
 * @brief 在指定接口上启动 DHCP 取址，结果经回调返回
 * @details timeout 为 0 时不追加 -nq，udhcpc 取到租约后转后台继续续租；
 *          timeout 大于 0 时追加 "-nq -T timeout"，取到或失败即退出
 * @param ifname  接口名，不可为空
 * @param timeout 单次请求超时秒数，0 表示由 udhcpc 自行决定
 * @param cb      取址状态回调，可为空（不需要通知时）
 * @param user    回调透传的用户指针，可为空
 * @return 0 已受理；负值为 NETMGRERR_* 错误码（该接口已在取址中返回 -NETMGRERR_DHCP）
 */
int netmgr_dhcp_start(const char* ifname, int timeout, netmgr_dhcp_cb cb, void* user);

/**
 * @brief 停止指定接口上的 DHCP 取址
 * @details 通过 udhcpc 的 pidfile 精确结束取址进程，线程读到输出结束后自行收尾；
 *          取址失败原因由回调的 FAIL 状态给出，本接口只反映停止动作本身的成败
 * @param ifname 接口名，不可为空
 * @return 0 成功（未启动时同样返回 0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_stop(const char* ifname);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_DHCP_H__ */
