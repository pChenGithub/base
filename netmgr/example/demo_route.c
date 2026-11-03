/**
 * @file demo_route.c
 * @brief netmgr sys 层路由模块（net_mgr_route）独立使用示例
 * @details 演示只读查询（下发/删除会改动系统路由表，示例不自动执行）：
 *          1) 查询当前生效（metric 最小）的默认路由；
 *          2) 查询指定接口上的默认路由。
 *          编译：make demo_route      运行：./demo_route
 */
#include <stdio.h>

#include "net_mgr_route.h"

/** 示例查询的接口名 */
#define DEMO_IFNAME "eth0"

/**
 * @brief 打印一条默认路由
 * @param tag 输出前缀
 * @param r   路由信息，不可为空
 */
static void demo_dump(const char* tag, const NETMGR_ROUTE* r)
{
    if (NULL == tag || NULL == r)
        return;

    printf("%s ifname=%s gate=%s metric=%d\r\n", tag, r->ifname, r->gate, r->metric);
}

/**
 * @brief 示例入口：查询生效默认路由与指定接口的默认路由
 * @return 0 恒返回 0
 */
int main(void)
{
    NETMGR_ROUTE act = {0}; /* 当前生效的默认路由 */
    NETMGR_ROUTE dev = {0}; /* 指定接口上的默认路由 */
    int ret = 0;            /* 接口返回值 */

    ret = netmgr_route_act(&act);
    if (ret < 0)
        printf("查询生效默认路由失败, ret=%d\r\n", ret);
    else
        demo_dump("生效默认路由:", &act);

    ret = netmgr_route_dev(DEMO_IFNAME, &dev);
    if (ret < 0)
        printf("查询 %s 默认路由失败, ret=%d\r\n", DEMO_IFNAME, ret);
    else
        demo_dump("接口默认路由:", &dev);

    /* 下发/删除默认路由会改动系统路由表，示例不自动执行，用法如下：
       netmgr_route_add("eth0", "192.168.1.1", 100);
       netmgr_route_del("eth0", "192.168.1.1", 100); */
    return 0;
}
