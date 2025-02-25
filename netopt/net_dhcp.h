#ifndef __NET_DHCP_H__
#define __NET_DHCP_H__
#ifdef __cplusplus
extern "C" {
#endif
#define NET_DHCP_AGAIN  37  // 重复dhcp
typedef enum {
    DHCP_RUNNING = 0,
    DHCP_FAIL_END,
    DHCP_SUCC_END,
} DHCP_STATUS;

typedef int (*hand_dhcpstatus)(DHCP_STATUS sta, const char* ifname);
int start_dhcp(const char* ifname, int timeouts, hand_dhcpstatus hand);
int stop_dhcp(const char* ifname);
#ifdef __cplusplus
}
#endif
#endif


