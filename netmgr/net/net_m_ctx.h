/**
 * @file net_m_ctx.h
 * @brief net 网络管理模块内部契约：链路槽、操作表、仲裁与路由同步（net 层内部）
 * @details 仅供 net 层内部各文件使用，宿主工程只包含 net_m_def.h。
 *          字段归属：链路实现只写 priv / ifname / ip / mask / gate / dns / state / err，
 *          其余字段（prio、route_on、rmetric、fault、prev）归仲裁与路由同步独占。
 *          线程归属：本层全部链路槽只由管理线程写，故结构体内不含任何锁；
 *          DHCP 回调、netlink 回调只投递消息，不直接改这些字段。
 */
#ifndef __NET_M_CTX_H__
#define __NET_M_CTX_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_m_def.h"
#include "net_mgr_dhcp.h"

/* ===== 常量 ===== */

/** 仲裁轮询默认间隔（毫秒）：配置未指定（tickMs<=0）时使用 */
#define NET_M_TICK_MS       500
/** 单轮仲裁事件产出上限；正常规模远小于该值，超出即丢弃（防御性上限） */
#define NET_M_EV_CAPACITY   16
/** 事件环形队列容量：netlink 事件、DHCP 回调与路由事件共用，满则置溢出标志转全量刷新 */
#define NET_M_QUEUE_CAPACITY 32
/** 管理线程等待命令的最长时间（毫秒）：决定定时仲裁的实际间隔精度 */
#define NET_M_WAIT_SLICE_MS 200

/* ===== 类型定义 ===== */

/** 单一链路的运行时上下文（仅管理线程写） */
typedef struct {
    NET_M_TYPE type;             /**< 链路类型 */
    NET_M_STATE state;           /**< 当前客观状态（仅 DISABLED/DOWN/READY） */
    NET_M_STATE prev;            /**< 上一轮状态，用于判定翻转 */
    unsigned char prio;          /**< 当前优先级，等同路由 metric */
    int enable;                  /**< 1=该链路已启用 */
    int carrier;                 /**< 1=物理连通（管理态 UP 且有载波），由 refresh 写入 */
    int carrierSeen;             /**< 1=本次启用后已记录过首轮载波，用于抑制首轮事件 */
    int route_on;                /**< 1=本模块已为该链路下发默认路由 */
    int rmetric;                 /**< 已下发路由的 metric，-1 表示未下发 */
    char rgate[16];              /**< 已下发路由使用的网关 */
    char ifname[16];             /**< 接口名，未启用时为空串 */
    char ip[16];                 /**< 当前 IP */
    char mask[16];               /**< 当前掩码 */
    char gate[16];               /**< 当前网关 */
    char dns[16];                /**< DNS 服务器 */
    int err;                     /**< 业务语义异常码，0 表示无异常 */
    int fault;                   /**< 内部故障码，0 表示无故障 */
    int faultStage;              /**< 故障阶段（NET_M_STAGE） */
    int dhcpSeen;                /**< 1=已收到过该链路的 DHCP 状态回调 */
    NETMGR_DHCP_STATE dhcpState;/**< 最近一次 DHCP 状态（由管理线程写入） */
    int dhcpErr;                 /**< 最近一次 DHCP 失败错误码 */
    long long retryAt;           /**< 下次允许重试的时刻（毫秒），0 表示不限制 */
    unsigned int retryCount;     /**< 连续失败次数，用于重试退避 */
    NET_M_WIFI_INFO wifi;        /**< WiFi 连接信息，仅 WiFi 链路刷新时填写 */
    void* priv;                  /**< 各链路私有数据，由实现自行分配释放 */
} NET_M_LINK_RT;

/**
 * 链路私有数据公共头：各链路 priv 结构体的首成员
 * @note DHCP 回调只拿到 user 指针，靠「首成员地址与结构体地址相同」这一约定
 *       取回链路类型；新增链路实现时 priv 必须以此类型开头
 */
typedef struct {
    NET_M_TYPE type; /**< 所属链路类型，回调上报事件用 */
} NET_M_HEAD;

/** 链路操作表，每类链路提供一份实现 */
typedef struct {
    /**
     * @brief 拉起链路：配置地址、启动外部进程、开始取址
     * @param link 链路运行时上下文
     * @param conf 链路配置，调用方保证类型与链路匹配
     * @return 0 成功；负值为错误码
     */
    int (*open)(NET_M_LINK_RT* link, const void* conf);

    /**
     * @brief 关闭链路并释放其占用的资源
     * @details 除停止取址 / 关联进程外，还须把接口管理态置 DOWN
     *          （等价 ip link set <ifname> down，地址保留），
     *          保证"禁用链路"与系统命令效果一致
     * @param link 链路运行时上下文
     * @return 0 成功；负值为错误码
     */
    int (*close)(NET_M_LINK_RT* link);

    /**
     * @brief 刷新链路实时状态：只把内核与进程的实际状态投影进上下文
     * @details 只写 ip / mask / gate / wifi / carrier / state / err，
     *          不得拉起或停止取址进程、不得改配置；
     *          carrier 表示物理连通（管理态 UP 且有载波），与 state（是否就绪）解耦
     * @param link 链路运行时上下文
     * @return 0 成功；负值为错误码
     */
    int (*refresh)(NET_M_LINK_RT* link);

    /**
     * @brief 自愈：链路处于 DOWN 且到达退避时间时执行一次修复动作
     * @details 与 refresh 的分工：refresh 只读投影，本钩子才是唯一允许「重新拉起」的地方，
     *          因此自愈时机与退避策略由管理线程统一决定，仲裁器保持纯逻辑。
     *          为空表示该链路不具备自愈能力（如 DHCP 由 sys 层自行重试）
     * @param link 链路运行时上下文
     * @return 0 修复完成；负值为错误码（管理线程据此进入退避）
     */
    int (*heal)(NET_M_LINK_RT* link);
} NET_M_OPS;

/** 仲裁器跨轮次状态 */
typedef struct {
    int active;      /**< 当前主链路下标，-1 表示无主链路 */
    char dns[16];    /**< 已下发到系统的 DNS，用于避免重复写盘 */
} NET_M_ARB;

/** DHCP 回调投递到管理线程的消息（回调线程只填这一份，不触碰链路槽） */
typedef struct {
    NET_M_TYPE type;              /**< 所属链路类型 */
    NETMGR_DHCP_STATE state;     /**< 取址状态 */
    int err;                      /**< FAIL 时的错误码 */
} NET_M_DHCP_EVT;

/* ===== 链路操作表 ===== */

/** 以太网链路操作表 */
extern const NET_M_OPS net_m_eth_ops;
/** WiFi 链路操作表 */
extern const NET_M_OPS net_m_wifi_ops;
/** 4G 蜂窝链路操作表 */
extern const NET_M_OPS net_m_cell_ops;

/* ===== 状态判定 ===== */

/**
 * @brief 按「物理连通 + 已取地址 + 路由表有默认网关」判定 DHCP 链路就绪
 * @details 未就绪时状态置为 DOWN；接口不存在时置 DOWN 并写 -NETMGRERR_NOTFOUND 到 err
 * @param link 链路运行时上下文，不可为空
 * @return 0 成功（未就绪亦为 0）；负值为错误码
 */
int net_m_link_ready(NET_M_LINK_RT* link);

/**
 * @brief 按「物理连通 + 已取地址 + 配置网关非空」判定静态地址链路就绪
 * @param link 链路运行时上下文，不可为空
 * @param gate 配置网关，不可为空（空串表示不参与默认路由，判为 DOWN）
 * @return 0 成功（未就绪亦为 0）；负值为错误码
 */
int net_m_link_ready_static(NET_M_LINK_RT* link, const char* gate);

/**
 * @brief DHCP 链路就绪判定：接口消失判定 + 通用判定 + DOWN 原因归因
 * @details 以太网、WiFi、4G 三条 DHCP 链路共用，避免归因规则多处维护后漂移
 * @param link 链路运行时上下文，不可为空
 * @return 0 成功（未就绪亦为 0）；负值为错误码
 */
int net_m_link_ready_dhcp(NET_M_LINK_RT* link);

/**
 * @brief 按 DHCP 会话状态归因 DOWN 原因（仅当状态为 DOWN 时生效）
 * @details 取址失败给出具体错误码；曾取到租约后中断置 -NETMGRERR_DHCP
 * @param link 链路运行时上下文，不可为空
 */
void net_m_link_attr_dhcp(NET_M_LINK_RT* link);

/* ===== DHCP 回调 ===== */

/**
 * @brief 三类链路共用的 DHCP 状态回调（在取址线程上下文执行）
 * @details 只把状态投递到管理线程的消息队列，不直接写链路槽，
 *          因此不存在跨线程改写 NET_M_LINK_RT 的问题
 * @param state  取址状态
 * @param ifname 接口名，可为空（为空直接忽略）
 * @param err    状态为 FAIL 时的错误码
 * @param user   注册时传入的链路私有数据（首成员须为 NET_M_HEAD）
 */
void net_m_dhcp_notify(NETMGR_DHCP_STATE state, const char* ifname, int err, void* user);

/* ===== 路由同步 ===== */

/**
 * @brief 按链路当前状态与优先级同步其默认路由
 * @details metric 直接取 prio（小者优）；网关或 metric 未变且已下发时不触碰内核。
 *          删除失败时保留 route_on 以便下一轮重试，不把状态改成「已撤销」
 * @param link 链路运行时上下文，不可为空
 * @return 0 成功或无需变更；负值为错误码
 */
int net_m_route_sync(NET_M_LINK_RT* link);

/**
 * @brief 撤销某链路的默认路由（幂等）；失败时保留 route_on
 * @param link 链路运行时上下文，不可为空
 * @return 0 成功或无需撤销；负值为错误码
 */
int net_m_route_revoke(NET_M_LINK_RT* link);

/* ===== 仲裁 ===== */

/**
 * @brief 复位仲裁器状态（start / stop 时调用）
 * @param arb 仲裁器，可为空
 */
void net_m_arb_reset(NET_M_ARB* arb);

/**
 * @brief 执行一轮仲裁：刷新 → 差异 → 路由 → 选举 → DNS/切换
 * @details 调用方为管理线程（唯一写者），本函数不起停进程、不调用业务回调
 * @param arb   仲裁器状态，不可为空
 * @param links 链路上下文数组（长度 NET_M_TYPE_MAX），不可为空
 * @param ops   链路类型到操作表的映射，不可为空
 * @param evs   事件输出数组，不可为空
 * @param max   事件数组容量，须大于 0
 * @param num   输出事件条数，不可为空
 * @return 0 本轮各阶段成功；负值为本轮首个失败步骤的错误码
 */
int net_m_arb_run(NET_M_ARB* arb, NET_M_LINK_RT* links, const NET_M_OPS* const ops[],
                  NET_M_EVENT* evs, int max, int* num);

/**
 * @brief 导出单条链路快照（叠加 ACTIVE 呈现态）
 * @param arb   仲裁器，可为空
 * @param links 链路上下文数组基址，不可为空
 * @param index 目标链路下标，范围 0~NET_M_TYPE_MAX-1
 * @param out   输出快照，不可为空
 */
void net_m_arb_snapshot_one(const NET_M_ARB* arb, const NET_M_LINK_RT* links,
                            int index, NET_M_LINK* out);

/**
 * @brief 导出全部链路快照（叠加 ACTIVE 呈现态）
 * @param arb   仲裁器，可为空
 * @param links 链路上下文数组，不可为空
 * @param list  输出数组，不可为空
 * @param num   入参为容量，出参为实际写入条数，不可为空
 */
void net_m_arb_snapshot(const NET_M_ARB* arb, const NET_M_LINK_RT* links,
                        NET_M_LINK* list, int* num);

/**
 * @brief 追加一条事件（值拷贝，满则丢弃）
 * @param evs 事件数组，不可为空
 * @param max 数组容量
 * @param num 当前条数，不可为空
 * @param ev  待追加事件，不可为空
 */
void net_m_arb_ev_push(NET_M_EVENT* evs, int max, int* num, const NET_M_EVENT* ev);

#ifdef __cplusplus
}
#endif
#endif /* __NET_M_CTX_H__ */
