#ifndef __NET_DNS_H__
#define __NET_DNS_H__
#include "net_def.h"
typedef struct {
    char ip[IP_STR_MAXLEN];
} DNS_INFO;
int set_resolv_dns(const char* dns);
int get_resolv_dns(DNS_INFO* dns, int* len);
#endif



