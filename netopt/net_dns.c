#include <stdio.h>
#include <string.h>
#include "net_dns.h"
#include "net_errno.h"

#define RESOLV_FILE "./resolv.conf"
int set_resolv_dns(const char* dns) {
    int ret = 0;
    char buff[32] = {0};
    if (NULL==dns)
        return -NETERR_CHECK_PARAM;

    // 检查dns的合法性

    FILE* fp = fopen(RESOLV_FILE, "w");
    if (NULL==fp)
        return -NETERR_FOPEN_FAIL;
    
    snprintf(buff, 32, "nameserver %s\n", dns);
    if (fwrite(buff, 1, strlen(buff), fp)<=0) {
        ret = -NETERR_FWRITE_FALI;
        goto closefd_exit;
    }

closefd_exit:
    fclose(fp);
    fp = NULL;
    return ret;
}

int get_resolv_dns(DNS_INFO* dns, int* len) {
    int ret = 0;
    char buff[32] = {0};
    if (NULL==dns || NULL==len || len<=0)
        return -NETERR_CHECK_PARAM;

    FILE* fp = fopen(RESOLV_FILE, "r");
    if (NULL==fp)
        return -NETERR_FOPEN_FAIL;

    int i = 0;
    while (NULL!=fgets(buff, 32, fp) && i<*len) {
        memcpy(dns[i].ip, buff+11, strlen(buff+11)+1);
        i++;
    }
    *len = i;

closefd_exit:
    fclose(fp);
    fp = NULL;
    return ret;
}


