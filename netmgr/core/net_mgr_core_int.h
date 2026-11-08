/**
 * @file net_mgr_core_int.h
 * @brief netmgr 仲裁层内部契约：链路上下文、链路操作表与链路实现声明（core 层内部）
 * @details 仅供 core 层内部各文件使用，宿主工程不应包含，对外接口见 net_mgr_core.h。
 *          链路实现（以太网 / WiFi / 4G）与就绪判定均在本层内部，直接依赖 sys 层能力，
 *          不再单独设链路实现层；字段归属：链路实现只写 priv 与状态字段，其余归仲裁独占。
 */
#ifndef __NET_MGR_CORE_INT_H__
#define __NET_MGR_CORE_INT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_core.h"

#include "net_mgr_dhcp.h"

/** 默认路由 metric 基址：实际 metric = 基址 + 链路优先级（prio 小者路由更优） */
#define NETMGR_METRIC_BASE  0

/** 仲裁定时驱动默认间隔（毫秒）：配置未指定（tickMs<=0）时使用 */
#define NETMGR_TICK_MS  500

/** 单一链路的运行时上下文 */
typedef struct {
    NETMGR_TYPE type;               /**< 链路类型 */
    NETMGR_STATE state;             /**< 当前状态（仅 DOWN/READY，ACTIVE 由仲裁结果得出） */
    NETMGR_STATE prev;              /**< 上一轮刷新得到的状态，用于判定状态翻转 */
    unsigned char prio;             /**< 当前优先级 */
    int enable;                     /**< 1=该链路已启用 */
    int route_on;                   /**< 1=本模块已为该链路下发默认路由 */
    int rmetric;                    /**< 已下发路由使用的 metric，-1 表示未下发 */
    char rgate[NETMGR_IP_LEN];      /**< 已下发路由使用的网关 */
    char ifname[NETMGR_IF_LEN];     /**< 接口名 */
    char ip[NETMGR_IP_LEN];         /**< 当前 IP */
    char mask[NETMGR_IP_LEN];       /**< 当前掩码 */
    char gate[NETMGR_IP_LEN];       /**< 当前网关 */
    int err;                        /**< 最近一次异常的错误码（0 表示无异常），负值为 NETMGRERR_*；
                                         DHCP 链路 DOWN 时：曾取到租约后中断为 -NETMGRERR_DHCP，取址失败为具体错误码 */
    char dns[NETMGR_IP_LEN];        /**< DNS 服务器 */
    NETMGR_WIFI_INFO wifi;          /**< WiFi 连接信息，仅 WiFi 链路刷新时填写，其余链路恒为空 */
    void* priv;                     /**< 各链路私有数据，由各实现自行分配释放 */
} NETMGR_CTX;

/** 链路操作表，每类链路提供一份实现 */
typedef struct {
    /**
     * @brief 拉起链路：配置地址、启动外部进程、开始取址
     * @param ctx  链路上下文
     * @param conf 链路配置，由调用方保证类型与链路匹配
     * @return 0 成功；负值为 NETMGRERR_* 或底层错误码
     */
    int (*open)(NETMGR_CTX* ctx, const void* conf);

    /**
     * @brief 关闭链路并释放其占用的资源
     * @param ctx 链路上下文
     * @return 0 成功；负值为 NETMGRERR_* 或底层错误码
     */
    int (*close)(NETMGR_CTX* ctx);

    /**
     * @brief 刷新链路实时状态：只把内核与进程的实际状态投影进上下文
     * @details 只写 ctx 的 ip / mask / gate / state / err，
     *          不得拉起或停止取址进程、不得改配置；取址会话的可靠性由 sys 层保证
     * @param ctx 链路上下文
     * @return 0 成功；负值为 NETMGRERR_* 或底层错误码
     */
    int (*refresh)(NETMGR_CTX* ctx);
} NETMGR_OPS;

/** 以太网链路操作表 */
extern const NETMGR_OPS core_eth_ops;
/** WiFi 链路操作表 */
extern const NETMGR_OPS core_wifi_ops;
/** 4G 蜂窝链路操作表 */
extern const NETMGR_OPS core_cell_ops;

/**
 * DHCP 回调公共头：三类链路私有数据的首成员（继承），
 * 供 eth/wifi/cell 共用同一取址状态回调，避免三份同构实现
 */
typedef struct {
    NETMGR_TYPE type;                     /**< 所属链路类型，回调上报事件用 */
    int reported;                         /**< 1=已上报过至少一次取址状态，用于事件去重 */
    volatile NETMGR_DHCP_STATE dhcpState; /**< 最近一次 DHCP 取址状态 */
    volatile int dhcpErr;                 /**< 最近一次 DHCP 取址错误码，0 表示无异常 */
} NETMGR_DHCP_BASE;

/* ===== 状态判定 ===== */

/**
 * @brief 按「物理连通 + 已取地址 + 有默认网关」判定 DHCP 链路是否就绪并刷新快照
 * @details 网关从路由表回查（由 DHCP 客户端写入），未就绪时状态置为 DOWN
 * @param ctx 链路上下文，不可为空
 * @return 0 成功（未就绪时状态置为 DOWN）；负值为 NETMGRERR_* 错误码
 */
int core_link_ready(NETMGR_CTX* ctx);

/**
 * @brief 按「物理连通 + 已取地址 + 配置网关非空」判定静态地址链路是否就绪
 * @details 静态链路的网关由配置给出，无需回查路由表；未就绪时状态置为 DOWN
 * @param ctx  链路上下文，不可为空
 * @param gate 配置网关，不可为空（空串表示该链路不参与默认路由，判为 DOWN）
 * @return 0 成功（未就绪时状态置为 DOWN）；负值为 NETMGRERR_* 错误码
 */
int core_link_ready_static(NETMGR_CTX* ctx, const char* gate);

/**
 * @brief DHCP 取址链路的就绪判定：接口消失判定 + 通用就绪判定 + DOWN 原因归因
 * @details eth / cell 的刷新逻辑完全一致，抽到本层避免两处维护同一套归因规则：
 *          1) 接口不存在 → DOWN + -NETMGRERR_NOTFOUND，跳过后续查询；
 *          2) 通用判定（连通 + 地址 + 路由网关）；
 *          3) DOWN 原因：会话 FAIL 取具体错误码；曾取到租约后中断置 -NETMGRERR_DHCP
 * @param ctx  链路上下文，不可为空
 * @param base 链路私有数据的公共头（含 DHCP 会话状态），不可为空
 * @return 0 成功（未就绪时状态置为 DOWN）；负值为 NETMGRERR_* 错误码
 */
int core_link_ready_dhcp(NETMGR_CTX* ctx, NETMGR_DHCP_BASE* base);

/* ===== DHCP 状态上报（链路实现注册的回调，由仲裁层转成业务事件） ===== */

/**
 * @brief 三类链路共用的 DHCP 取址状态回调，在 DHCP 取址线程上下文执行
 * @details user 须指向以 NETMGR_DHCP_BASE 为首成员的链路私有数据；
 *          只记录状态与错误码并上报仲裁层，地址仍由刷新流程从接口读取；
 *          无返回值——重试与续租均归 sys 层取址会话负责
 * @param state  取址状态
 * @param ifname 接口名
 * @param err    状态为 FAIL 时的错误码
 * @param user   注册时传入的链路私有数据（首成员须为 NETMGR_DHCP_BASE）
 */
void core_dhcp_cb(NETMGR_DHCP_STATE state, const char* ifname, int err, void* user);

/* WiFi 连接信息由链路刷新写入上下文，仲裁层按轮次比对后生成连接/断开事件 */

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_CORE_INT_H__ */
