/**
 * @file net_mgr.h
 * @brief 网络管理服务（netmgr）对外接口
 * @details 统一管理以太网、WiFi（STA/AP）、4G 三类链路：
 *          1) 按配置拉起或关闭链路；
 *          2) 以优先级作为默认路由 metric 下发到内核，实现主备自动切换；
 *          3) 链路状态变化通过回调通知应用；
 *          4) 提供 DNS 下发。
 *          本模块不依赖 base 内其它自研模块，可独立编入宿主工程。
 */
#ifndef __NET_MGR_H__
#define __NET_MGR_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_err.h"

/** 接口名字符串长度上限（含结束符）；被多处结构体成员与长度校验共用，故具名 */
#define NETMGR_IF_LEN    16
/** IPv4 点分十进制字符串长度上限（含结束符）；被多处结构体成员与长度校验共用，故具名 */
#define NETMGR_IP_LEN    16

/** 链路类型 */
typedef enum {
    NETMGR_ETH = 0,     /**< 以太网 */
    NETMGR_WIFI,        /**< WiFi */
    NETMGR_CELL,        /**< 4G 蜂窝 */
    NETMGR_TYPE_MAX     /**< 链路类型数量（内部使用） */
} NETMGR_TYPE;

/** 链路状态 */
typedef enum {
    NETMGR_ST_DOWN = 0, /**< 不可用：链路未连通或未取得地址 */
    NETMGR_ST_READY,    /**< 就绪：已取得 IP 与网关，默认路由已下发 */
    NETMGR_ST_ACTIVE    /**< 生效：当前 metric 最小，实际承载默认流量 */
} NETMGR_STATE;

/** 链路事件类型 */
typedef enum {
    NETMGR_EV_UP = 0,   /**< 链路上线（DOWN -> READY） */
    NETMGR_EV_DOWN,     /**< 链路掉线（READY -> DOWN） */
    NETMGR_EV_SWITCH    /**< 主链路切换（ACTIVE 发生变化） */
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

/** 链路状态快照，由 netmgr_stat_get 填充 */
typedef struct {
    NETMGR_TYPE type;           /**< 链路类型 */
    NETMGR_STATE state;         /**< 当前状态 */
    unsigned char prio;         /**< 当前优先级（0-255，小者优先，等同路由 metric） */
    char ifname[NETMGR_IF_LEN]; /**< 实际接口名，未启用时为空串 */
    char ip[NETMGR_IP_LEN];     /**< 当前 IP，未取得时为空串 */
    char gate[NETMGR_IP_LEN];   /**< 当前网关，未取得时为空串 */
} NETMGR_LINK;

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

/**
 * @brief 链路状态事件回调
 * @param ev   事件类型
 * @param link 触发事件的链路快照，仅在回调期间有效，不可保存指针
 * @return 0 继续；返回值当前被忽略，不影响内部流程
 */
typedef int (*netmgr_ev_cb)(NETMGR_EV ev, const NETMGR_LINK* link);

/**
 * @brief 启动网络管理服务，按配置拉起全部启用的链路
 * @param conf 总配置，不可为空；配置内容在函数内复制，调用后外部可释放
 * @param cb   链路状态回调，可为空（不需要通知时）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_start(const NETMGR_CONF* conf, netmgr_ev_cb cb);

/**
 * @brief 停止网络管理服务，关闭全部链路并删除本模块下发的默认路由
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_stop(void);

/**
 * @brief 运行时修改指定类型链路的优先级
 * @param type 链路类型
 * @param prio 新优先级（0-255，小者优先）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_prio_set(NETMGR_TYPE type, unsigned char prio);

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

/**
 * @brief 重新配置以太网链路（会先关闭再按新配置拉起）
 * @param conf 以太网配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_eth_set(const NETMGR_ETH_CONF* conf);

/**
 * @brief 重新配置 WiFi 链路（STA 切换 SSID 或 AP 重载均走此接口）
 * @param conf WiFi 配置，不可为空
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

#ifdef __cplusplus
}
#endif
#endif
