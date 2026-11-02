/**
 * @file net_mgr_dns.c
 * @brief netmgr 系统适配层：DNS 下发实现
 * @details 采用「写临时文件 + 原子改名」的方式替换配置，避免宿主程序读到半截内容。
 */
#include "net_mgr_dns.h"

#include "net_mgr_err.h"
#include "net_mgr_util.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/** DNS 系统配置文件路径 */
#define NETMGR_DNS_FILE     "/etc/resolv.conf"

/**
 * @brief 将 DNS 写入系统 DNS 配置文件
 * @param dns IPv4 地址；为空或空串表示清空已下发的 DNS 配置
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_dns_apply(const char* dns)
{
    char tmp[128] = {0}; /* 临时文件路径 */
    FILE* fp = NULL;     /* 临时文件句柄 */
    int empty = 0;       /* 1=本次为清空 DNS */

    empty = (NULL == dns || '\0' == dns[0]) ? 1 : 0;
    if (!empty && !netmgr_ip_valid(dns))
    {
        return -NETMGRERR_PARAM;
    }

    if (snprintf(tmp, sizeof(tmp), "%s.netmgr", NETMGR_DNS_FILE) >= (int)sizeof(tmp))
    {
        return -NETMGRERR_BUFF;
    }

    fp = fopen(tmp, "w");
    if (NULL == fp)
    {
        return -NETMGRERR_FILE;
    }

    /* 仅在有 DNS 时写入一条 nameserver，空串表示清空配置 */
    if (!empty && fprintf(fp, "nameserver %s\n", dns) < 0)
    {
        fclose(fp);
        unlink(tmp);
        return -NETMGRERR_FILE;
    }

    if (0 != fflush(fp))
    {
        fclose(fp);
        unlink(tmp);
        return -NETMGRERR_FILE;
    }
    /* 落盘后再改名，避免掉电后配置为空文件 */
    if (0 != fsync(fileno(fp)))
    {
        fclose(fp);
        unlink(tmp);
        return -NETMGRERR_FILE;
    }
    fclose(fp);
    fp = NULL;

    if (0 != rename(tmp, NETMGR_DNS_FILE))
    {
        unlink(tmp);
        return -NETMGRERR_FILE;
    }
    return 0;
}
