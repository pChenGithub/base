/**
 * @file demo_event.c
 * @brief netmgr sys 层事件模块（net_mgr_event）独立使用示例
 * @details 演示下层可脱离 netmgr 主流程单独编入宿主工程：
 *          1) 订阅链路、IPv4 地址、路由三类 netlink 事件；
 *          2) 回调中按结构化事件内容打印类别、增删方向、接口名、地址与是否默认路由；
 *          3) 监听固定时长后停止。
 *          编译：make demo_event      运行：./demo_event
 */
#include <stdio.h>
#include <unistd.h>

#include "net_mgr_event.h"

/** 示例监听时长（秒），到点自动停止 */
#define DEMO_RUN_S 10

/**
 * @brief 事件类别转可读名称
 * @param type 事件类别
 * @return 常量字符串，未知类别返回 UNKNOWN
 */
static const char* demo_type_name(NETMGR_NEV_TYPE type)
{
    switch (type)
    {
    case NETMGR_NEV_LINK_ADD:
        return "LINK+";
    case NETMGR_NEV_LINK_DEL:
        return "LINK-";
    case NETMGR_NEV_ADDR_ADD:
        return "ADDR+";
    case NETMGR_NEV_ADDR_DEL:
        return "ADDR-";
    case NETMGR_NEV_ROUTE_ADD:
        return "ROUTE+";
    case NETMGR_NEV_ROUTE_DEL:
        return "ROUTE-";
    case NETMGR_NEV_LOST:
        return "LOST";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 事件回调：打印一条结构化事件
 * @param ev   事件内容，仅在回调期间有效
 * @param user 未使用
 * @return 恒返回 0，继续监听
 */
static int demo_event_cb(const NETMGR_NEV* ev, void* user)
{
    (void)user;

    if (NULL == ev)
        return 0;

    printf("[event] %-6s ifname=%-8s fam=%-2d flags=0x%04x ip=%-15s mask=%-15s gate=%-15s metric=%-3d default=%d\r\n",
           demo_type_name(ev->type), ev->ifname, ev->family, ev->flags,
           ev->ip, ev->mask, ev->gate, ev->metric, ev->isDefault);
    return 0;
}

/**
 * @brief 示例入口：订阅事件、监听若干秒后停止
 * @return 0 正常结束；1 启动或停止失败
 */
int main(void)
{
    int ret = 0; /* 接口返回值 */

    ret = netmgr_event_open(demo_event_cb, NULL, NETMGR_EVENT_GROUPS_DEFAULT);
    if (ret < 0)
    {
        printf("启动事件监听失败, ret=%d\r\n", ret);
        return 1;
    }

    printf("已订阅链路/地址/路由事件, 监听 %d 秒, 期间插拔网线或改地址可看到输出...\r\n",
           DEMO_RUN_S);
    sleep(DEMO_RUN_S);

    ret = netmgr_event_close();
    if (ret < 0)
    {
        printf("停止事件监听失败, ret=%d\r\n", ret);
        return 1;
    }
    return 0;
}
