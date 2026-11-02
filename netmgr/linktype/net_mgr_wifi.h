/**
 * @file net_mgr_wifi.h
 * @brief netmgr 链路实现层：WiFi 链路（linktype 层，可独立使用）
 * @details STA 模式直连 wpa_supplicant 的 unix 控制接口（CTRL-REQ/CTRL-RSP 最小协议），
 *          取址支持 DHCP 与静态地址两种方式（由 NETMGR_WIFI_CONF.use_dhcp 选择）；
 *          AP 模式通过 hostapd + udhcpd 提供接入与 DNS，本机地址由 ap_ip/ap_mask 静态配置。
 */
#ifndef __NET_MGR_WIFI_H__
#define __NET_MGR_WIFI_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_ctx.h"

/** WiFi 链路操作表：open/close/refresh 由本层实现 */
extern const NETMGR_OPS netmgr_wifi_ops;

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_WIFI_H__ */
