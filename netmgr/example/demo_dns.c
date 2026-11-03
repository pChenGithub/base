/**
 * @file demo_dns.c
 * @brief netmgr sys 层 DNS 模块（net_mgr_dns）独立使用示例
 * @details 演示：
 *          1) 打印当前系统 DNS 配置文件内容（只读）；
 *          2) 可选执行 DNS 下发后再打印，用于观察原子替换结果。
 *          DNS 下发会改写系统 /etc/resolv.conf，默认关闭，需要时把
 *          DEMO_DNS_WRITE 置 1 再编译。
 *          编译：make demo_dns        运行：./demo_dns
 */
#include <stdio.h>

#include "net_mgr_dns.h"

/** 系统 DNS 配置文件路径 */
#define DEMO_DNS_CONF   "/etc/resolv.conf"
/** 单行读取缓冲长度 */
#define DEMO_LINE_LEN   128
/** 最多打印行数 */
#define DEMO_LINE_MAX   5
/** 1=执行 DNS 下发（改写系统配置），0=只读演示 */
#define DEMO_DNS_WRITE  0
/** 下发使用的 DNS 服务器 */
#define DEMO_DNS_SERVER "114.114.114.114"

/**
 * @brief 打印 DNS 配置文件的前若干行
 */
static void demo_dump_conf(void)
{
    FILE* fp = NULL;                  /* 配置文件句柄 */
    char line[DEMO_LINE_LEN] = {0};   /* 单行缓冲 */
    int count = 0;                    /* 已打印行数，从 0 开始 */

    fp = fopen(DEMO_DNS_CONF, "r");
    if (NULL == fp)
    {
        printf("打开 %s 失败, 可能不存在\r\n", DEMO_DNS_CONF);
        return;
    }

    printf("== %s ==\r\n", DEMO_DNS_CONF);
    while (count < DEMO_LINE_MAX && NULL != fgets(line, (int)sizeof(line), fp))
    {
        printf("%s", line);
        ++count;
    }
    fclose(fp);

    if (0 == count)
        printf("(空文件)\r\n");
}

#if (1 == DEMO_DNS_WRITE)
/**
 * @brief 执行一次 DNS 下发并打印结果
 */
static void demo_dns_write(void)
{
    int ret = 0; /* 接口返回值 */

    ret = netmgr_dns_apply(DEMO_DNS_SERVER);
    if (ret < 0)
        printf("下发 DNS %s 失败, ret=%d\r\n", DEMO_DNS_SERVER, ret);
    else
        printf("下发 DNS %s 成功\r\n", DEMO_DNS_SERVER);
}
#endif

/**
 * @brief 示例入口：打印 DNS 配置，可选下发后再打印
 * @return 0 恒返回 0
 */
int main(void)
{
    demo_dump_conf();

#if (1 == DEMO_DNS_WRITE)
    demo_dns_write();
    demo_dump_conf();
#else
    printf("(DEMO_DNS_WRITE=0, 未执行下发；置 1 重新编译可观察改写结果)\r\n");
#endif
    return 0;
}
