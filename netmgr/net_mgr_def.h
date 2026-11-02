/**
 * @file net_mgr_def.h
 * @brief netmgr 模块内部契约：运行参数、链路上下文、链路操作表与内部接口
 * @details 仅供 netmgr 内部源文件包含，宿主工程不应直接包含本文件。
 */
#ifndef __NET_MGR_DEF_H__
#define __NET_MGR_DEF_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include "net_mgr.h"

/** 链路状态刷新周期（毫秒），netlink 事件到达时提前唤醒 */
#define NETMGR_TICK_MS      200
/** DNS 系统配置文件路径 */
#define NETMGR_DNS_FILE     "/etc/resolv.conf"
/** wpa_supplicant 控制接口目录 */
#define NETMGR_WPA_DIR      "/var/run/wpa_supplicant"
/** wpa_supplicant 启动配置文件路径 */
#define NETMGR_WPA_CONF     "/tmp/netmgr_wpa.conf"
/** hostapd 启动配置文件路径 */
#define NETMGR_AP_CONF      "/tmp/netmgr_ap.conf"
/** AP 模式 DHCP 服务配置文件路径 */
#define NETMGR_UDHCP_CONF   "/tmp/netmgr_dhcpd.conf"
/** DHCP 实例数量上限（等于链路类型数）；多处用作数组长度与循环边界，故具名 */
#define NETMGR_DHCP_MAX     4
/** 默认以太网接口名 */
#define NETMGR_DEV_ETH      "eth0"
/** 默认 WiFi 接口名 */
#define NETMGR_DEV_WIFI     "wlan0"
/** AP 模式默认本机地址 */
#define NETMGR_AP_IP        "192.168.4.1"
/** AP 模式默认掩码 */
#define NETMGR_AP_MASK      "255.255.255.0"
/** AP 模式默认分配起始地址 */
#define NETMGR_AP_START     "192.168.4.100"
/** AP 模式默认分配结束地址 */
#define NETMGR_AP_END       "192.168.4.200"
/** 路由 metric 基准值，叠加优先级后作为最终 metric */
#define NETMGR_METRIC_BASE  0

/**
 * @brief 内部错误日志宏，错误分支必须调用，避免静默失败
 * @param fmt 格式串
 */
#define NETMGR_ERR(fmt, ...) \
    fprintf(stderr, "[netmgr][%s:%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

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
    char gate[NETMGR_IP_LEN];       /**< 当前网关 */
    char dns[NETMGR_IP_LEN];        /**< DNS 服务器 */
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
     * @brief 刷新链路实时状态（IP、网关、就绪标志）
     * @param ctx 链路上下文
     * @return 0 成功；负值为 NETMGRERR_* 或底层错误码
     */
    int (*refresh)(NETMGR_CTX* ctx);
} NETMGR_OPS;

/** 默认路由条目 */
typedef struct {
    char ifname[NETMGR_IF_LEN]; /**< 出口接口名 */
    char gate[NETMGR_IP_LEN];   /**< 网关地址 */
    int metric;                 /**< 路由 metric */
} NETMGR_ROUTE;

/* ======================= 链路通用能力（net_mgr_link.c） ======================= */

/**
 * @brief 判断网络接口是否存在
 * @param ifname 接口名，不可为空
 * @return 1 存在；0 不存在
 */
int netmgr_link_exist(const char* ifname);

/**
 * @brief 判断接口物理链路是否连通
 * @param ifname 接口名，不可为空
 * @return 1 连通；0 未连通
 */
int netmgr_link_up(const char* ifname);

/**
 * @brief 使能或关闭接口
 * @param ifname 接口名，不可为空
 * @param on     1=启用（IFF_UP），0=关闭
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_enable(const char* ifname, int on);

/**
 * @brief 读取接口当前 IPv4 地址
 * @param ifname 接口名，不可为空
 * @param ip     输出缓冲，不可为空
 * @param len    输出缓冲长度
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_ip(const char* ifname, char* ip, int len);

/**
 * @brief 设置接口 IPv4 地址
 * @param ifname 接口名，不可为空
 * @param ip     点分十进制地址，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_setip(const char* ifname, const char* ip);

/**
 * @brief 设置接口 IPv4 掩码
 * @param ifname 接口名，不可为空
 * @param mask   点分十进制掩码，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_setmask(const char* ifname, const char* mask);

/**
 * @brief 校验 IPv4 点分十进制字符串合法性
 * @param ip 待校验字符串，不可为空
 * @return 1 合法；0 非法
 */
int netmgr_ip_valid(const char* ip);

/**
 * @brief 在指定接口中查找第一个存在的接口名
 * @param names 候选接口名数组
 * @param count 候选数量
 * @param out   输出接口名缓冲，不可为空
 * @param len   输出缓冲长度
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
int netmgr_dev_find(const char* const* names, int count, char* out, int len);

/**
 * @brief 按「物理连通 + 已取地址 + 有默认网关」判定链路是否就绪并刷新快照
 * @details DHCP 取址链路通用：网关从路由表回查，适用于以太网、WiFi STA、4G ECM
 * @param ctx 链路上下文，不可为空
 * @return 0 成功（未就绪时状态置为 DOWN）；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_ready(NETMGR_CTX* ctx);

/* ======================= 路由管理（net_mgr_route.c） ======================= */

/**
 * @brief 下发默认路由（带 metric）
 * @param ifname 出口接口名，不可为空
 * @param gate   网关地址，不可为空
 * @param metric 路由 metric，越小越优先
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_add(const char* ifname, const char* gate, int metric);

/**
 * @brief 删除默认路由（精确匹配接口、网关与 metric）
 * @param ifname 出口接口名，不可为空
 * @param gate   网关地址，不可为空
 * @param metric 路由 metric
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_del(const char* ifname, const char* gate, int metric);

/**
 * @brief 查询当前生效（metric 最小）的默认路由
 * @param out 输出路由信息，不可为空
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_act(NETMGR_ROUTE* out);

/**
 * @brief 查询指定接口上的默认路由（取 metric 最小的一条）
 * @param ifname 出口接口名，不可为空
 * @param out    输出路由信息，不可为空
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
int netmgr_route_dev(const char* ifname, NETMGR_ROUTE* out);

/* ======================= DHCP（net_mgr_dhcp.c） ======================= */

/**
 * @brief 在指定接口上启动 DHCP 取址
 * @param ifname  接口名，不可为空
 * @param timeout 单次请求超时秒数，0 表示使用 udhcpc 默认值
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_start(const char* ifname, int timeout);

/**
 * @brief 停止指定接口上的 DHCP 取址
 * @param ifname 接口名，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_dhcp_stop(const char* ifname);

/**
 * @brief 查询指定接口的 DHCP 是否仍在运行
 * @param ifname 接口名，不可为空
 * @return 1 运行中；0 已结束或未启动
 */
int netmgr_dhcp_busy(const char* ifname);

/* ======================= 进程控制（net_mgr_proc.c） ======================= */

/**
 * @brief 探测指定名称的进程是否存在
 * @param name 进程名（不含路径），不可为空
 * @return 1 存在；0 不存在
 */
int netmgr_proc_exist(const char* name);

/**
 * @brief 结束指定名称的全部进程
 * @param name 进程名，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_proc_kill(const char* name);

/**
 * @brief 以后台方式执行 shell 命令
 * @param fmt 命令格式串，不可为空
 * @return 0 命令返回成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_proc_exec(const char* fmt, ...);

/* ======================= DNS 下发（net_mgr_dns.c） ======================= */

/**
 * @brief 将 DNS 写入系统 DNS 配置文件
 * @param dns IPv4 地址；为空或空串表示清空 DNS 配置
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_dns_apply(const char* dns);

/* ======================= 链路实现（net_mgr_eth/wifi/cell.c） ======================= */

/** 以太网链路操作表 */
extern const NETMGR_OPS netmgr_eth_ops;

/** WiFi 链路操作表 */
extern const NETMGR_OPS netmgr_wifi_ops;

/** 4G 链路操作表 */
extern const NETMGR_OPS netmgr_cell_ops;

/* ======================= 事件监听（net_mgr_event.c） ======================= */

/**
 * @brief 启动 netlink 事件监听（链路、地址、路由变化）
 * @param wake 事件到达时的唤醒回调，可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_event_open(void (*wake)(void));

/**
 * @brief 停止 netlink 事件监听并回收线程
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_event_close(void);

/* ======================= 仲裁核心（net_mgr.c） ======================= */

/**
 * @brief 链路上下文快照转换为对外结构
 * @param ctx  链路上下文，不可为空
 * @param link 输出快照，不可为空
 */
void netmgr_core_conv(const NETMGR_CTX* ctx, NETMGR_LINK* link);

#ifdef __cplusplus
}
#endif
#endif
