/**
 * @file net_demo.c
 * @brief netmgr 示例程序：演示以太网、WiFi（STA/AP）、4G 的配置与优先级主备
 *
 * 用法：
 *   net_demo eth                         以太网 DHCP
 *   net_demo eth-st <ip> <mask> <gw>     以太网静态地址
 *   net_demo wifi-sta <ssid> <passwd>    WiFi 站点模式
 *   net_demo wifi-ap <ssid> <passwd> <dns> WiFi 热点模式（dns 下发给客户端）
 *   net_demo cell                        4G ECM 网卡 + DHCP
 *   net_demo all <ssid> <passwd>         以太网 + WiFi STA + 4G 三条并存
 *
 * 优先级约定：以太网 10、WiFi 20、4G 30，数值越小越优先（等同路由 metric）。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "net_mgr.h"

/** 状态打印周期（秒） */
#define DEMO_PRINT_S 5
/** 状态打印轮数 */
#define DEMO_PRINT_N 12

/**
 * @brief 链路类型转可读名称
 * @param type 链路类型
 * @return 常量字符串，未知类型返回 UNKNOWN
 */
static const char* demo_type_name(NETMGR_TYPE type)
{
    switch (type)
    {
    case NETMGR_ETH:
        return "ETH";
    case NETMGR_WIFI:
        return "WIFI";
    case NETMGR_CELL:
        return "CELL";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 事件类型转可读名称
 * @param ev 事件类型
 * @return 常量字符串，未知类型返回 UNKNOWN
 */
static const char* demo_ev_name(NETMGR_EV ev)
{
    switch (ev)
    {
    case NETMGR_EV_UP:
        return "UP";
    case NETMGR_EV_DOWN:
        return "DOWN";
    case NETMGR_EV_SWITCH:
        return "SWITCH";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 链路状态转可读名称
 * @param state 链路状态
 * @return 常量字符串，未知状态返回 UNKNOWN
 */
static const char* demo_state_name(NETMGR_STATE state)
{
    switch (state)
    {
    case NETMGR_ST_DOWN:
        return "DOWN";
    case NETMGR_ST_READY:
        return "READY";
    case NETMGR_ST_ACTIVE:
        return "ACTIVE";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 链路事件回调：打印链路上线、掉线与主链路切换
 * @param ev   事件类型
 * @param link 触发事件的链路快照
 * @return 恒返回 0
 */
static int demo_event(NETMGR_EV ev, const NETMGR_LINK* link)
{
    if (NULL == link)
        return 0;

    printf("[event] %-4s %-6s ifname=%-6s ip=%-15s gate=%-15s prio=%u\r\n",
           demo_ev_name(ev), demo_type_name(link->type), link->ifname,
           link->ip, link->gate, (unsigned int)link->prio);
    return 0;
}

/**
 * @brief 打印全部链路状态快照
 * @param list 快照数组
 * @param num  数组有效条数
 */
static void demo_dump(const NETMGR_LINK* list, int num)
{
    int i = 0; /* 快照下标，从 0 开始 */

    printf("-------- 链路状态 --------\r\n");
    for (i = 0; i < num; ++i)
    {
        printf("%-6s %-6s ifname=%-6s ip=%-15s gate=%-15s prio=%u\r\n",
               demo_type_name(list[i].type), demo_state_name(list[i].state),
               list[i].ifname, list[i].ip, list[i].gate, (unsigned int)list[i].prio);
    }
}

/**
 * @brief 打印用法说明
 * @param name 程序名
 */
static void demo_usage(const char* name)
{
    printf("用法:\r\n");
    printf("  %s eth\r\n", name);
    printf("  %s eth-st <ip> <mask> <gw>\r\n", name);
    printf("  %s wifi-sta <ssid> <passwd>\r\n", name);
    printf("  %s wifi-ap <ssid> <passwd> <dns>\r\n", name);
    printf("  %s cell\r\n", name);
    printf("  %s all <ssid> <passwd>\r\n", name);
}

/**
 * @brief 解析命令行并填充配置
 * @param conf 待填充的总配置，不可为空
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 0 成功；负值表示参数非法
 */
static int demo_parse(NETMGR_CONF* conf, int argc, char* argv[])
{
    const char* scene = NULL; /* 场景名 */

    if (NULL == conf || argc < 2)
        return -1;
    scene = argv[1];

    if (0 == strcmp(scene, "eth"))
    {
        conf->eth.enable = 1;
        conf->eth.use_dhcp = 1;
        return 0;
    }
    if (0 == strcmp(scene, "eth-st"))
    {
        if (argc < 5)
            return -1;
        conf->eth.enable = 1;
        conf->eth.use_dhcp = 0;
        snprintf(conf->eth.ip, sizeof(conf->eth.ip), "%s", argv[2]);
        snprintf(conf->eth.mask, sizeof(conf->eth.mask), "%s", argv[3]);
        snprintf(conf->eth.gate, sizeof(conf->eth.gate), "%s", argv[4]);
        return 0;
    }
    if (0 == strcmp(scene, "wifi-sta"))
    {
        if (argc < 4)
            return -1;
        conf->wifi.enable = 1;
        conf->wifi.mode = NETMGR_MODE_STA;
        snprintf(conf->wifi.ssid, sizeof(conf->wifi.ssid), "%s", argv[2]);
        snprintf(conf->wifi.passwd, sizeof(conf->wifi.passwd), "%s", argv[3]);
        return 0;
    }
    if (0 == strcmp(scene, "wifi-ap"))
    {
        if (argc < 5)
            return -1;
        conf->wifi.enable = 1;
        conf->wifi.mode = NETMGR_MODE_AP;
        snprintf(conf->wifi.ssid, sizeof(conf->wifi.ssid), "%s", argv[2]);
        snprintf(conf->wifi.passwd, sizeof(conf->wifi.passwd), "%s", argv[3]);
        snprintf(conf->wifi.dns, sizeof(conf->wifi.dns), "%s", argv[4]);
        return 0;
    }
    if (0 == strcmp(scene, "cell"))
    {
        conf->cell.enable = 1;
        conf->cell.mode = NETMGR_CELL_ECM;
        return 0;
    }
    if (0 == strcmp(scene, "all"))
    {
        if (argc < 4)
            return -1;
        conf->eth.enable = 1;
        conf->eth.use_dhcp = 1;
        conf->wifi.enable = 1;
        conf->wifi.mode = NETMGR_MODE_STA;
        snprintf(conf->wifi.ssid, sizeof(conf->wifi.ssid), "%s", argv[2]);
        snprintf(conf->wifi.passwd, sizeof(conf->wifi.passwd), "%s", argv[3]);
        conf->cell.enable = 1;
        conf->cell.mode = NETMGR_CELL_ECM;
        return 0;
    }
    return -1;
}

/**
 * @brief 示例程序入口
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 0 正常结束；1 参数或启动失败
 */
int main(int argc, char* argv[])
{
    NETMGR_CONF conf;                    /* 总配置 */
    NETMGR_LINK list[NETMGR_TYPE_MAX];   /* 状态快照数组 */
    int num = 0;                         /* 快照条数 */
    int ret = 0;                         /* 接口返回值 */
    int i = 0;                           /* 打印轮次，从 0 开始 */

    memset(&conf, 0, sizeof(conf));
    /* 默认优先级：以太网最优，其次 WiFi，最后 4G */
    conf.eth.prio = 10;
    conf.wifi.prio = 20;
    conf.cell.prio = 30;

    if (demo_parse(&conf, argc, argv) < 0)
    {
        demo_usage(argv[0]);
        return 1;
    }

    ret = netmgr_start(&conf, demo_event);
    if (ret < 0)
    {
        printf("启动网络管理失败, ret=%d\r\n", ret);
        return 1;
    }

    /* 周期打印链路状态，便于观察主备切换结果 */
    for (i = 0; i < DEMO_PRINT_N; ++i)
    {
        sleep(DEMO_PRINT_S);
        num = (int)(sizeof(list) / sizeof(list[0]));
        ret = netmgr_stat_get(list, &num);
        if (ret < 0)
        {
            printf("获取链路状态失败, ret=%d\r\n", ret);
            continue;
        }
        demo_dump(list, num);
    }

    (void)netmgr_stop();
    return 0;
}
