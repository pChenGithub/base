/**
 * @file net_mgr_dns.c
 * @brief DNS 下发：将 DNS 服务器写入系统 DNS 配置文件
 * @details 采用「写临时文件 + 原子改名」的方式替换配置，避免宿主程序读到半截内容。
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "net_mgr_def.h"

/**
 * @brief 将 DNS 写入系统 DNS 配置文件
 * @param dns IPv4 地址；为空或空串表示清空已下发的 DNS 配置
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_dns_apply(const char* dns)
{
    char tmp[128] = {0}; /* 临时文件路径 */
    FILE* fp = NULL;          /* 临时文件句柄 */
    int empty = 0;            /* 1=本次为清空 DNS */
    int err = 0;              /* 失败时的 errno 快照 */

    empty = (NULL == dns || '\0' == dns[0]) ? 1 : 0;
    if (!empty && !netmgr_ip_valid(dns))
    {
        NETMGR_ERR("DNS 地址非法, dns=%s", dns);
        return -NETMGRERR_PARAM;
    }

    if (snprintf(tmp, sizeof(tmp), "%s.netmgr", NETMGR_DNS_FILE) >= (int)sizeof(tmp))
    {
        NETMGR_ERR("临时文件路径超长");
        return -NETMGRERR_BUFF;
    }

    fp = fopen(tmp, "w");
    if (NULL == fp)
    {
        NETMGR_ERR("打开 %s 失败, errno=%d", tmp, errno);
        return -NETMGRERR_FILE;
    }

    /* 仅在有 DNS 时写入一条 nameserver，空串表示清空配置 */
    if (!empty && fprintf(fp, "nameserver %s\n", dns) < 0)
    {
        err = errno;
        fclose(fp);
        unlink(tmp);
        NETMGR_ERR("写入 %s 失败, errno=%d", tmp, err);
        return -NETMGRERR_FILE;
    }

    if (0 != fflush(fp))
    {
        err = errno;
        fclose(fp);
        unlink(tmp);
        NETMGR_ERR("刷新 %s 失败, errno=%d", tmp, err);
        return -NETMGRERR_FILE;
    }
    /* 落盘后再改名，避免掉电后配置为空文件 */
    if (0 != fsync(fileno(fp)))
        NETMGR_ERR("落盘 %s 失败, errno=%d", tmp, errno);
    fclose(fp);
    fp = NULL;

    if (0 != rename(tmp, NETMGR_DNS_FILE))
    {
        err = errno;
        unlink(tmp);
        NETMGR_ERR("替换 %s 失败, errno=%d", NETMGR_DNS_FILE, err);
        return -NETMGRERR_FILE;
    }
    return 0;
}
