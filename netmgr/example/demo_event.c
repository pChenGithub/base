/**
 * @file demo_event.c
 * @brief netmgr sys 层事件模块（net_mgr_event）独立使用示例
 * @details 演示下层可脱离 netmgr 主流程单独编入宿主工程：
 *          1) 订阅链路、IPv4 地址、路由三类 netlink 事件；
 *          2) 回调只做入口校验与转调，格式化按类别下沉到各打印函数（单一职责）；
 *          3) 每条事件先打印一行 [raw]（逐字段解析结果），再打印一行友好信息，便于对照；
 *          4) 回车退出（真实业务按自身主循环/事件驱动退出，不用 sleep 轮候）。
 *          编译：make demo_event      运行：./demo_event
 */
#include <stdio.h>

#include "net_mgr_event.h"

/**
 * @brief 事件类别转可读名称
 * @param type 事件类别
 * @return 常量字符串，未知类别返回 UNKNOWN
 */
static const char* demo_type_name(NETMGR_NEV_TYPE type)
{
    switch (type)
    {
    case NETMGR_NEV_LINK_UP:
        return "LINK+";
    case NETMGR_NEV_LINK_DOWN:
        return "LINK-";
    case NETMGR_NEV_LINK_DEL:
        return "DEL";
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
 * @brief 打印事件原始字段（调试用）
 * @details 按类别只输出该类别实际携带的载荷，便于与 ip / 内核日志对照
 * @param ev 事件内容，不可为空
 */
static void demo_event_raw(const NETMGR_NEV* ev)
{
    /* 按类别取对应联合体载荷：不同类别的字段互斥，禁止跨类型读取 */
    switch (ev->type)
    {
    case NETMGR_NEV_LINK_UP:
    case NETMGR_NEV_LINK_DOWN:
    case NETMGR_NEV_LINK_DEL:
        printf("[raw]   type=%s(%d) ifname=%s\r\n",
               demo_type_name(ev->type), (int)ev->type, ev->ifname);
        break;
    case NETMGR_NEV_ADDR_ADD:
    case NETMGR_NEV_ADDR_DEL:
        printf("[raw]   type=%s(%d) ifname=%s ip=%s mask=%s\r\n",
               demo_type_name(ev->type), (int)ev->type, ev->ifname,
               ev->u.addr.ip, ev->u.addr.mask);
        break;
    case NETMGR_NEV_ROUTE_ADD:
    case NETMGR_NEV_ROUTE_DEL:
        printf("[raw]   type=%s(%d) ifname=%s metric=%d default=%d gate=%s\r\n",
               demo_type_name(ev->type), (int)ev->type, ev->ifname, ev->u.route.metric,
               ev->u.route.isDefault, ev->u.route.gate);
        break;
    default:
        printf("[raw]   type=%d ifname=%s\r\n", (int)ev->type, ev->ifname);
        break;
    }
}

/**
 * @brief 打印链路事件（LINK_UP / LINK_DOWN / LINK_DEL）
 * @details 方向已由事件的类别表达，此处不再重复解释标志位
 * @param ev 事件内容，不可为空
 */
static void demo_print_link(const NETMGR_NEV* ev)
{
    printf("[event] %-6s ifname=%s\r\n", demo_type_name(ev->type), ev->ifname);
}

/**
 * @brief 打印地址事件（含地址与掩码）
 * @param ev 事件内容，不可为空
 */
static void demo_print_addr(const NETMGR_NEV* ev)
{
    printf("[event] %-6s ifname=%-8s ip=%-15s mask=%s\r\n",
           demo_type_name(ev->type), ev->ifname, ev->u.addr.ip, ev->u.addr.mask);
}

/**
 * @brief 打印路由事件（含 metric、是否默认路由与下一跳）
 * @param ev 事件内容，不可为空
 */
static void demo_print_route(const NETMGR_NEV* ev)
{
    printf("[event] %-6s ifname=%-8s metric=%-3d 默认路由=%s gate=%s\r\n",
           demo_type_name(ev->type), ev->ifname, ev->u.route.metric,
           (0 != ev->u.route.isDefault) ? "是" : "否", ev->u.route.gate);
}

/**
 * @brief 按事件类别分发打印（格式化集中在此，回调只做转调）
 * @param ev 事件内容，不可为空
 */
static void demo_print(const NETMGR_NEV* ev)
{
    demo_event_raw(ev);

    switch (ev->type)
    {
    case NETMGR_NEV_LINK_UP:
    case NETMGR_NEV_LINK_DOWN:
    case NETMGR_NEV_LINK_DEL:
        demo_print_link(ev);
        break;
    case NETMGR_NEV_ADDR_ADD:
    case NETMGR_NEV_ADDR_DEL:
        demo_print_addr(ev);
        break;
    case NETMGR_NEV_ROUTE_ADD:
    case NETMGR_NEV_ROUTE_DEL:
        demo_print_route(ev);
        break;
    case NETMGR_NEV_LOST:
        printf("[event] LOST   内核事件缓冲溢出，已有事件丢失（建议做一次全量刷新）\r\n");
        break;
    default:
        printf("[event] 未识别事件 type=%d（示例未覆盖，请更新 demo_print）\r\n", (int)ev->type);
        break;
    }
}

/**
 * @brief 事件回调：只做入口校验与转调，不含格式化与业务处理
 * @param ev   事件内容，仅在回调期间有效
 * @param user 未使用
 */
static void demo_event_cb(const NETMGR_NEV* ev, void* user)
{
    (void)user;

    if (NULL == ev)
        return;

    demo_print(ev);
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
        printf("[event] 启动监听失败, action=open ret=%d\r\n", ret);
        return 1;
    }

    printf("[event] 已订阅链路/地址/路由事件。可用以下命令触发（拔插网线亦可）：\r\n");
    printf("  ip link set dev eth0 down / up             -> LINK- / LINK+\r\n");
    printf("  ip link set dev eth0 mtu 1500              -> LINK+（仍连通）\r\n");
    printf("  ip addr add 192.168.1.10/24 dev eth0       -> ADDR+\r\n");
    printf("  ip addr del 192.168.1.10/24 dev eth0       -> ADDR-\r\n");
    printf("  ip route add default via 192.168.1.1       -> ROUTE+\r\n");
    printf("  ip route del default via 192.168.1.1       -> ROUTE-\r\n");
    printf("[event] 监听中，按回车退出...\r\n");
    (void)getchar();

    ret = netmgr_event_close();
    if (ret < 0)
    {
        printf("[event] 停止监听失败, action=close ret=%d\r\n", ret);
        return 1;
    }
    return 0;
}
