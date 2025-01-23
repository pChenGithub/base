#ifndef __WIFI_OPT_H__
#define __WIFI_OPT_H__
#ifdef __cplusplus
extern "C" {
#endif
/** wifi station 模式 ******************/
// 打开wifi
int wifi_sta_enable();
int wifi_sta_disable();
// 扫描wifi
int wifi_sta_scan();
// 连接wifi
int wifi_sta_connect();

/** wifi access point 模式 *************/
int wifi_ap_enable();
int wifi_ap_disable();

#ifdef __cplusplus
}
#endif
#endif

