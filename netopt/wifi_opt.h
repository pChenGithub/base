#ifndef __WIFI_OPT_H__
#define __WIFI_OPT_H__
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    WIFI_OPEN = 0,
    WIFI_WPA_PSK,
    WIFI_WPA2_PSK,
} WIFI_SECURITY;
/** wifi station 模式 ******************/
// 打开wifi
int wifi_sta_enable();
int wifi_sta_disable();
// 扫描wifi
int wifi_sta_scan();
// 连接wifi
int wifi_sta_connect();

/** wifi access point 模式 *************/
typedef struct {
    char ssid[64];
    char passwd[16];
    WIFI_SECURITY security;
} AP_CONFIG; // ap配置参数
typedef struct {
    char startIP[16];
    char endIP[16];
    int max_leases;
    char opt_router[16];
} UDHCPD_CONFIG; // dhcpd 配置参数
typedef struct {
    AP_CONFIG ap;
    UDHCPD_CONFIG dhcpd;
} WIFI_AP_CONFIG;   // 启动配置参数
int wifi_ap_enable(WIFI_AP_CONFIG* conf);
int wifi_ap_reload(WIFI_AP_CONFIG* conf);
int wifi_ap_disable();

#ifdef __cplusplus
}
#endif
#endif

