/**
 * @file net_mgr_wpa.h
 * @brief netmgr 系统适配层：wpa_supplicant 控制接口（sys 层，可独立使用）
 * @details 直连 wpa_supplicant 的 unix 控制接口（CTRL-REQ/CTRL-RSP 最小协议），
 *          提供「启动 / 停止 / 连接 / 断开 / 状态查询 / 扫描 / 扫描结果」能力与事件监听，
 *          只依赖 common 与系统 socket，可脱离 netmgr 上层单独使用。
 *          职责边界（硬性）：wpa_supplicant 自身进程的拉起与结束由本层承担
 *          （`netmgr_wpa_start` / `netmgr_wpa_stop`），进程控制能力来自 common 层的
 *          `net_mgr_proc`，sys 层模块之间仍保持零横向依赖；
 *          本层不处理 DHCP 与路由，也不编排 hostapd / udhcpd 等其它进程
 *          （跨进程互斥，如 STA/AP 切换，由调用方决定）。
 *          网络配置由调用方每次 connect 下发、不持久化；disconnect 会清空 wpa 内网络配置，
 *          避免残留网络被自动重连。
 *
 * 使用约束（独立调用方须知）：
 * 1) 单实例：进程内维护一份控制通道，同一时刻只服务一个接口；
 * 2) 线程：命令接口全部同步返回；事件通道（netmgr_wpa_event_open）另起 1 个接收线程，
 *    回调在该线程上下文执行，回调内不可长时间阻塞；
 * 3) 权限：拉起/结束 wpa_supplicant 需 root 或对应用户权限，
 *    连接控制接口需与 wpa_supplicant 同用户或具备访问权限；
 * 4) 外部依赖：wpa_supplicant 可执行文件；由 `netmgr_wpa_start` 拉起并创建
 *    /var/run/wpa_supplicant/<ifname>，不依赖外部先行启动。
 */
#ifndef __NET_MGR_WPA_H__
#define __NET_MGR_WPA_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_limits.h"

/** SSID 最大长度（含结束符） */
#define NETMGR_SSID_LEN   64
/** BSSID 文本长度（xx:xx:xx:xx:xx:xx 含结束符） */
#define NETMGR_BSSID_LEN  18
/** 扫描结果缓存条数上限；超出部分丢弃，避免动态分配 */
#define NETMGR_SCAN_MAX   16

/** wpa_supplicant 工作状态（由 wpa_state 映射） */
typedef enum {
    NETMGR_WPA_UNKNOWN = 0, /**< 未知：未取到状态或状态文本无法识别 */
    NETMGR_WPA_SCANNING,    /**< 扫描中 */
    NETMGR_WPA_ASSOCIATING, /**< 关联或四次握手进行中 */
    NETMGR_WPA_COMPLETED,   /**< 已关联热点并完成四次握手 */
    NETMGR_WPA_DISCONNECTED /**< 已断开或未连接热点 */
} NETMGR_WPA_STATE;

/** WiFi 扫描到的接入点 */
typedef struct {
    char ssid[NETMGR_SSID_LEN];  /**< 热点名称，隐藏 SSID 时为空串 */
    char bssid[NETMGR_BSSID_LEN];/**< BSSID，形如 xx:xx:xx:xx:xx:xx */
    int freq;                    /**< 频率 MHz */
    int signal;                  /**< 信号强度 dBm（负值，越大越好） */
    int secure;                  /**< 1=加密，0=开放 */
} NETMGR_WIFI_AP;

/** 当前 WiFi 连接信息 */
typedef struct {
    int connected;                   /**< 1=已关联热点 */
    char ssid[NETMGR_SSID_LEN];      /**< 当前 SSID */
    char bssid[NETMGR_BSSID_LEN];    /**< 当前 BSSID */
    int freq;                        /**< 频率 MHz */
    int signal;                      /**< 信号强度 dBm */
    int linkSpeed;                   /**< 协商速率 Mbps，未知为 0 */
} NETMGR_WIFI_INFO;

/** wpa 事件类别（CTRL-EVENT-* 映射；无法识别的事件不上报） */
typedef enum {
    NETMGR_WEV_DISCONNECTED = 0, /**< 已断开（含断开原因码） */
    NETMGR_WEV_CONNECTED,        /**< 关联完成（四次握手通过） */
    NETMGR_WEV_SCAN_DONE,        /**< 一轮扫描结束，结果可经 netmgr_wpa_scan_result 读取 */
    NETMGR_WEV_ASSOC_REJECT,     /**< 关联被拒（含状态码） */
    NETMGR_WEV_TERMINATING       /**< wpa_supplicant 进程退出，事件通道随之失效 */
} NETMGR_WEV_TYPE;

/** 断开载荷：NETMGR_WEV_DISCONNECTED */
typedef struct {
    int reason; /**< 断开原因码（CTRL-EVENT-DISCONNECTED 的 reason=） */
} NETMGR_WEVP_DISC;

/** 关联被拒载荷：NETMGR_WEV_ASSOC_REJECT */
typedef struct {
    int status; /**< 关联状态码（CTRL-EVENT-ASSOC-REJECT 的 status_code=） */
} NETMGR_WEVP_REJECT;

/**
 * 单条 wpa 事件内容：type / bssid 为公共字段，其余按类别取 u
 * @note 载荷映射（禁止跨类型读取 u）：
 *       CONNECTED              → 无载荷（BSSID 见公共字段 bssid，SSID 需经 netmgr_wpa_status 查询）
 *       DISCONNECTED           → u.disc（reason）
 *       ASSOC_REJECT           → u.reject（status）
 *       SCAN_DONE / TERMINATING → 无载荷（bssid 为空串）
 */
typedef struct {
    NETMGR_WEV_TYPE type;         /**< 事件类别（判别符） */
    char bssid[NETMGR_BSSID_LEN]; /**< 相关 BSSID，无则为空串 */
    union {
        NETMGR_WEVP_DISC   disc;   /**< DISCONNECTED 载荷 */
        NETMGR_WEVP_REJECT reject; /**< ASSOC_REJECT 载荷 */
    } u; /**< 按 type 取用的载荷；CONNECTED / SCAN_DONE / TERMINATING 无载荷 */
} NETMGR_WEV;

/**
 * @brief wpa 事件回调，在事件接收线程上下文执行
 * @param ev   事件内容，仅在回调期间有效，不可保存指针
 * @param user 注册回调时传入的用户指针，可为空
 * @return 0 继续监听；返回值当前被忽略，不影响接收线程
 */
typedef int (*netmgr_wev_cb)(const NETMGR_WEV* ev, void* user);

/* ===== 生命周期 ===== */

/**
 * @brief 拉起 wpa_supplicant 并打开控制通道
 * @details 步骤：结束残留实例（独占启动）→ 写最小配置 → 后台拉起 →
 *          等控制接口出现 → 打开通道；通道已打开时先停止再重建。
 *          只负责 wpa_supplicant 进程，不碰 hostapd / udhcpd 与地址配置
 * @param ifname    接口名，不可为空
 * @param confPath  配置文件输出路径，不可为空
 * @param country   国家码（如 "CN"），NULL 或空串表示不写 country 行
 * @param timeoutMs 等待控制接口的超时（毫秒），0 表示使用默认超时
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_start(const char* ifname, const char* confPath, const char* country, int timeoutMs);

/**
 * @brief 断开连接、关闭控制通道并结束 wpa_supplicant 进程
 * @details 未运行时同样返回 0；断开失败不阻断后续清理，错误码在末尾上抛
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_stop(void);

/* ===== 控制通道 ===== */

/**
 * @brief 写 wpa_supplicant 最小配置（控制接口目录、调用方指定国家码）
 * @param path    配置文件输出路径，不可为空
 * @param country 国家码（如 "CN"），NULL 或空串表示不写 country 行（由 wpa 默认 regulatory 决定）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_conf_write(const char* path, const char* country);

/**
 * @brief 等待 wpa_supplicant 控制接口出现
 * @param ifname 接口名，不可为空
 * @param timeoutMs 等待超时（毫秒），0 表示使用默认超时
 * @return 0 就绪；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_wait(const char* ifname, int timeoutMs);

/**
 * @brief 建立控制通道（已连接同一接口时直接返回成功）
 * @param ifname 接口名，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_open(const char* ifname);

/**
 * @brief 查询控制通道是否已打开
 * @return 1 已打开；0 未打开
 */
int netmgr_wpa_is_open(void);

/**
 * @brief 关闭控制通道（未打开时直接返回）
 */
void netmgr_wpa_close(void);

/* ===== 协议操作 ===== */

/**
 * @brief 连接指定热点（内部完成网络配置、使能、选中与关联完成等待）
 * @param ssid   热点名称，不可为空
 * @param passwd 密码，空串表示开放网络
 * @param timeoutMs 关联完成等待超时（毫秒，按挂钟计），0 表示使用默认超时
 * @return 0 已关联；负值为 NETMGRERR_* 错误码（通道未打开返回 -NETMGRERR_STATE）
 */
int netmgr_wpa_connect(const char* ssid, const char* passwd, int timeoutMs);

/**
 * @brief 断开当前热点并清空 wpa 内网络配置（重连需重新 connect）
 * @return 0 成功（wpa 未连接热点时同样返回 0）；负值为 NETMGRERR_* 错误码
 *         （通道未打开返回 -NETMGRERR_STATE）
 */
int netmgr_wpa_disconnect(void);

/**
 * @brief 读取当前工作状态
 * @param state 输出状态，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码（通道未打开返回 -NETMGRERR_STATE）
 */
int netmgr_wpa_state(NETMGR_WPA_STATE* state);

/**
 * @brief 读取当前连接信息（SSID、BSSID、频率、信号、速率）
 * @param info 输出连接信息，不可为空
 * @return 0 成功（未连接时 connected=0 其余为空）；负值为 NETMGRERR_* 错误码
 *         （通道未打开返回 -NETMGRERR_STATE）
 */
int netmgr_wpa_status(NETMGR_WIFI_INFO* info);

/**
 * @brief 触发一次扫描，并等待扫描结束
 * @param timeoutMs 扫描结束等待超时（毫秒，按挂钟计），0 表示使用默认超时
 * @return 0 扫描完成；负值为 NETMGRERR_* 错误码（超时返回 -NETMGRERR_TIMEOUT，
 *         此时扫描仍在后台进行、结果仍可读取）
 */
int netmgr_wpa_scan(int timeoutMs);

/**
 * @brief 读取最近一次扫描结果
 * @param list 输出数组，容量由 *num 传入
 * @param num  入参为数组容量，出参为实际写入条数
 * @return 0 成功；负值为 NETMGRERR_* 错误码（结果超出接收缓冲返回 -NETMGRERR_BUFF）
 */
int netmgr_wpa_scan_result(NETMGR_WIFI_AP* list, int* num);

/* ===== 事件通道 ===== */

/**
 * @brief 启动 wpa 事件监听（须先 netmgr_wpa_open 建立命令通道；重复调用直接返回成功）
 * @param cb   事件回调，可为空（只订阅不关心内容时）
 * @param user 回调透传的用户指针，可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码（通道未打开返回 -NETMGRERR_STATE）
 */
int netmgr_wpa_event_open(netmgr_wev_cb cb, void* user);

/**
 * @brief 停止 wpa 事件监听并回收接收线程（未启动时同样返回 0）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_event_close(void);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_WPA_H__ */
