/**
 * @file demo_core.c
 * @brief netmgr 仲裁层（core）示例程序：以太网 + WiFi 双链路主备演示
 *
 * 与 net_demo（经 api 层转调）不同，本示例直接面向 core 层接口，
 * 覆盖 core 对外全部能力：启动/停止、事件分发、状态快照、优先级读写、
 * 运行时链路开关与重配置、DNS 下发、WiFi 扫描与结果读取。
 *
 * 用法：sudo ./demo_core [eth_ifname] [wifi_ssid] [wifi_passwd]
 *       （改接口/路由/起进程需 root；接口名按实际设备填，如 eth0 / enp0s3 / wlan0）
 *       运行后按提示输入单字符命令（回车执行）：
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
#include <string.h>
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
 * 示例总配置（文件级配置，示例程序的唯一配置来源）
 * 用途：作为 netmgr_core_start 的入参，演示以太网 + WiFi 双链路启用与主备优先级
 * 单位/范围：prio 取 0~255（小者优先）；use_dhcp 取 0/1；地址均为点分十进制字符串
 * 默认值依据：双链路启用并按 DHCP 取址，优先级 10/20；4G 不启用；
 *             接口名留空表示使用默认接口（eth0、wlan0），也可由命令行覆盖
 * 写入方/读取方：main 启动前按命令行参数覆盖（接口名 / SSID / 密码），运行期只读
 * 生命周期：仅内存态，随进程存在，不持久化
 */
static NETMGR_CONF g_demoConf = {
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
    /* 仲裁定时驱动 500ms：由库内部线程周期跑仲裁，业务主循环无需再调 poll。
       AP 进程意外退出等“无事件”变化也能在半个周期内被发现并上报 */
    .tickMs = 500,
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
    case NETMGR_EV_DHCP_LOST:
        return "DHCP!";
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
        /* 信息变化：位图分解出变化字段，前后快照逐项打印，便于核对 */
        printf("[event] INFO   changed=%s%s%s\r\n",
               (0 != (ev->u.info.changed & NETMGR_LINK_F_IP)) ? "IP " : "",
               (0 != (ev->u.info.changed & NETMGR_LINK_F_MASK)) ? "MASK " : "",
               (0 != (ev->u.info.changed & NETMGR_LINK_F_GATE)) ? "GATE" : "");
        printf("[event]        ip=%s -> %s mask=%s -> %s gate=%s -> %s\r\n",
               ev->u.info.prev.ip, ev->u.info.cur.ip,
               ev->u.info.prev.mask, ev->u.info.cur.mask,
               ev->u.info.prev.gate, ev->u.info.cur.gate);
        break;
    case NETMGR_EV_ROUTE_CHANGE:
        printf("[event] ROUTE  %s ifname=%s gate=%s\r\n",
               (0 != ev->u.route.isAdd) ? "add" : "del",
               ev->u.route.ref.ifname, ev->u.route.gate);
        break;
    case NETMGR_EV_DHCP_RUNNING:
    case NETMGR_EV_DHCP_SUCC:
    case NETMGR_EV_DHCP_FAIL:
    case NETMGR_EV_DHCP_LOST:
        printf("[event] %-6s ifname=%s err=%d\r\n", demo_ev_name(ev->ev),
               ev->u.dhcp.ref.ifname, ev->u.dhcp.err);
        break;
    case NETMGR_EV_WIFI_CONNECTED:
    case NETMGR_EV_WIFI_DISCONNECTED:
        printf("[event] %-6s ifname=%s 连接状态=%s ssid=%s signal=%ddBm\r\n",
               demo_ev_name(ev->ev), ev->u.wifi.ref.ifname,
               ev->u.wifi.info.connected ? "已连接" : "未连接",
               ev->u.wifi.info.ssid, ev->u.wifi.info.signal);
        break;
    case NETMGR_EV_WIFI_SCAN_DONE:
        printf("[event] SCAN   完成=%s apNum=%d\r\n",
               ev->u.scan.complete ? "是" : "否（超时或失败）", ev->u.scan.apNum);
        break;
    case NETMGR_EV_UP:
    case NETMGR_EV_DOWN:
    case NETMGR_EV_ENABLED:
    case NETMGR_EV_DISABLED:
        /* 以完整链路快照承载的事件 */
        printf("[event] %-6s %-6s ifname=%-6s ip=%-15s mask=%-15s gate=%-15s prio=%u err=%d\r\n",
               demo_ev_name(ev->ev), demo_type_name(ev->u.link.type), ev->u.link.ifname,
               ev->u.link.ip, ev->u.link.mask, ev->u.link.gate,
               (unsigned int)ev->u.link.prio, ev->u.link.err);
        break;
    default:
        /* 防御：库新增事件而示例未接入时显式暴露，避免误按链路载荷解读 */
        printf("[event] 未识别事件 type=%d（示例未覆盖，请更新 demo_core 的事件打印）\r\n", (int)ev->ev);
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

    printf("[core] 链路状态：\r\n");
    for (i = 0; i < num; ++i)
    {
        printf("[core]   %-6s %-6s ifname=%-6s ip=%-15s mask=%-15s gate=%-15s prio=%u err=%d\r\n",
               demo_type_name(list[i].type), demo_state_name(list[i].state),
               list[i].ifname, list[i].ip, list[i].mask,
               list[i].gate, (unsigned int)list[i].prio, list[i].err);
    }
}

/**
 * @brief 比较两次链路快照是否一致
 * @param a 快照 A，不可为空
 * @param b 快照 B，不可为空
 * @return 1 一致；0 不一致
 */
static int demo_link_same(const NETMGR_LINK* a, const NETMGR_LINK* b)
{
    if (a->type != b->type || a->state != b->state || a->prio != b->prio || a->err != b->err)
        return 0;
    if (0 != strcmp(a->ifname, b->ifname) || 0 != strcmp(a->ip, b->ip) ||
        0 != strcmp(a->mask, b->mask) || 0 != strcmp(a->gate, b->gate))
        return 0;
    return 1;
}

/** 上一次打印的链路快照，用于状态变化检测 */
static NETMGR_LINK g_demoLast[NETMGR_TYPE_MAX];
/** 1=已建立基线快照 */
static int g_demoLastValid = 0;

/**
 * @brief 仅在链路快照变化时打印（稳态不刷屏，跃变即时可见）
 */
static void demo_print_on_change(void)
{
    NETMGR_LINK list[NETMGR_TYPE_MAX] = {{0}}; /* 本次快照 */
    int num = (int)(sizeof(list) / sizeof(list[0])); /* 入参容量 / 出参条数 */
    int changed = 0;                                 /* 1=存在变化 */
    int ret = 0;                                     /* 接口返回值 */
    int i = 0;                                       /* 快照下标，从 0 开始 */

    ret = netmgr_core_stat_get(list, &num);
    if (ret < 0)
        return;

    for (i = 0; i < num; ++i)
    {
        if (0 != g_demoLastValid && 0 != demo_link_same(&list[i], &g_demoLast[i]))
            continue;
        changed = 1;
        g_demoLast[i] = list[i];
    }
    g_demoLastValid = 1;

    if (0 != changed)
        demo_dump(list, num);
}

/**
 * @brief 按命令行参数覆盖接口名与 WiFi 账号
 * @details 用法：demo_core [eth_ifname] [wifi_ssid] [wifi_passwd]；缺省沿用文件内配置
 * @param argc 参数个数
 * @param argv 参数数组，不可为空
 */
static void demo_args(int argc, char* argv[])
{
    if (argc > 1)
        (void)snprintf(g_demoConf.eth.ifname, sizeof(g_demoConf.eth.ifname), "%s", argv[1]);
    if (argc > 2)
        (void)snprintf(g_demoConf.wifi.ssid, sizeof(g_demoConf.wifi.ssid), "%s", argv[2]);
    if (argc > 3)
        (void)snprintf(g_demoConf.wifi.passwd, sizeof(g_demoConf.wifi.passwd), "%s", argv[3]);
}

/**
 * @brief 打印一次全部链路快照（启动即用，DISABLED 时 err 给出未启用原因）
 */
static void demo_state_print(void)
{
    NETMGR_LINK list[NETMGR_TYPE_MAX] = {{0}}; /* 状态快照数组 */
    int num = (int)(sizeof(list) / sizeof(list[0])); /* 入参容量 / 出参条数 */
    int ret = 0;                                     /* 接口返回值 */

    ret = netmgr_core_stat_get(list, &num);
    if (ret < 0)
    {
        printf("[core] 获取链路状态失败, action=stat_get ret=%d\r\n", ret);
        return;
    }
    demo_dump(list, num);
}

/**
 * @brief 检查是否有链路未启用成功，并给出排查提示
 * @details 常见原因：接口名与实际不符、非 root 运行（改接口/路由/起进程需权限）、
 *          外部进程（wpa_supplicant/udhcpc）不可执行
 */
static void demo_check(void)
{
    NETMGR_LINK list[NETMGR_TYPE_MAX] = {{0}}; /* 状态快照数组 */
    int num = (int)(sizeof(list) / sizeof(list[0])); /* 入参容量 / 出参条数 */
    int ret = 0;                                     /* 接口返回值 */
    int i = 0;                                       /* 快照下标，从 0 开始 */

    ret = netmgr_core_stat_get(list, &num);
    if (ret < 0)
        return;

    for (i = 0; i < num; ++i)
    {
        if (NETMGR_ST_DISABLED != list[i].state || 0 == list[i].err)
            continue;
        printf("[提示] %s 未启用 (err=%d)：核对接口名是否存在、是否以 root 运行、"
               "wpa_supplicant/udhcpc 是否可执行\r\n",
               demo_type_name(list[i].type), list[i].err);
    }
}

/**
 * @brief 打印命令帮助
 */
static void demo_help(void)
{
    printf("[core] 命令列表：\r\n"
           "h=帮助  s=状态  w=WiFi扫描\r\n"
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
        printf("[core] WiFi 扫描失败, action=scan ret=%d（须先启用 WiFi 链路）\r\n", ret);
        return;
    }

    printf("[core] 扫描结果：apNum=%d\r\n", num);
    for (i = 0; i < num; ++i)
    {
        printf("[core]   %-32s %s %-4dMHz %ddBm %s\r\n", list[i].ssid, list[i].bssid,
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
        printf("[core] 读取优先级失败, link=ETH ret=%d\r\n", ret);
        return;
    }
    ret = netmgr_core_prio_get(NETMGR_WIFI, &wifiPrio);
    if (ret < 0)
    {
        printf("[core] 读取优先级失败, link=WIFI ret=%d\r\n", ret);
        return;
    }

    ret = netmgr_core_prio_set(NETMGR_ETH, wifiPrio);
    if (ret < 0)
    {
        printf("[core] 设置优先级失败, link=ETH ret=%d\r\n", ret);
        return;
    }
    ret = netmgr_core_prio_set(NETMGR_WIFI, ethPrio);
    if (ret < 0)
    {
        printf("[core] 设置优先级失败, link=WIFI ret=%d\r\n", ret);
        return;
    }
    printf("[core] 优先级已互换: ETH %u -> %u, WIFI %u -> %u（主备切换已随本轮仲裁生效）\r\n",
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
        demo_state_print();
        break;
    case 'w':
        demo_scan();
        break;
    case '1':
        ret = netmgr_core_link_enable(NETMGR_ETH, 1);
        printf("[core] 启用链路, link=ETH ret=%d\r\n", ret);
        break;
    case '0':
        ret = netmgr_core_link_enable(NETMGR_ETH, 0);
        printf("[core] 关闭链路, link=ETH ret=%d\r\n", ret);
        break;
    case '3':
        ret = netmgr_core_link_enable(NETMGR_WIFI, 1);
        printf("[core] 启用链路, link=WIFI ret=%d\r\n", ret);
        break;
    case '2':
        ret = netmgr_core_link_enable(NETMGR_WIFI, 0);
        printf("[core] 关闭链路, link=WIFI ret=%d\r\n", ret);
        break;
    case 'g':
    {
        unsigned char ethPrio = 0;  /* 以太网当前优先级 */
        unsigned char wifiPrio = 0; /* WiFi 当前优先级 */

        (void)netmgr_core_prio_get(NETMGR_ETH, &ethPrio);
        (void)netmgr_core_prio_get(NETMGR_WIFI, &wifiPrio);
        printf("[core] 当前优先级: ETH=%u WIFI=%u\r\n",
               (unsigned int)ethPrio, (unsigned int)wifiPrio);
        break;
    }
    case 'G':
        demo_prio_swap();
        break;
    case 'e':
        /* 显式重配置：先关闭再按缓存配置重建，事件随返回即可见 */
        ret = netmgr_core_eth_set(&g_demoConf.eth);
        printf("[core] 重建链路, link=ETH ret=%d\r\n", ret);
        break;
    case 'r':
        ret = netmgr_core_wifi_set(&g_demoConf.wifi);
        printf("[core] 重建链路, link=WIFI ret=%d\r\n", ret);
        break;
    case 'd':
        ret = netmgr_core_dns_set(DEMO_DNS_ADDR);
        printf("[core] 下发 DNS, server=%s ret=%d\r\n", DEMO_DNS_ADDR, ret);
        break;
    case 'D':
        ret = netmgr_core_dns_set("");
        printf("[core] 清除 DNS ret=%d\r\n", ret);
        break;
    default:
        printf("[core] 未知命令 cmd='%c'，输入 h 查看帮助\r\n", cmd);
        break;
    }
}

/**
 * @brief 示例程序入口：启动双链路仲裁核心，交互式演示 core 全部对外接口
 * @return 0 正常结束；1 启动失败
 */
int main(int argc, char* argv[])
{
    char cmd[DEMO_CMD_LEN] = {0};   /* 命令输入缓冲 */
    struct pollfd pfd = {0};        /* 标准输入监听描述符 */
    int ret = 0;                    /* 接口返回值 */

    /* 命令行可覆盖接口名与 WiFi 账号：不同设备接口名不同（如 enp0s3），
       且改接口/路由/起进程需要 root 权限 */
    demo_args(argc, argv);
    printf("[core] 用法：sudo ./demo_core [eth_ifname] [wifi_ssid] [wifi_passwd]\r\n");
    printf("[core] 配置: eth.ifname=%s wifi.ssid=%s\r\n",
           ('\0' == g_demoConf.eth.ifname[0]) ? "(默认 eth0)" : g_demoConf.eth.ifname,
           ('\0' == g_demoConf.wifi.ssid[0]) ? "(空)" : g_demoConf.wifi.ssid);

    ret = netmgr_core_start(&g_demoConf, demo_event, NULL);
    if (ret < 0)
    {
        /* 业务程序按错误码自行打印日志 */
        printf("[core] 启动仲裁核心失败, action=start ret=%d\r\n", ret);
        return 1;
    }

    /* 启动后立即呈现链路状态：DISABLED 且 err 非 0 即表示该链路没起来（接口名/权限/进程） */
    demo_state_print();
    demo_check();
    demo_help();

    /* 主循环：输入处理与仲裁驱动互不依赖——每个 200ms 周期都跑一轮仲裁，
       netlink 事件只起加速作用，事件没到时也不会漏掉状态变化 */
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

        /* 仲裁已由库内 tickMs 定时驱动，业务不再周期性调 poll；
           这里只在主循环里做“变化才打印”，保持输出安静且跃变即时可见 */
        demo_print_on_change();
    }

    ret = netmgr_core_stop();
    if (ret < 0)
        printf("[core] 停止仲裁核心失败, action=stop ret=%d\r\n", ret);
    return 0;
}
