/**
 * @file net_mgr_dhcp.h
 * @brief netmgr 系统适配层：DHCP 取址（sys 层，可独立使用）
 * @details 采用「长驻会话」模型（DHCP 不是一次性取址：取不到租约要重试、取到后要续租）：
 *          1) 每个接口对应一个取址线程，线程内用 popen 执行 udhcpc（前台）并逐行判定输出；
 *          2) 取到租约回调 SUCC，此后 udhcpc 常驻并由它自身续租，续租成功不重复上报；
 *          3) 取不到租约时 udhcpc 自身持续重试且不退出；会话内超过判定时长（10 秒）
 *             仍未取到即回调一次 FAIL，此后再取到则回调 SUCC 形成状态翻转；
 *             仅当 udhcpc 异常退出且本次从未取到租约时再回调 FAIL，并由本层节流重建会话；
 *          4) 已取到租约后若退回重新获取阶段（续租失败）回调一次 LOST，重新取到再回调 SUCC；
 *          5) 会话内解析租约输出（地址、租约时长），可经 netmgr_dhcp_lease_get 查询；
 *          6) 会话与链路同生命周期：start 建立、stop 结束，重试与续租均不属于调用方职责；
 *          7) 线程以脏标志占用槽位，槽位表由本层互斥锁保护。
 *          本层不读取接口地址，调用方按回调给出的接口名自行读取，保持 sys 层各能力相互独立。
 *
 * 使用约束（独立调用方须知）：
 * 1) 单实例：进程内维护 4 个取址槽位，同一接口不可并发启动两份；
 * 2) 线程：每接口常驻 1 个取址线程（续租期间同样占用），回调在其上下文执行，不可长时间阻塞；
 * 3) 权限：需可执行 udhcpc 且具备网络配置权限；
 * 4) 外部依赖：udhcpc（停止依赖其 -p 参数生成的 pidfile）。
 */
#ifndef __NET_MGR_DHCP_H__
#define __NET_MGR_DHCP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_limits.h"

/** DHCP 取址状态 */
typedef enum {
    NETMGR_DHCP_RUNNING = 0, /**< 会话已建立，正在取址 */
    NETMGR_DHCP_SUCC,        /**< 已取得租约（重新取到同样上报） */
    NETMGR_DHCP_FAIL,        /**< 取址失败：超过判定时长未取到，或进程异常退出 */
    NETMGR_DHCP_LOST         /**< 租约侧进入重新获取（续租失败）。注意：udhcpc 不监听 carrier，
                                  插拔网线不会立即触发，时延可达租约时长；插拔感知以链路状态为准 */
} NETMGR_DHCP_STATE;

/** DHCP 租约信息，供业务做租期告警与链路决策 */
typedef struct {
    char ip[NETMGR_IP_LEN]; /**< 租约地址，无租约时为空串 */
    int leaseSec;           /**< 租约时长（秒），未知为 0 */
    int remainSec;          /**< 查询时刻的剩余租期（秒），无租约、租期未知或已过期为 0 */
    int valid;              /**< 1=当前持有租约（租期未知时按持有处理，过期置 0） */
} NETMGR_DHCP_LEASE;

/**
 * @brief DHCP 取址状态回调，在取址线程上下文执行（只做通知，返回值不参与流程）
 * @details SUCC 只在会话首次拿到租约时上报一次（后续续租不再上报）；
 *          FAIL 有两种来源：① 会话内超过判定时长仍未取到租约（对端无响应，udhcpc 继续重试，
 *          失败后成功则再报 SUCC）；② udhcpc 异常退出且本次从未取到租约（本层自动节流重建）；
 *          重试与续租策略完全由本层决定，调用方无需干预
 * @param state  取址状态
 * @param ifname 接口名，回调期间有效，不可保存指针
 * @param err    状态为 FAIL 时的错误码（负值），其余状态恒为 0
 * @param user   注册回调时传入的用户指针，可为空
 */
typedef void (*netmgr_dhcp_cb)(NETMGR_DHCP_STATE state, const char* ifname,
                               int err, void* user);

/* ===== 查询 ===== */

/**
 * @brief 查询指定接口的 DHCP 会话是否在运行
 * @details 会话从 netmgr_dhcp_start 建立到 netmgr_dhcp_stop 结束期间恒为运行中，
 *          取到租约后的续租阶段同样属于运行中
 * @param ifname 接口名，不可为空
 * @return 1 会话运行中；0 已结束或未启动
 */
int netmgr_dhcp_running(const char* ifname);

/**
 * @brief 查询指定接口当前租约信息
 * @details 数据来自会话内解析的租约输出；剩余租期按「获取时刻 + 租约时长 - 当前时刻」推算，
 *          过期后 valid 置 0；租期未知（输出未给 lease time）时按持有处理：valid=1、remainSec=0；
 *          本接口只读快照，不影响会话
 * @param ifname 接口名，不可为空
 * @param info   输出租约信息，不可为空
 * @return 0 成功（无会话或无租约时 valid=0，其余字段为空/0）；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_lease_get(const char* ifname, NETMGR_DHCP_LEASE* info);

/* ===== 操作 ===== */

/**
 * @brief 在指定接口上启动 DHCP 取址会话，结果经回调返回
 * @details 会话以 udhcpc 前台方式运行（发包节奏取其默认值）：取不到租约由它自身持续重试，
 *          取到后常驻并由它自身续租，会话一直持续到 netmgr_dhcp_stop；
 *          调用方无需（也不应）自行周期重拉来维持租约
 * @param ifname 接口名，不可为空
 * @param cb     取址状态回调，可为空（不需要通知时）
 * @param user   回调透传的用户指针，可为空
 * @return 0 已受理；负值为 NETMGRERR_* 错误码（该接口已有会话返回 -NETMGRERR_DHCP）
 */
int netmgr_dhcp_start(const char* ifname, netmgr_dhcp_cb cb, void* user);

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
