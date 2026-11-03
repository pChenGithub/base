/**
 * @file demo_core.c
 * @brief netmgr 仲裁层（core）示例程序：以太网 + WiFi 双链路主备演示
 *
 * 与 net_demo（经 api 层转调）不同，本示例直接面向 core 层接口，
 * 覆盖 core 对外全部能力：启动/停止、事件分发、状态快照、优先级读写、
 * 运行时链路开关与重配置、DNS 下发、WiFi 扫描与结果读取。
 *
 * 用法：./demo_core 运行后按提示输入单字符命令（回车执行）：
 *   h  帮助                              s  打印链路状态快照
 *   p  立即驱动一轮仲裁                  w  WiFi 扫描并打印结果
 *   1  启用以太网（按缓存配置重建）      0  关闭以太网
 *   3  启用 WiFi（按缓存配置重建）       2  关闭 WiFi
 *   g  读取 eth/wifi 当前优先级          G  互换 eth/wifi 优先级（演示主备切换）
 *   e  按缓存配置重建以太网（eth_set）   r  按缓存配置重建 WiFi（wifi_set）
 *   d  下发示例 DNS                      D  清除已下发 DNS
 *   q  退出
 *
 * 优先级约定：以太网 10、WiFi 20，数值越小越优先（等同路由 metric）。
 */
#include <stdio.h>
#include <unistd.h>
#include <poll.h>

#include "net_mgr_core.h"

/** 业务轮询间隔（毫秒）：等待输入期间仍按此节奏驱动仲裁刷新 */
#define DEMO_POLL_MS 200
/** 命令缓冲长度（单字符命令 + 换行 + 结束符） */
#define DEMO_CMD_LEN 16
/** 示例 DNS 地址：仅演示下发动作，按实际网络环境修改 */
#define DEMO_DNS_ADDR "8.8.8.8"

/**
 * 示例总配置（文件级只读配置，示例程序的唯一配置来源）
 * 用途：作为 netmgr_core_start 的入参，演示以太网 + WiFi 双链路启用与主备优先级
 * 单位/范围：prio 取 0~255（小者优先）；use_dhcp 取 0/1；地址均为点分十进制字符串
 * 默认值依据：双链路启用并按 DHCP 取址，优先级 10/20；4G 不启用；
 *             接口名留空表示使用默认接口（eth0、wlan0）
 * 写入方/读取方：仅 main 与命令处理读取，运行期不修改（static const）
 * 生命周期：仅内存态，随进程存在，不持久化
 */
static const NETMGR_CONF g_demoConf = {
    .eth = {
        .enable = 1,                   /* 启用以太网 */
        .prio = 10,                    /* 优先级 10：双链路中最优 */
        .use_dhcp = 1,                 /* 1=DHCP 取址 */
        .ifname = "",                  /* 空=使用默认 eth0 */
        .ip = "",                      /* 静态 IP，DHCP 取址时不生效 */
        .mask = "",                    /* 静态掩码，DHCP 取址时不生效 */
        .gate = "",                    /* 静态网关，DHCP 取址时不生效 */
        .dns = "",                     /* 空=不参与 DNS 下发 */
    },
    .wifi = {
        .enable = 1,                   /* 启用 WiFi */
        .prio = 20,                    /* 优先级 20：次优，作备用链路 */
        .mode = NETMGR_MODE_STA,       /* STA 模式：连接外部热点 */
        .ifname = "",                  /* 空=使用默认 wlan0 */
        .ssid = "your_ssid",           /* 目标热点名称，按实际修改 */
        .passwd = "your_passwd",       /* 热点密码，空表示开放网络 */
        .use_dhcp = 1,                 /* 1=DHCP 取址 */
        .ip = "",                      /* STA 静态 IP，DHCP 取址时不生效 */
        .mask = "",                    /* STA 静态掩码，DHCP 取址时不生效 */
        .gate = "",                    /* STA 静态网关，DHCP 取址时不生效 */
        .dns = "",                     /* 空=不参与 DNS 下发 */
        .ap_ip = "",                   /* AP 参数本示例不使用 */
        .ap_mask = "",
        .ap_start = "",
        .ap_end = "",
    },
    .cell = {
        .enable = 0,                   /* 本示例不启用 4G，双链路演示以太网 + WiFi */
        .prio = 30,
        .mode = NETMGR_CELL_ECM,
        .ifname = "",
        .apn = "",
        .user = "",
        .passwd = "",
        .dns = "",
    },
};

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
 * @return 常量字符串，未知事件返回 UNKNOWN
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
    case NETMGR_EV_ENABLED:
        return "ENABLED";
    case NETMGR_EV_DISABLED:
        return "DISABLED";
    case NETMGR_EV_INFO_CHANGE:
        return "INFO";
    case NETMGR_EV_ROUTE_CHANGE:
        return "ROUTE";
    case NETMGR_EV_DHCP_RUNNING:
        return "DHCP>";
    case NETMGR_EV_DHCP_SUCC:
        return "DHCP+";
    case NETMGR_EV_DHCP_FAIL:
        return "DHCP-";
    case NETMGR_EV_WIFI_CONNECTED:
        return "WIFI+";
    case NETMGR_EV_WIFI_DISCONNECTED:
        return "WIFI-";
    case NETMGR_EV_WIFI_SCAN_DONE:
        return "SCAN";
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
    case NETMGR_ST_DISABLED:
        return "DISABLED";
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
 * @brief 统一事件回调：打印链路状态、信息变化、取址过程等全部事件
 * @param ev   事件数据（类型 + 按类型有效的载荷）
 * @param user 注册时传入的用户指针，本示例未使用
 * @return 恒返回 0
 */
static int demo_event(const NETMGR_EVENT* ev, void* user)
{
    (void)user;
    if (NULL == ev)
        return 0;

    switch (ev->ev)
    {
    case NETMGR_EV_SWITCH:
        /* 主链路切换：只关心从哪条切到哪条 */
        printf("[event] SWITCH %s -> %s\r\n",
               demo_type_name(ev->u.sw.from), demo_type_name(ev->u.sw.to));
        break;
    case NETMGR_EV_INFO_CHANGE:
        /* 信息变化：位图标注哪些字段变了，前后快照都已给出 */
        printf("[event] INFO   changed=0x%x ip=%s -> %s\r\n", ev->u.info.changed,
               ev->u.info.prev.ip, ev->u.info.cur.ip);
        break;
    case NETMGR_EV_ROUTE_CHANGE:
        printf("[event] ROUTE  %s ifname=%s gate=%s\r\n",
               (0 != ev->u.route.isAdd) ? "add" : "del",
               ev->u.route.ref.ifname, ev->u.route.gate);
        break;
    case NETMGR_EV_DHCP_RUNNING:
    case NETMGR_EV_DHCP_SUCC:
    case NETMGR_EV_DHCP_FAIL:
        printf("[event] %-6s ifname=%s err=%d\r\n", demo_ev_name(ev->ev),
               ev->u.dhcp.ref.ifname, ev->u.dhcp.err);
        break;
    case NETMGR_EV_WIFI_CONNECTED:
    case NETMGR_EV_WIFI_DISCONNECTED:
        printf("[event] %-6s ifname=%s connected=%d ssid=%s signal=%d\r\n",
               demo_ev_name(ev->ev), ev->u.wifi.ref.ifname,
               ev->u.wifi.info.connected, ev->u.wifi.info.ssid, ev->u.wifi.info.signal);
        break;
    case NETMGR_EV_WIFI_SCAN_DONE:
        printf("[event] SCAN   complete=%d ap=%d\r\n",
               ev->u.scan.complete, ev->u.scan.apNum);
        break;
    default:
        /* UP/DOWN 等以完整快照承载的事件 */
        printf("[event] %-6s %-6s ifname=%-6s ip=%-15s mask=%-15s gate=%-15s prio=%u err=%d\r\n",
               demo_ev_name(ev->ev), demo_type_name(ev->u.link.type), ev->u.link.ifname,
               ev->u.link.ip, ev->u.link.mask, ev->u.link.gate,
               (unsigned int)ev->u.link.prio, ev->u.link.err);
        break;
    }
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
        printf("%-6s %-6s ifname=%-6s ip=%-15s mask=%-15s gate=%-15s prio=%u err=%d\r\n",
               demo_type_name(list[i].type), demo_state_name(list[i].state),
               list[i].ifname, list[i].ip, list[i].mask,
               list[i].gate, (unsigned int)list[i].prio, list[i].err);
    }
}

/**
 * @brief 打印命令帮助
 */
static void demo_help(void)
{
    printf("-------- 命令 --------\r\n"
           "h=帮助  s=状态  p=驱动仲裁  w=WiFi扫描\r\n"
           "1=启用ETH  0=关闭ETH  3=启用WIFI  2=关闭WIFI\r\n"
           "g=读优先级  G=互换优先级  e=重建ETH  r=重建WIFI\r\n"
           "d=下发DNS  D=清除DNS  q=退出\r\n");
}

/**
 * @brief WiFi 扫描并打印扫描结果
 */
static void demo_scan(void)
{
    NETMGR_WIFI_AP list[NETMGR_SCAN_MAX]; /* 扫描结果数组 */
    int num = (int)(sizeof(list) / sizeof(list[0])); /* 入参容量 / 出参条数 */
    int ret = 0;   /* 接口返回值 */
    int i = 0;     /* 结果下标，从 0 开始 */

    ret = netmgr_core_wifi_scan(list, &num);
    if (ret < 0)
    {
        printf("WiFi 扫描失败, ret=%d（须先启用 WiFi 链路）\r\n", ret);
        return;
    }

    printf("-------- 扫描结果 %d 条 --------\r\n", num);
    for (i = 0; i < num; ++i)
    {
        printf("%-32s %s %-4dMHz %ddBm %s\r\n", list[i].ssid, list[i].bssid,
               list[i].freq, list[i].signal,
               (0 != list[i].secure) ? "加密" : "开放");
    }
}

/**
 * @brief 互换以太网与 WiFi 的优先级，演示主备切换
 */
static void demo_prio_swap(void)
{
    unsigned char ethPrio = 0;  /* 以太网当前优先级 */
    unsigned char wifiPrio = 0; /* WiFi 当前优先级 */
    int ret = 0;                /* 接口返回值 */

    ret = netmgr_core_prio_get(NETMGR_ETH, &ethPrio);
    if (ret < 0)
    {
        printf("读取以太网优先级失败, ret=%d\r\n", ret);
        return;
    }
    ret = netmgr_core_prio_get(NETMGR_WIFI, &wifiPrio);
    if (ret < 0)
    {
        printf("读取 WiFi 优先级失败, ret=%d\r\n", ret);
        return;
    }

    ret = netmgr_core_prio_set(NETMGR_ETH, wifiPrio);
    if (ret < 0)
    {
        printf("设置以太网优先级失败, ret=%d\r\n", ret);
        return;
    }
    ret = netmgr_core_prio_set(NETMGR_WIFI, ethPrio);
    if (ret < 0)
    {
        printf("设置 WiFi 优先级失败, ret=%d\r\n", ret);
        return;
    }
    printf("优先级已互换：ETH %u -> %u, WIFI %u -> %u（主备切换已随本轮仲裁生效）\r\n",
           (unsigned int)ethPrio, (unsigned int)wifiPrio,
           (unsigned int)wifiPrio, (unsigned int)ethPrio);
}

/**
 * @brief 执行单字符命令
 * @param cmd 命令字符
 */
static void demo_cmd_run(char cmd)
{
    int ret = 0; /* 接口返回值 */

    switch (cmd)
    {
    case 'h':
        demo_help();
        break;
    case 's':
    {
        NETMGR_LINK list[NETMGR_TYPE_MAX] = {{0}}; /* 状态快照数组 */
        int num = (int)(sizeof(list) / sizeof(list[0])); /* 入参容量 / 出参条数 */

        ret = netmgr_core_stat_get(list, &num);
        if (ret < 0)
            printf("获取链路状态失败, ret=%d\r\n", ret);
        else
            demo_dump(list, num);
        break;
    }
    case 'p':
        ret = netmgr_core_poll();
        printf("驱动仲裁 ret=%d\r\n", ret);
        break;
    case 'w':
        demo_scan();
        break;
    case '1':
        ret = netmgr_core_link_enable(NETMGR_ETH, 1);
        printf("启用以太网 ret=%d\r\n", ret);
        break;
    case '0':
        ret = netmgr_core_link_enable(NETMGR_ETH, 0);
        printf("关闭以太网 ret=%d\r\n", ret);
        break;
    case '3':
        ret = netmgr_core_link_enable(NETMGR_WIFI, 1);
        printf("启用 WiFi ret=%d\r\n", ret);
        break;
    case '2':
        ret = netmgr_core_link_enable(NETMGR_WIFI, 0);
        printf("关闭 WiFi ret=%d\r\n", ret);
        break;
    case 'g':
    {
        unsigned char ethPrio = 0;  /* 以太网当前优先级 */
        unsigned char wifiPrio = 0; /* WiFi 当前优先级 */

        (void)netmgr_core_prio_get(NETMGR_ETH, &ethPrio);
        (void)netmgr_core_prio_get(NETMGR_WIFI, &wifiPrio);
        printf("当前优先级：ETH=%u WIFI=%u\r\n",
               (unsigned int)ethPrio, (unsigned int)wifiPrio);
        break;
    }
    case 'G':
        demo_prio_swap();
        break;
    case 'e':
        /* 显式重配置：先关闭再按缓存配置重建，事件随返回即可见 */
        ret = netmgr_core_eth_set(&g_demoConf.eth);
        printf("重建以太网 ret=%d\r\n", ret);
        break;
    case 'r':
        ret = netmgr_core_wifi_set(&g_demoConf.wifi);
        printf("重建 WiFi ret=%d\r\n", ret);
        break;
    case 'd':
        ret = netmgr_core_dns_set(DEMO_DNS_ADDR);
        printf("下发 DNS %s ret=%d\r\n", DEMO_DNS_ADDR, ret);
        break;
    case 'D':
        ret = netmgr_core_dns_set("");
        printf("清除 DNS ret=%d\r\n", ret);
        break;
    default:
        printf("未知命令 '%c'，输入 h 查看帮助\r\n", cmd);
        break;
    }
}

/**
 * @brief 示例程序入口：启动双链路仲裁核心，交互式演示 core 全部对外接口
 * @return 0 正常结束；1 启动失败
 */
int main(void)
{
    char cmd[DEMO_CMD_LEN] = {0};   /* 命令输入缓冲 */
    struct pollfd pfd = {0};        /* 标准输入监听描述符 */
    int ret = 0;                    /* 接口返回值 */

    ret = netmgr_core_start(&g_demoConf, demo_event, NULL);
    if (ret < 0)
    {
        /* 业务程序按错误码自行打印日志 */
        printf("启动仲裁核心失败, ret=%d\r\n", ret);
        return 1;
    }
    demo_help();

    /* 主循环：等待输入的间隙按 200ms 节奏驱动仲裁，保证轮询与事件两路都不停 */
    while (1)
    {
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        ret = poll(&pfd, 1, DEMO_POLL_MS);
        if (ret > 0 && 0 != (pfd.revents & POLLIN))
        {
            if (NULL == fgets(cmd, (int)sizeof(cmd), stdin))
                break;
            if ('q' == cmd[0])
                break;
            demo_cmd_run(cmd[0]);
        }
        else if (0 == ret)
        {
            ret = netmgr_core_poll();
            if (ret < 0)
                printf("驱动仲裁失败, ret=%d\r\n", ret);
        }
    }

    ret = netmgr_core_stop();
    if (ret < 0)
        printf("停止仲裁核心失败, ret=%d\r\n", ret);
    return 0;
}
