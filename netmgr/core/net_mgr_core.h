/**
 * @file net_mgr_core.h
 * @brief netmgr 仲裁层接口与数据类型（core 层）
 * @details 链路装配、优先级仲裁、默认路由同步与事件分发的全部实现均在 core 层；
 *          api 层只做入参校验与转调。本层直接依赖 common + sys，不依赖 api 层；
 *          以太网 / WiFi / 4G 三条链路的实现与就绪判定属本层内部（不对外暴露）。
 *          链路类型、状态、事件、快照与配置均由本层产出或拥有，故类型定义在本文件，
 *          api 层通过包含本文件向上转出，形成 api → core 的单向依赖。
 *          本层不创建线程：
 *          1) netlink 事件由 sys 层事件回调直接驱动一轮仲裁；
 *          2) 其它不产生事件的状态变化由业务周期调用 netmgr_core_poll 驱动。
 */
#ifndef __NET_MGR_CORE_H__
#define __NET_MGR_CORE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_limits.h"
#include "net_mgr_wpa.h"

/** 链路快照字段变化位：IPv4 地址变化（INFO_CHANGE 载荷使用） */
#define NETMGR_LINK_F_IP    0x01u
/** 链路快照字段变化位：掩码变化 */
#define NETMGR_LINK_F_MASK  0x02u
/** 链路快照字段变化位：网关变化 */
#define NETMGR_LINK_F_GATE  0x04u

/* ===== 类型定义 ===== */

/** 链路类型 */
typedef enum {
    NETMGR_ETH = 0,     /**< 以太网 */
    NETMGR_WIFI,        /**< WiFi */
    NETMGR_CELL,        /**< 4G 蜂窝 */
    NETMGR_TYPE_MAX     /**< 链路类型数量（内部使用） */
} NETMGR_TYPE;

/** 链路状态 */
typedef enum {
    NETMGR_ST_DISABLED = 0, /**< 未启用：用户显式关闭，不参与刷新与仲裁 */
    NETMGR_ST_DOWN,         /**< 不可用：已启用但未连通或未取得地址 */
    NETMGR_ST_READY,        /**< 就绪：已取得 IP 与网关，默认路由已下发 */
    NETMGR_ST_ACTIVE        /**< 生效：当前 metric 最小，实际承载默认流量 */
} NETMGR_STATE;

/** 链路事件类型 */
typedef enum {
    /* 链路聚合状态 */
    NETMGR_EV_UP = 0,        /**< 链路上线（DOWN -> READY） */
    NETMGR_EV_DOWN,          /**< 链路掉线（READY -> DOWN） */
    NETMGR_EV_SWITCH,        /**< 主链路切换（ACTIVE 发生变化） */
    NETMGR_EV_ENABLED,       /**< 链路被启用；启用即就绪时 link.state 直接为 READY/ACTIVE */
    NETMGR_EV_DISABLED,      /**< 链路被关闭（任意状态 -> DISABLED） */

    /* 网络信息与路由变化 */
    NETMGR_EV_INFO_CHANGE,   /**< 链路地址信息变化（ip/mask/gate），事件携带前后快照 */
    NETMGR_EV_ROUTE_CHANGE,  /**< 默认路由变化（本模块下发或外部改写） */

    /* 取址过程 */
    NETMGR_EV_DHCP_RUNNING,  /**< 已发起 DHCP 取址 */
    NETMGR_EV_DHCP_SUCC,     /**< DHCP 取址成功 */
    NETMGR_EV_DHCP_FAIL,     /**< DHCP 取址失败（链路快照的 err 给出错误码） */

    /* WiFi */
    NETMGR_EV_WIFI_CONNECTED,    /**< WiFi STA 已关联热点（wifi 字段有效） */
    NETMGR_EV_WIFI_DISCONNECTED, /**< WiFi STA 关联断开 */
    NETMGR_EV_WIFI_SCAN_DONE     /**< WiFi 扫描完成（结果经 netmgr_core_wifi_scan_result 获取） */
} NETMGR_EV;

/** WiFi 工作模式 */
typedef enum {
    NETMGR_MODE_STA = 0, /**< 站点模式：连接外部热点 */
    NETMGR_MODE_AP       /**< 热点模式：自身发射热点并为客户端分配地址 */
} NETMGR_WMODE;

/** 4G 接入方式 */
typedef enum {
    NETMGR_CELL_ECM = 0, /**< ECM/RNDIS/NCM 网卡 + DHCP，当前已实现 */
    NETMGR_CELL_PPP,     /**< PPP 拨号（预留，暂未实现） */
    NETMGR_CELL_QMI      /**< QMI/MBIM 拨号（预留，暂未实现） */
} NETMGR_CMODE;

/** 链路状态快照，由 netmgr_core_stat_get 填充 */
typedef struct {
    NETMGR_TYPE type;           /**< 链路类型 */
    NETMGR_STATE state;         /**< 当前状态 */
    unsigned char prio;         /**< 当前优先级（0-255，小者优先，等同路由 metric） */
    char ifname[NETMGR_IF_LEN]; /**< 实际接口名，未启用时为空串 */
    char ip[NETMGR_IP_LEN];     /**< 当前 IP，未取得时为空串 */
    char mask[NETMGR_IP_LEN];   /**< 当前掩码，未取得时为空串 */
    char gate[NETMGR_IP_LEN];   /**< 当前网关，未取得时为空串 */
    int err;                    /**< 最近一次异常的错误码（0 表示无异常），负值为 NETMGRERR_* */
} NETMGR_LINK;

/** 精简链路标识：事件中用于定位链路，不含地址快照 */
typedef struct {
    NETMGR_TYPE type;           /**< 链路类型 */
    char ifname[NETMGR_IF_LEN]; /**< 接口名 */
} NETMGR_EV_LINKREF;

/** DHCP 事件载荷：三类取址事件共用 */
typedef struct {
    NETMGR_EV_LINKREF ref; /**< 关联链路 */
    int err;               /**< FAIL 时的错误码，其余为 0 */
} NETMGR_EVP_DHCP;

/** WiFi 事件载荷：连接与断开共用，用 info.connected 区分 */
typedef struct {
    NETMGR_EV_LINKREF ref; /**< 关联链路 */
    NETMGR_WIFI_INFO info; /**< connected=1 已连接；0 已断开（ssid 为断开前热点） */
} NETMGR_EVP_WIFI;

/** 信息变化载荷：变化前后快照与变化字段位图 */
typedef struct {
    NETMGR_LINK cur;      /**< 变化后的链路快照 */
    NETMGR_LINK prev;     /**< 变化前的链路快照 */
    unsigned int changed; /**< NETMGR_LINK_F_IP / MASK / GATE 组合 */
} NETMGR_EVP_INFO;

/** 主链路切换载荷 */
typedef struct {
    NETMGR_TYPE from; /**< 切换前主链路，NETMGR_TYPE_MAX 表示此前无主链路 */
    NETMGR_TYPE to;   /**< 切换后主链路 */
} NETMGR_EVP_SWITCH;

/** 默认路由变化载荷 */
typedef struct {
    NETMGR_EV_LINKREF ref;    /**< 关联链路 */
    int isAdd;                /**< 1=新增，0=删除 */
    char gate[NETMGR_IP_LEN]; /**< 该默认路由的下一跳 */
} NETMGR_EVP_ROUTE;

/** 扫描完成载荷：扫描与链路状态无关，不带链路标识 */
typedef struct {
    int apNum;    /**< 本次扫描到的接入点数量 */
    int complete; /**< 1=正常完成，0=超时或失败（此时 apNum 可能为 0） */
} NETMGR_EVP_SCAN;

/**
 * 单次事件数据：外层只有判别符，其余按 ev 取 u 的对应成员
 * @note 载荷映射（禁止跨类型读取 u）：
 *       UP / DOWN / ENABLED / DISABLED → u.link（完整链路快照）
 *       DHCP_RUNNING / DHCP_SUCC / DHCP_FAIL → u.dhcp
 *       WIFI_CONNECTED / WIFI_DISCONNECTED   → u.wifi
 *       INFO_CHANGE → u.info
 *       SWITCH      → u.sw
 *       ROUTE_CHANGE→ u.route
 *       WIFI_SCAN_DONE → u.scan
 */
typedef struct {
    NETMGR_EV ev; /**< 事件类型（判别符），唯一公共字段 */
    union {
        NETMGR_LINK       link;  /**< UP / DOWN / ENABLED / DISABLED：完整快照 */
        NETMGR_EVP_DHCP   dhcp;  /**< DHCP_RUNNING / SUCC / FAIL */
        NETMGR_EVP_WIFI   wifi;  /**< WIFI_CONNECTED / WIFI_DISCONNECTED */
        NETMGR_EVP_INFO   info;  /**< INFO_CHANGE */
        NETMGR_EVP_SWITCH sw;    /**< SWITCH */
        NETMGR_EVP_ROUTE  route; /**< ROUTE_CHANGE */
        NETMGR_EVP_SCAN   scan;  /**< WIFI_SCAN_DONE */
    } u; /**< 唯一可变载荷，按 ev 取成员 */
} NETMGR_EVENT;

/**
 * @brief 事件回调：所有事件统一经此返回
 * @details 事件类型由 NETMGR_EVENT.ev 给出，载荷按类型取自 link / prevLink / wifi / apNum；
 *          回调可能在 sys 事件线程、DHCP 取址线程或业务调用 netmgr_core_poll 的线程上执行，
 *          不可长时间阻塞；结构体指针仅在回调期间有效，不可保存
 * @param ev   事件数据，不可为空
 * @param user 注册时传入的用户指针，可为空
 * @return 0 继续；返回值当前被忽略，不影响内部流程
 */
typedef int (*netmgr_event_cb)(const NETMGR_EVENT* ev, void* user);

/** 以太网链路配置 */
typedef struct {
    int enable;                   /**< 1=启用该链路，0=不启用 */
    unsigned char prio;           /**< 优先级 0-255，小者优先，建议 10 */
    int use_dhcp;                 /**< 1=DHCP 取址，0=静态配置 */
    char ifname[NETMGR_IF_LEN];   /**< 接口名，空则使用 eth0 */
    char ip[NETMGR_IP_LEN];       /**< 静态 IP（use_dhcp=0 时有效） */
    char mask[NETMGR_IP_LEN];     /**< 静态掩码（use_dhcp=0 时有效） */
    char gate[NETMGR_IP_LEN];     /**< 静态网关（use_dhcp=0 时有效） */
    char dns[NETMGR_IP_LEN];      /**< DNS 服务器，空则不参与下发 */
} NETMGR_ETH_CONF;

/** WiFi 链路配置 */
typedef struct {
    int enable;                     /**< 1=启用该链路，0=不启用 */
    unsigned char prio;             /**< 优先级 0-255，小者优先，建议 20 */
    NETMGR_WMODE mode;              /**< STA 或 AP 模式 */
    char ifname[NETMGR_IF_LEN];     /**< 接口名，空则使用 wlan0 */
    char ssid[64];     /**< STA：目标 SSID；AP：热点 SSID */
    char passwd[64];   /**< 密码，空表示开放网络 */
    int use_dhcp;                   /**< STA：1=DHCP 取址，0=静态配置；AP 模式不使用该字段 */
    char ip[NETMGR_IP_LEN];         /**< STA 静态 IP（use_dhcp=0 时必填） */
    char mask[NETMGR_IP_LEN];       /**< STA 静态掩码（use_dhcp=0 时必填） */
    char gate[NETMGR_IP_LEN];       /**< STA 静态网关（use_dhcp=0 时必填，决定默认路由） */
    char dns[NETMGR_IP_LEN];        /**< DNS 服务器，空则不参与下发 */
    char ap_ip[NETMGR_IP_LEN];      /**< AP 本机 IP，空则使用 192.168.4.1 */
    char ap_mask[NETMGR_IP_LEN];    /**< AP 掩码，空则使用 255.255.255.0 */
    char ap_start[NETMGR_IP_LEN];   /**< AP 分配起始地址，空则使用 192.168.4.100 */
    char ap_end[NETMGR_IP_LEN];     /**< AP 分配结束地址，空则使用 192.168.4.200 */
} NETMGR_WIFI_CONF;

/** 4G 链路配置 */
typedef struct {
    int enable;                   /**< 1=启用该链路，0=不启用 */
    unsigned char prio;           /**< 优先级 0-255，小者优先，建议 30 */
    NETMGR_CMODE mode;            /**< 接入方式，当前仅 ECM 已实现 */
    char ifname[NETMGR_IF_LEN];   /**< 接口名，空则自动探测 usb0/wwan0/eth1 */
    char apn[32];     /**< APN（PPP 模式使用） */
    char user[32];   /**< 拨号用户名（PPP 模式使用） */
    char passwd[64]; /**< 拨号密码（PPP 模式使用） */
    char dns[NETMGR_IP_LEN];      /**< DNS 服务器，空则不参与下发 */
} NETMGR_CELL_CONF;

/** 网络管理总配置 */
typedef struct {
    NETMGR_ETH_CONF eth;    /**< 以太网配置 */
    NETMGR_WIFI_CONF wifi;  /**< WiFi 配置 */
    NETMGR_CELL_CONF cell;  /**< 4G 配置 */
} NETMGR_CONF;

/* ===== 生命周期 ===== */

/**
 * @brief 启动仲裁核心，按配置拉起全部启用的链路
 * @details 全部事件统一经 cb 返回：链路状态、网络信息变化、默认路由变化、
 *          DHCP 过程与 WiFi 状态；回调在事件线程或业务调用线程
 *          （netmgr_core_poll）上下文执行，不可长时间阻塞
 * @param conf 总配置，不可为空；配置内容在函数内复制，调用后外部可释放
 * @param cb   事件回调，可为空（不需要通知时）
 * @param user 回调透传的用户指针，可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_start(const NETMGR_CONF* conf, netmgr_event_cb cb, void* user);

/**
 * @brief 停止仲裁核心，关闭全部链路并删除本模块下发的默认路由
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_stop(void);

/* ===== 驱动 ===== */

/**
 * @brief 驱动一轮仲裁刷新
 * @details 本层不创建线程：netlink 事件由 sys 层事件回调驱动，
 *          其余不产生事件的状态变化（AP 进程退出、DHCP 异步失败、
 *          静态地址首次就绪、优先级变更）需由业务周期性调用本接口
 * @return 0 成功；负值为本轮首个失败步骤的错误码；未启动返回 -NETMGRERR_STATE
 */
int netmgr_core_poll(void);

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
 * @details 显式配置立即生效：函数内跑一轮仲裁，主链路改选（SWITCH 事件）
 *          与默认路由 metric 更新随返回即可见
 * @param type 链路类型
 * @param prio 新优先级（0-255，小者优先）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_prio_set(NETMGR_TYPE type, unsigned char prio);

/**
 * @brief 启用或关闭指定链路
 * @details 启用时按最近一次保存的配置重新拉起（含接口、取址与守护进程）；
 *          关闭时撤掉本模块下发的默认路由并释放链路资源、清空地址快照，状态置为 DISABLED。
 *          开关属于显式操作，函数内立即跑一轮仲裁，事件随返回即可见。
 *          对已启用链路重复传 on=1 的语义是「先关闭再按缓存配置重建」：
 *          不会叠开第二个链路实例（open 前旧链路必先 close），但会产生一次断流重连，
 *          配置未变时无须反复调用
 * @param type 链路类型
 * @param on   1=启用，0=关闭
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_link_enable(NETMGR_TYPE type, int on);

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

/* ===== WiFi 扫描 ===== */

/**
 * @brief 触发一次 WiFi 扫描并等待扫描结束，结果直接写入调用方数组
 * @details 要求 WiFi 链路已启用（控制通道已建立）；无论成功与否都会上报
 *          NETMGR_EV_WIFI_SCAN_DONE，complete 标记本轮是否正常完成；
 *          结果与事件 apNum 来自同一次读取，保证一致；超过 *num 容量的条目被丢弃
 * @param list 输出数组，不可为空
 * @param num  不可为空，入参为数组容量，出参为实际写入条数（扫描失败时为 0）
 * @return 0 扫描完成；负值为 NETMGRERR_* 错误码（超时返回 -NETMGRERR_TIMEOUT）
 */
int netmgr_core_wifi_scan(NETMGR_WIFI_AP* list, int* num);

/**
 * @brief 读取最近一次扫描结果（与最近一次 netmgr_core_wifi_scan 返回的内容一致）
 * @param list 输出数组，容量由 *num 传入
 * @param num  入参为数组容量，出参为实际写入条数
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_core_wifi_scan_result(NETMGR_WIFI_AP* list, int* num);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_CORE_H__ */
