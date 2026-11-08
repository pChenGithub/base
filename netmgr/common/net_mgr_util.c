/**
 * @file net_mgr_util.c
 * @brief netmgr 通用工具实现：IPv4 地址文本校验
 * @details 仅依赖系统头，可脱离 netmgr 其它文件单独编译使用。
 */
#include "net_mgr_util.h"


#include <stdio.h>
#include <arpa/inet.h>

/**
 * @brief 校验 IPv4 点分十进制字符串合法性
 * @param ip 待校验字符串，不可为空
 * @return 1 合法；0 非法
 */
int netmgr_ip_valid(const char* ip)
{
    struct in_addr addr = {0}; /* 转换结果容器 */

    if (NULL == ip || '\0' == ip[0])
    {
        return 0;
    }
    return (1 == inet_pton(AF_INET, ip, &addr)) ? 1 : 0;
}
