/**
 * @file demo_iface.c
 * @brief netmgr sys 层接口模块（net_mgr_iface）独立使用示例
 * @details 本示例只做只读查询，不改动系统网络配置：
 *          1) 在候选接口列表中探测第一个存在的接口；
 *          2) 查询该接口是否存在、是否就绪（管理态 UP 且载波在位）；
 *          3) 读取该接口当前 IPv4 地址与掩码。
 *          编译：make demo_iface      运行：./demo_iface
 */
#include <stdio.h>
#include <string.h>

#include "net_mgr_iface.h"

/** 候选接口都探测不到时使用的接口名 */
#define DEMO_IFNAME     "eth0"
/** 地址/掩码输出缓冲长度 */
#define DEMO_BUF_LEN    32

/** 候选接口名，演示按顺序探测第一个存在的接口 */
static const char* const g_cands[] = {"eth0", "enp0s3", "wlan0", "usb0", "wwan0"};

/**
 * @brief 示例入口：探测并打印指定接口的只读信息
 * @return 0 恒返回 0
 */
int main(void)
{
    char target[DEMO_BUF_LEN] = {0}; /* 实际查询的接口名 */
    char ip[DEMO_BUF_LEN] = {0};     /* 接口当前 IPv4 地址 */
    char mask[DEMO_BUF_LEN] = {0};   /* 接口当前 IPv4 掩码 */
    int candNum = (int)(sizeof(g_cands) / sizeof(g_cands[0])); /* 候选数量 */
    int ret = 0;                                              /* 接口返回值 */

    /* 第 1 步：按候选列表探测，探测不到时退回默认接口名继续演示 */
    ret = netmgr_iface_find(g_cands, candNum, target, (int)sizeof(target));
    if (ret < 0)
    {
        printf("[iface] 候选接口均不存在, ret=%d（改为探测默认接口 %s）\r\n", ret, DEMO_IFNAME);
        (void)snprintf(target, sizeof(target), "%s", DEMO_IFNAME);
    }

    /* 第 2 步：只读查询接口状态与地址 */
    printf("[iface] 接口 %s 只读信息：\r\n", target);
    printf("[iface] 存在        : %s\r\n", netmgr_iface_exist(target) ? "是" : "否");
    printf("[iface] 接口就绪    : %s\r\n", netmgr_iface_ready(target) ? "是" : "否");

    ret = netmgr_iface_ip(target, ip, (int)sizeof(ip));
    if (ret < 0)
        printf("[iface] 地址        : 读取失败, ifname=%s ret=%d\r\n", target, ret);
    else
        printf("[iface] 地址        : %s\r\n", ('\0' == ip[0]) ? "(未配置)" : ip);

    ret = netmgr_iface_mask(target, mask, (int)sizeof(mask));
    if (ret < 0)
        printf("[iface] 掩码        : 读取失败, ifname=%s ret=%d\r\n", target, ret);
    else
        printf("[iface] 掩码        : %s\r\n", ('\0' == mask[0]) ? "(未配置)" : mask);

    /* 写操作会改动系统网络配置，示例不自动执行，用法如下：
       netmgr_iface_enable("eth0", 1);
       netmgr_iface_setip("eth0", "192.168.1.20");
       netmgr_iface_setmask("eth0", "255.255.255.0"); */
    return 0;
}
