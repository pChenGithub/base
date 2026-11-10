/**
 * @file net_m_def.h
 * @brief net 网络管理模块对外类型与接口（net 层，宿主工程只包含本文件）
 * @details 本模块统一管理以太网、WiFi（STA 多热点择优 / AP）、4G 三类链路，
 *          并以优先级作为默认路由 metric 实现主备自动切换。设计要点：
 *          1) 单一所有者线程：全部状态只由管理线程写，业务线程经命令队列请求，
 *             因此模块内无「多驱动源 + 大锁」，读写路径不共享锁；
 *          2) 链路状态 NET_M_LINK.state 只表达客观状态，ACTIVE 由仲裁结果在快照中叠加；
 *          3) 错误不吞：内部故障记入 NET_M_LINK.fault 并上报 NET_M_EV_FAULT，
 *             业务语义异常记入 NET_M_LINK.err 并随事件上报。
 *          依赖方向：net -> sys -> common，不依赖其他模块。
 *          本层不打印任何日志，异常一律以负值返回（NETMGRERR_*，common 层统一定义，含 sys 层原样上抛的码）。
 */
#ifndef __NET_M_DEF_H__
#define __NET_M_DEF_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_err.h"
#include "net_mgr_wpa.h"

/* ===== 类型定义 ===== */

/** 链路类型，取值同时作为内部数组下标 */
typedef enum {
    NET_M_ETH = 0,  /**< 以太网 */
    NET_M_WIFI,     /**< WiFi（STA 或 AP） */
    NET_M_CELL,     /**< 4G 蜂窝 */
    NET_M_TYPE_MAX  /**< 链路类型数量（内部使用） */
} NET_M_TYPE;

/** 链路状态；ACTIVE 仅出现在快照中，表示当前承载默认流量的链路 */
typedef enum {
    NET_M_ST_DISABLED = 0, /**< 未启用：不参与刷新与仲裁 */
    NET_M_ST_DOWN,         /**< 不可用：已启用但未连通/未取得地址（WiFi STA 未关联热点亦属此态，
                                此时 carrier=0 且 err 给出最近一次关联失败原因） */
    NET_M_ST_READY,        /**< 就绪：已取得地址与网关，默认路由已下发 */
    NET_M_ST_ACTIVE        /**< 生效：就绪链路中优先级最高者（快照呈现态） */
} NET_M_STATE;

/** 内部故障阶段，用于定位 NET_M_EV_FAULT 的来源 */
typedef enum {
    NET_M_STAGE_OPEN = 0,  /**< 拉起链路 */
    NET_M_STAGE_REFRESH,   /**< 刷新链路状态 */
    NET_M_STAGE_ROUTE,     /**< 默认路由同步 */
    NET_M_STAGE_DNS,       /**< DNS 下发 */
    NET_M_STAGE_CLOSE      /**< 关闭链路 */
} NET_M_STAGE;

/** 链路事件类型 */
typedef enum {
    /* 链路聚合状态 */
    NET_M_EV_UP = 0,        /**< 链路上线（DOWN -> READY） */
    NET_M_EV_DOWN,          /**< 链路掉线（READY -> DOWN） */
    NET_M_EV_SWITCH,        /**< 主链路切换（ACTIVE 变化） */
    NET_M_EV_ENABLED,       /**< 链路被启用（启用即就绪时 state 直接为 READY/ACTIVE） */
    NET_M_EV_DISABLED,      /**< 链路被关闭（任意状态 -> DISABLED） */

    /* 网络信息与路由变化 */
    NET_M_EV_INFO_CHANGE,   /**< 地址信息变化，载荷含前后快照与变化位图 */
    NET_M_EV_ROUTE_CHANGE,  /**< 默认路由变化（本模块下发或外部改写） */

    /* 取址过程 */
    NET_M_EV_DHCP_RUNNING,  /**< 已发起取址 */
    NET_M_EV_DHCP_SUCC,     /**< 取址成功 */
    NET_M_EV_DHCP_FAIL,     /**< 取址失败（err 给出错误码） */
    NET_M_EV_DHCP_LOST,     /**< 已取得租约后丢失 */

    /* WiFi */
    NET_M_EV_WIFI_CONNECTED,    /**< STA 已关联热点 */
    NET_M_EV_WIFI_DISCONNECTED, /**< STA 关联断开 */
    NET_M_EV_WIFI_SCAN_DONE,    /**< 扫描完成 */

    /* 故障可观测性（本模块相对 netmgr 新增） */
    NET_M_EV_FAULT,     /**< 内部故障：载荷给出阶段与错误码，链路可能已降级 */
    NET_M_EV_DEGRADED,  /**< 链路仍就绪但子能力异常（DNS 写盘失败等） */

    /* 物理层连通性：与取址/地址信息变化解耦，单独上报（追加在末尾以保持既有取值不变） */
    NET_M_EV_CARRIER_UP,   /**< 载波接通：插入网线 / 接口管理态 UP 且 carrier 在位 */
    NET_M_EV_CARRIER_DOWN  /**< 载波断开：拔出网线 / 接口管理态 DOWN / 载波丢失 */
} NET_M_EV;

/** 链路快照字段变化位（INFO_CHANGE 载荷使用） */
#define NET_M_LINK_F_IP    0x01u
#define NET_M_LINK_F_MASK  0x02u
#define NET_M_LINK_F_GATE  0x04u

/** 扫描结果与连接信息沿用 sys 层类型，对外统一 NET_M_ 前缀 */
typedef NETMGR_WIFI_AP   NET_M_WIFI_AP;
typedef NETMGR_WIFI_INFO NET_M_WIFI_INFO;

/** 链路状态快照，由 netm_link_get / netm_link_list 填充 */
typedef struct {
    NET_M_TYPE type;       /**< 链路类型 */
    NET_M_STATE state;     /**< 当前状态 */
    unsigned char prio;    /**< 当前优先级（0-255，小者优先，等同路由 metric） */
    char ifname[16];       /**< 实际接口名，未启用时为空串 */
    char ip[16];           /**< 当前 IP，未取得时为空串 */
    char mask[16];         /**< 当前掩码，未取得时为空串 */
    char gate[16];         /**< 当前网关，未取得时为空串 */
    int err;               /**< 业务语义异常码（0 表示无异常），负值为 NETMGRERR_* */
    int fault;             /**< 内部故障码（0 表示无故障），由 NET_M_EV_FAULT 上报 */
    int faultStage;        /**< 故障阶段（NET_M_STAGE），fault 为 0 时无意义 */
    NET_M_WIFI_INFO wifi;  /**< WiFi 连接信息，仅 WiFi 链路有效，其余链路恒为空 */
    int carrier;           /**< 物理连通：1=管理态 UP 且有载波（网线在位/已关联），0=断开；
                               与 state 解耦：carrier=1 但未取到地址时 state 仍为 DOWN */
} NET_M_LINK;

/** 精简链路标识：事件中用于定位链路 */
typedef struct {
    NET_M_TYPE type;   /**< 链路类型 */
    char ifname[16];   /**< 接口名 */
} NET_M_LINKREF;

/** DHCP 事件载荷 */
typedef struct {
    NET_M_LINKREF ref; /**< 关联链路 */
    int err;           /**< FAIL 时的错误码，其余为 0 */
} NET_M_EVP_DHCP;

/** WiFi 事件载荷：连接与断开共用，用 info.connected 区分 */
typedef struct {
    NET_M_LINKREF ref;     /**< 关联链路 */
    NET_M_WIFI_INFO info;  /**< connected=1 已连接；0 已断开（ssid 为断开前热点） */
} NET_M_EVP_WIFI;

/** 信息变化载荷 */
typedef struct {
    NET_M_LINK cur;      /**< 变化后的链路快照 */
    NET_M_LINK prev;     /**< 变化前的链路快照 */
    unsigned int changed; /**< NET_M_LINK_F_IP / MASK / GATE 组合 */
} NET_M_EVP_INFO;

/** 主链路切换载荷 */
typedef struct {
    NET_M_TYPE from; /**< 切换前主链路，NET_M_TYPE_MAX 表示此前无主链路 */
    NET_M_TYPE to;   /**< 切换后主链路 */
} NET_M_EVP_SWITCH;

/** 默认路由变化载荷 */
typedef struct {
    NET_M_LINKREF ref; /**< 关联链路 */
    int isAdd;         /**< 1=新增，0=删除 */
    char gate[16];     /**< 该默认路由的下一跳 */
} NET_M_EVP_ROUTE;

/** 扫描完成载荷 */
typedef struct {
    int apNum;     /**< 本次扫描到的接入点数量 */
    int complete;  /**< 1=正常完成，0=超时或失败 */
} NET_M_EVP_SCAN;

/** 故障载荷 */
typedef struct {
    NET_M_TYPE type;  /**< 关联链路 */
    NET_M_STAGE stage;/**< 故障阶段 */
    int code;         /**< 错误码（负值） */
} NET_M_EVP_FAULT;

/**
 * 单次事件数据：外层只有判别符，其余按 ev 取 u 的对应成员
 * @note 载荷映射（禁止跨类型读取 u）：
 *       UP / DOWN / ENABLED / DISABLED → u.link（完整链路快照）
 *       CARRIER_UP / CARRIER_DOWN     → u.link（完整链路快照，carrier 字段为变化后值）
 *       DHCP_*                        → u.dhcp
 *       WIFI_CONNECTED / DISCONNECTED → u.wifi
 *       INFO_CHANGE                   → u.info
 *       SWITCH                        → u.sw
 *       ROUTE_CHANGE                  → u.route
 *       WIFI_SCAN_DONE                → u.scan
 *       FAULT / DEGRADED              → u.fault
 */
typedef struct {
    NET_M_EV ev; /**< 事件类型（判别符），唯一公共字段 */
    union {
        NET_M_LINK        link;  /**< UP / DOWN / ENABLED / DISABLED / CARRIER_*：完整快照 */
        NET_M_EVP_DHCP    dhcp;  /**< DHCP_RUNNING / SUCC / FAIL / LOST */
        NET_M_EVP_WIFI    wifi;  /**< WIFI_CONNECTED / WIFI_DISCONNECTED */
        NET_M_EVP_INFO    info;  /**< INFO_CHANGE */
        NET_M_EVP_SWITCH  sw;    /**< SWITCH */
        NET_M_EVP_ROUTE   route; /**< ROUTE_CHANGE */
        NET_M_EVP_SCAN    scan;  /**< WIFI_SCAN_DONE */
        NET_M_EVP_FAULT   fault; /**< FAULT / DEGRADED */
    } u; /**< 唯一可变载荷，按 ev 取成员 */
} NET_M_EVENT;

/**
 * @brief 事件回调：所有事件统一经此返回
 * @details 回调在管理线程上下文执行，不可长时间阻塞；回调内可安全调用本模块任意接口
 *          （模块会识别自调用并直接执行，不再排队）。结构体指针仅在回调期间有效，不可保存
 * @param ev   事件数据，不可为空
 * @param user 注册时传入的用户指针，可为空
 * @return 0 继续；返回值当前被忽略，不影响内部流程
 */
typedef int (*net_m_event_cb)(const NET_M_EVENT* ev, void* user);

/* ===== 链路配置 ===== */

/** 以太网链路配置 */
typedef struct {
    int enable;             /**< 1=启用该链路，0=不启用 */
    unsigned char prio;     /**< 优先级 0-255，小者优先，启用链路之间必须唯一 */
    int useDhcp;            /**< 1=DHCP 取址，0=静态配置 */
    char ifname[16];        /**< 接口名，空则使用 eth0 */
    char ip[16];            /**< 静态 IP（useDhcp=0 时必填） */
    char mask[16];          /**< 静态掩码（useDhcp=0 时必填） */
    char gate[16];          /**< 静态网关（useDhcp=0 时必填，决定默认路由） */
    char dns[16];           /**< DNS 服务器，空则不参与下发 */
} NET_M_ETH_CONF;

/** WiFi 工作模式 */
typedef enum {
    NET_M_MODE_STA = 0, /**< 站点模式：连接外部热点 */
    NET_M_MODE_AP       /**< 热点模式：自身发射热点并为客户端分配地址 */
} NET_M_WMODE;

/**
 * STA 模式候选热点（多 SSID 记忆的最小单元）
 * @note prio 为 0 表示「按扫描信号强度择优」，1~255 表示固定优先序（小者先试）
 */
typedef struct {
    char ssid[64];        /**< 热点名称，不可为空 */
    char passwd[64];      /**< 口令，空串表示开放网络 */
    unsigned char prio;   /**< 0=自动择优；1-255=固定顺序 */
} NET_M_WIFI_CAND;

/** WiFi 链路配置 */
typedef struct {
    int enable;                    /**< 1=启用该链路，0=不启用 */
    unsigned char prio;            /**< 优先级 0-255，小者优先，启用链路之间必须唯一 */
    NET_M_WMODE mode;              /**< STA 或 AP 模式 */
    char ifname[16];               /**< 接口名，空则使用 wlan0 */
    /* ---- STA 模式：候选热点列表 ---- */
    NET_M_WIFI_CAND cand[4];       /**< 候选热点，按 candNum 有效 */
    int candNum;                   /**< 有效候选数（0~4）；0 表示未配置热点，链路可启用但不连接 */
    int useDhcp;                   /**< STA：1=DHCP 取址，0=静态配置 */
    char ip[16];                   /**< STA 静态 IP（useDhcp=0 时必填） */
    char mask[16];                 /**< STA 静态掩码（useDhcp=0 时必填） */
    char gate[16];                 /**< STA 静态网关（useDhcp=0 时必填） */
    /* ---- AP 模式 ---- */
    char apSsid[64];               /**< AP 热点名称，AP 模式必填 */
    char apPasswd[64];             /**< AP 口令，空串表示开放热点 */
    char apIp[16];                 /**< AP 本机 IP，空则使用 192.168.4.1 */
    char apMask[16];               /**< AP 掩码，空则使用 255.255.255.0 */
    char apStart[16];              /**< AP 分配起始地址，空则使用 192.168.4.100 */
    char apEnd[16];                /**< AP 分配结束地址，空则使用 192.168.4.200 */
    /* ---- 公共 ---- */
    char dns[16];                  /**< DNS 服务器，空则不参与下发 */
} NET_M_WIFI_CONF;

/** 4G 接入方式；PPP / QMI 为预留，当前构建返回 -NETMGRERR_NOTSUP */
typedef enum {
    NET_M_CELL_ECM = 0, /**< ECM/RNDIS/NCM 网卡 + DHCP（已实现） */
    NET_M_CELL_PPP,     /**< PPP 拨号（预留） */
    NET_M_CELL_QMI      /**< QMI/MBIM 拨号（预留） */
} NET_M_CMODE;

/** 4G 链路配置 */
typedef struct {
    int enable;             /**< 1=启用该链路，0=不启用 */
    unsigned char prio;     /**< 优先级 0-255，启用链路之间必须唯一 */
    NET_M_CMODE mode;       /**< 接入方式，当前仅 ECM 可用 */
    char ifname[16];        /**< 接口名，空则自动探测 usb0/wwan0/eth1 */
    char apn[32];           /**< APN（PPP / QMI 预留字段） */
    char user[32];          /**< 拨号用户名（PPP / QMI 预留字段） */
    char passwd[64];        /**< 拨号密码（PPP / QMI 预留字段） */
    char dns[16];           /**< DNS 服务器，空则不参与下发 */
} NET_M_CELL_CONF;

/** 网络管理总配置 */
typedef struct {
    NET_M_ETH_CONF eth;   /**< 以太网配置 */
    NET_M_WIFI_CONF wifi; /**< WiFi 配置 */
    NET_M_CELL_CONF cell; /**< 4G 配置 */
    int tickMs;           /**< 仲裁轮询间隔（毫秒），<=0 时取默认 500；建议 500~1000 */
} NET_M_CONF;

/* ===== 生命周期 ===== */

/**
 * @brief 启动网络管理服务，按配置拉起全部启用链路
 * @details 启动后模块自建 1 个管理线程：周期仲裁、netlink 事件、DHCP 回调、业务配置
 *          全部作为消息进入同一线程串行处理，业务无需驱动任何刷新接口。
 *          配置内容在函数内复制，调用返回后外部可释放；配置非法（启用链路优先级冲突、
 *          必填项缺失）返回 -NETMGRERR_CONF 且不启动。
 * @param conf 总配置，不可为空
 * @param cb   事件回调，可为空（不需要通知时）
 * @param user 回调透传的用户指针，可为空
 * @return 0 成功；负值为 NETMGRERR_*
 */
int netm_start(const NET_M_CONF* conf, net_m_event_cb cb, void* user);

/**
 * @brief 停止网络管理服务：停止定时驱动、撤销默认路由、关闭全部链路、回收管理线程
 * @details 未启动时返回 -NETMGRERR_STATE。在事件回调内调用时直接执行清理，
 *          由管理线程在返回后自行退出，不阻塞
 * @return 0 成功；负值为 NETMGRERR_*
 */
int netm_stop(void);

/* ===== 查询 ===== */

/**
 * @brief 读取指定链路的状态快照
 * @details 只读快照副本，不排队、不阻塞，可在任意线程高频调用
 * @param type 链路类型
 * @param out  输出快照，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netm_link_get(NET_M_TYPE type, NET_M_LINK* out);

/**
 * @brief 查询全部链路的状态快照
 * @param list 输出数组，不可为空，容量由 *num 传入
 * @param num  不可为空，入参为数组容量，出参为实际写入条数
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netm_link_list(NET_M_LINK* list, int* num);

/**
 * @brief 读取当前承载默认流量的主链路
 * @param type 输出主链路类型，不可为空；无主链路时输出 NET_M_TYPE_MAX
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netm_active_get(NET_M_TYPE* type);

/**
 * @brief 读取指定链路的当前优先级
 * @param type 链路类型
 * @param prio 输出优先级，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netm_prio_get(NET_M_TYPE type, unsigned char* prio);

/* ===== 运行时配置 ===== */

/**
 * @brief 修改指定链路的优先级，并立即重跑一轮仲裁
 * @details 与当前其它启用链路优先级冲突时返回 -NETMGRERR_PRIO 且不修改；
 *          修改成功后主链路改选通过 SWITCH 事件体现
 * @param type 链路类型
 * @param prio 新优先级（0-255，小者优先）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netm_prio_set(NET_M_TYPE type, unsigned char prio);

/**
 * @brief 预检查优先级是否可用（不修改任何状态）
 * @param type 链路类型
 * @param prio 待检查的优先级
 * @return 0 可用；-NETMGRERR_PRIO 与其它启用链路冲突；其余负值为 NETMGRERR_* 错误码
 */
int netm_prio_check(NET_M_TYPE type, unsigned char prio);

/**
 * @brief 启用或关闭指定链路
 * @details 启用时按最近一次保存的配置重新拉起；关闭时撤掉本模块下发的默认路由、
 *          释放链路资源并把状态置为 DISABLED。重复传 on=1 的语义是「先关闭再按
 *          缓存配置重建」，不会叠开第二个链路实例
 * @param type 链路类型
 * @param on   1=启用，0=关闭（非 0 一律按 1 处理）
 * @return 0 成功；负值为 NETMGRERR_*
 */
int netm_link_enable(NET_M_TYPE type, int on);

/**
 * @brief 重新配置以太网链路（先关闭再按新配置拉起）
 * @param conf 以太网配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_*
 */
int netm_eth_set(const NET_M_ETH_CONF* conf);

/**
 * @brief 重新配置 WiFi 链路（STA 换热点 / AP 重载均走此接口）
 * @param conf WiFi 配置，不可为空；STA 模式 candNum 可为 0（未配置热点，仅启用接口）
 * @return 0 成功；负值为 NETMGRERR_*
 * @note 启用成功不代表已连上热点：关联与取址由内部自愈按退避完成，
 *       期间 NET_M_LINK.state 为 DOWN，关联失败原因见 err
 */
int netm_wifi_set(const NET_M_WIFI_CONF* conf);

/**
 * @brief 重新配置 4G 链路
 * @param conf 4G 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_*（PPP / QMI 返回 -NETMGRERR_NOTSUP）
 */
int netm_cell_set(const NET_M_CELL_CONF* conf);

/**
 * @brief 下发 DNS 服务器地址（写入系统 DNS 配置并同步内部记录）
 * @param dns 单个 IPv4 地址字符串；为空或空串表示清除已下发的 DNS
 * @return 0 成功；负值为 NETMGRERR_*
 */
int netm_dns_set(const char* dns);

/* ===== WiFi 扩展 ===== */

/**
 * @brief 触发一次 WiFi 扫描并等待扫描结束
 * @details 阻塞直到扫描完成或超时（默认 8 秒），结果写入调用方数组；
 *          无论成功与否都会上报 NET_M_EV_WIFI_SCAN_DONE；超过 *num 容量的条目被丢弃
 * @param list 输出数组，不可为空
 * @param num  不可为空，入参为数组容量，出参为实际写入条数（失败时为 0）
 * @return 0 扫描完成；-NETMGRERR_TIMEOUT 超时；其余负值为 NETMGRERR_* 错误码
 */
int netm_wifi_scan(NET_M_WIFI_AP* list, int* num);

/**
 * @brief 读取最近一次扫描结果（与最近一次 netm_wifi_scan 返回内容一致）
 * @param list 输出数组，不可为空，容量由 *num 传入
 * @param num  不可为空，入参为数组容量，出参为实际条数
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netm_wifi_scan_result(NET_M_WIFI_AP* list, int* num);

/**
 * @brief 读取 WiFi 当前连接信息
 * @param info 输出连接信息，不可为空；未连接时 connected=0
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netm_wifi_status(NET_M_WIFI_INFO* info);

#ifdef __cplusplus
}
#endif
#endif /* __NET_M_DEF_H__ */
