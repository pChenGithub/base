/**
 * @file net_mgr_ap.h
 * @brief netmgr 系统适配层：WiFi AP 模式（hostapd + udhcpd）控制（sys 层，可独立使用）
 * @details 本层只负责 AP 侧的两件事：生成 hostapd / udhcpd 配置、拉起与结束这两个
 *          常驻进程；接口地址（本机 IP / 掩码 / 接口启用）由调用方经 iface 能力配置，
 *          以保持 sys 层各模块零横向依赖。
 *
 * 使用约束（独立调用方须知）：
 * 1) 单实例：进程内不保存状态，重复 start 会先停再起（幂等重建）；
 * 2) 互斥假设：同一射频的 STA（wpa_supplicant）与 AP 不并发，调用方须在
 *    netmgr_ap_start 之前停止 STA 侧进程；本层不做互斥校验，也不停 wpa_supplicant；
 * 3) 线程：本层不起线程，start/stop 内部只做配置写盘与进程起停，立即返回；
 * 4) 权限：写 /tmp 与启停 hostapd/udhcpd 需 root；
 * 5) 外部依赖：hostapd、udhcpd 可执行文件。
 */
#ifndef __NET_MGR_AP_H__
#define __NET_MGR_AP_H__

#ifdef __cplusplus
extern "C" {
#endif

/** AP 模式配置：本层只关心 AP 侧所需字段 */
typedef struct {
    char ifname[16];              /**< 承载 AP 的接口名，不可为空 */
    char ssid[64];                /**< 热点 SSID，不可为空 */
    char passwd[64];              /**< 口令，空串表示开放 AP */
    char ap_ip[16];               /**< AP 本机地址（hostapd/udhcpd 的 router 选项） */
    char ap_mask[16];             /**< AP 子网掩码 */
    char ap_start[16];            /**< DHCP 地址池起始地址 */
    char ap_end[16];              /**< DHCP 地址池结束地址 */
    char dns[16];                 /**< 下发给客户端的 DNS，空则不写该选项 */
} NETMGR_APCONF;

/* ===== 操作 ===== */

/**
 * @brief 启动 AP：写 hostapd/udhcpd 配置并拉起两个常驻进程
 * @details 先停旧进程再按新配置拉起（幂等重建）；接口启用与地址配置由调用方
 *          事先完成，本层不碰 iface
 * @param conf AP 配置，不可为空；ifname/ssid/ap_ip/ap_mask/ap_start/ap_end 必填
 * @return 0 成功；负值为 NETMGRERR_* 错误码（配置非法 -NETMGRERR_CONF，
 *         写盘失败 -NETMGRERR_FILE，进程拉起失败 -NETMGRERR_PROC）
 */
int netmgr_ap_start(const NETMGR_APCONF* conf);

/**
 * @brief 停止 AP：结束 udhcpd 与 hostapd（幂等，未运行时同样返回 0）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_ap_stop(void);

/**
 * @brief 查询 AP 是否在服务
 * @details 以 hostapd 进程存活为判据（信标由它发送，udhcpd 仅负责分配地址）
 * @return 1 在服务；0 未运行
 */
int netmgr_ap_running(void);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_AP_H__ */
