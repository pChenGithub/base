/**
 * @file net_mgr_arbiter.h
 * @brief netmgr 仲裁层内部契约：**仲裁器**（纯机制，不含生命周期与对外接口）
 *
 * ## 为什么独立成模块
 * 仲裁是 netmgr 的核心机制：它决定「每条链路此刻是否可用」「谁承担默认路由」
 * 「业务该收到哪些事件」。它既不属于生命周期管理（start/stop），也不属于对外
 * API（配置下发）。独立出来后，机制可被单独阅读、单独测试，core 只负责驱动它。
 *
 * ## 仲裁机制（一轮 = 五阶段 + 一次产出）
 * ```
 *   ┌───────────────────────────────────────────────────────────────────┐
 *   │ 阶段 1 Refresh  逐条链路把「内核与进程的真实状态」投影进上下文      │
 *   │                 （iface ioctl / 路由回查为只读；不启停任何进程）     │
 *   ├───────────────────────────────────────────────────────────────────┤
 *   │ 阶段 2 Diff     与刷新前快照比对，决定要产生哪些事件：               │
 *   │                 · 状态翻转 → ENABLED / UP / DOWN                   │
 *   │                 · 字段变化 → INFO_CHANGE（IP/MASK/GATE 位图+前后值）│
 *   │                 · WiFi 关联变化 → WIFI_CONNECTED / DISCONNECTED     │
 *   ├───────────────────────────────────────────────────────────────────┤
 *   │ 阶段 3 Route    按「就绪 + 有网关」同步默认路由：                    │
 *   │                 metric = NETMGR_METRIC_BASE + prio；同接口只留一条  │
 *   ├───────────────────────────────────────────────────────────────────┤
 *   │ 阶段 4 Elect    就绪且有网关的链路中，prio 最小者成为主链路（ACTIVE）│
 *   │                 AP 等无网关链路不参与选举                           │
 *   ├───────────────────────────────────────────────────────────────────┤
 *   │ 阶段 5 Apply    主链路变化时：先下发 DNS，再产出 SWITCH 事件         │
 *   └───────────────────────────────────────────────────────────────────┘
 *   产出：事件数组（值语义）+ 新一轮快照 + 本轮首个错误码
 * ```
 *
 * ## 三条硬约束（违反会破坏线程与职责边界）
 * 1. **不做启停动作**：仲裁不得调用 dhcp/wpa/ap 的 start/stop。这些模块的回调
 *    可能就在调用链上（如 DHCP 会话线程），启停会造成自 join 死锁。
 * 2. **不加锁**：调用方（core）在外层持 `g_core.lock` 串行化；仲裁器不感知锁。
 * 3. **事件锁内收集、锁外分发**：仲裁只产出事件数组，分发由 core 在解锁后做，
 *    业务回调里再调本模块接口才不会自死锁。
 */
#ifndef __NET_MGR_ARBITER_H__
#define __NET_MGR_ARBITER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_core_int.h"

/**
 * 单轮仲裁最多产出的事件数
 * 预算：三链路 ×（状态翻转 + WiFi 变化 + 信息变化）+ 主链路切换 + 余量
 * 取值依据：超出即丢弃（防御性上限，避免动态分配），正常规模远小于此值
 */
#define NETMGR_ARB_EV_MAX  16

/**
 * 仲裁器跨轮次状态
 * 用途：保存「上一轮的选举结果」与「已下发的 DNS」，用于产生差异事件与去重
 * 单位/范围：active 取 -1（无主链路）或 0~NETMGR_TYPE_MAX-1；dns 为 IPv4 点分串
 * 默认值依据：reset 时 active=-1、dns 为空，表示「尚未选举、尚未下发过」
 * 写入方/读取方：仅仲裁器写；core 的配置变更（dns_set）会直接写入 dns 字段
 * 生命周期：随 core 生命周期，start 重建、stop 复位
 */
typedef struct {
    int active;              /**< 当前主链路下标，-1 表示无主链路 */
    char dns[NETMGR_IP_LEN]; /**< 已下发到系统的 DNS，用于避免重复写盘 */
} NETMGR_ARB;

/**
 * @brief 复位仲裁器状态（start/stop 时调用）
 * @param arb 仲裁器，可为空
 */
void netmgr_arb_reset(NETMGR_ARB* arb);

/**
 * @brief 执行一轮仲裁（五阶段，见文件头说明）
 * @details 调用方须已持有 core 的仲裁锁；本函数不启停进程、不调用业务回调
 * @param arb   仲裁器状态，不可为空
 * @param links 三条链路上下文数组（长度 NETMGR_TYPE_MAX），不可为空
 * @param ops   链路类型到操作表的映射（元素可为空表示该类型不支持），不可为空
 * @param evs   事件输出数组，容量由 max 指定，不可为空
 * @param max   事件数组容量，须大于 0
 * @param num   输出事件条数，不可为空
 * @return 0 本轮各阶段均成功；负值为本轮**首个**失败步骤的错误码
 */
int netmgr_arb_run(NETMGR_ARB* arb, NETMGR_CTX* links, const NETMGR_OPS* const ops[],
                   NETMGR_EVENT* evs, int max, int* num);

/**
 * @brief 导出全部链路快照（含 ACTIVE 呈现）
 * @details ACTIVE 不写回上下文，只在对外快照里体现，保证上下文只表达「客观状态」
 * @param arb   仲裁器（用于判定谁是主链路），可为空（此时不标 ACTIVE）
 * @param links 链路上下文数组，不可为空
 * @param list  输出数组，不可为空
 * @param num   入参为容量，出参为实际写入条数，不可为空
 */
void netmgr_arb_snapshot(const NETMGR_ARB* arb, const NETMGR_CTX* links,
                         NETMGR_LINK* list, int* num);

/**
 * @brief 导出单条链路快照（含 ACTIVE 呈现）
 * @param arb   仲裁器，可为空
 * @param links 链路上下文数组基址，不可为空
 * @param index 目标链路下标，范围 0~NETMGR_TYPE_MAX-1
 * @param out   输出快照，不可为空
 */
void netmgr_arb_snapshot_one(const NETMGR_ARB* arb, const NETMGR_CTX* links,
                             int index, NETMGR_LINK* out);

/**
 * @brief 构造一条「以完整链路快照承载」的事件（UP/DOWN/ENABLED/DISABLED）
 * @param ev   待填充事件，不可为空
 * @param type 事件类型
 * @param link 链路快照，不可为空
 */
void netmgr_arb_ev_link(NETMGR_EVENT* ev, NETMGR_EV type, const NETMGR_LINK* link);

/**
 * @brief 向事件数组追加一条事件（值拷贝，满则丢弃）
 * @param evs 事件数组，不可为空
 * @param max 数组容量
 * @param num 当前条数，不可为空
 * @param ev  待追加事件，不可为空
 */
void netmgr_arb_ev_push(NETMGR_EVENT* evs, int max, int* num, const NETMGR_EVENT* ev);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_ARBITER_H__ */
