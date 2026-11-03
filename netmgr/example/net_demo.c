/**
 * @file net_demo.c
 * @brief netmgr 示例程序：演示以太网、WiFi（STA/AP）、4G 的配置与优先级主备
 *
 * 用法：直接编译运行，配置由文件内的 g_demoConf 给出，不解析命令行参数。
 *      换场景时改 g_demoConf 即可：
 *      - 以太网静态地址：eth.use_dhcp = 0，并填 eth.ip / eth.mask / eth.gate
 *      - WiFi 静态地址： wifi.use_dhcp = 0，并填 wifi.ip / wifi.mask / wifi.gate
 *      - WiFi 热点模式： wifi.mode = NETMGR_MODE_AP，并填 wifi.ssid / wifi.passwd
 *      - 关闭某条链路： 对应成员的 enable 置 0
 *
 * 优先级约定：以太网 10、WiFi 20、4G 30，数值越小越优先（等同路由 metric）。
 */
#include <stdio.h>
#include <unistd.h>

#include "net_mgr.h"

/** 业务轮询间隔（毫秒）：驱动一轮仲裁刷新 */
#define DEMO_POLL_MS 200
/** 每次打印前的轮询次数，25 × 200ms = 5 秒打印一次 */
#define DEMO_POLL_PER_PRINT 25
/** 状态打印轮数 */
#define DEMO_PRINT_N 12

/**
 * 示例总配置（文件级只读配置，示例程序的唯一配置来源）
 * 用途：作为 netmgr_start 的入参，演示三条链路的启用方式与优先级主备
 * 单位/范围：prio 取 0~255（小者优先）；use_dhcp 取 0/1；地址均为点分十进制字符串
 * 默认值依据：三条链路全部启用并按 DHCP/自动探测取址，优先级 10/20/30；
 *             接口名留空表示使用各链路默认接口（eth0、wlan0、自动探测 usb0/wwan0/eth1）
 * 写入方/读取方：仅 main 读取，运行期不修改（static const）
 * 生命周期：仅内存态，随进程存在，不持久化
 */
static const NETMGR_CONF g_demoConf = {
    .eth = {
        .enable = 1,                   /* 启用以太网 */
        .prio = 10,                    /* 优先级 10：三链路中最优 */
        .use_dhcp = 1,                 /* 1=DHCP 取址 */
        .ifname = "",                  /* 空=使用默认 eth0 */
        .ip = "",                      /* 静态 IP，DHCP 取址时不生效 */
        .mask = "",                    /* 静态掩码，DHCP 取址时不生效 */
        .gate = "",                    /* 静态网关，DHCP 取址时不生效 */
        .dns = "",                     /* 空=不参与 DNS 下发 */
    },
    .wifi = {
        .enable = 1,                   /* 启用 WiFi */
        .prio = 20,                    /* 优先级 20：次优 */
        .mode = NETMGR_MODE_STA,       /* STA 模式：连接外部热点 */
        .ifname = "",                  /* 空=使用默认 wlan0 */
        .ssid = "your_ssid",           /* 目标热点名称，按实际修改 */
        .passwd = "your_passwd",       /* 热点密码，空表示开放网络 */
        .use_dhcp = 1,                 /* 1=DHCP 取址；0=静态，需填 ip/mask/gate */
        .ip = "",                      /* STA 静态 IP，DHCP 取址时不生效 */
        .mask = "",                    /* STA 静态掩码，DHCP 取址时不生效 */
        .gate = "",                    /* STA 静态网关，DHCP 取址时不生效 */
        .dns = "",                     /* 空=不参与 DNS 下发 */
        .ap_ip = "",                   /* AP 本机 IP，空=192.168.4.1（仅 AP 模式） */
        .ap_mask = "",                 /* AP 掩码，空=255.255.255.0（仅 AP 模式） */
        .ap_start = "",                /* AP 分配起始，空=192.168.4.100（仅 AP 模式） */
        .ap_end = "",                  /* AP 分配结束，空=192.168.4.200（仅 AP 模式） */
    },
    .cell = {
        .enable = 1,                   /* 启用 4G */
        .prio = 30,                    /* 优先级 30：最低 */
        .mode = NETMGR_CELL_ECM,       /* ECM 网卡形态，走 DHCP */
        .ifname = "",                  /* 空=自动探测 usb0/wwan0/eth1 */
        .apn = "",                     /* APN，仅 PPP 模式使用 */
        .user = "",                    /* 拨号用户名，仅 PPP 模式使用 */
        .passwd = "",                  /* 拨号密码，仅 PPP 模式使用 */
        .dns = "",                     /* 空=不参与 DNS 下发 */
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
 * @brief 示例程序入口：按 g_demoConf 启动网络管理并周期打印链路状态
 * @return 0 正常结束；1 启动或查询失败
 */
int main(void)
{
    NETMGR_LINK list[NETMGR_TYPE_MAX] = {0}; /* 状态快照数组，长度等于链路类型数 */
    int num = 0;                             /* 快照条数，入参为容量、出参为实际条数 */
    int ret = 0;                             /* 接口返回值 */
    int i = 0;                               /* 打印轮次，从 0 开始 */
    int j = 0;                               /* 轮询次数，从 0 开始 */

    ret = netmgr_start(&g_demoConf, demo_event, NULL);
    if (ret < 0)
    {
        /* 业务程序按错误码自行打印日志 */
        printf("启动网络管理失败, ret=%d\r\n", ret);
        return 1;
    }

    /* 业务主循环：周期驱动仲裁刷新，并按固定间隔打印链路状态 */
    for (i = 0; i < DEMO_PRINT_N; ++i)
    {
        for (j = 0; j < DEMO_POLL_PER_PRINT; ++j)
        {
            usleep(DEMO_POLL_MS * 1000);
            ret = netmgr_poll();
            if (ret < 0)
            {
                printf("驱动仲裁失败, ret=%d\r\n", ret);
                break;
            }
        }

        num = (int)(sizeof(list) / sizeof(list[0]));
        ret = netmgr_stat_get(list, &num);
        if (ret < 0)
        {
            printf("获取链路状态失败, ret=%d\r\n", ret);
            continue;
        }
        demo_dump(list, num);
    }

    ret = netmgr_stop();
    if (ret < 0)
        printf("停止网络管理失败, ret=%d\r\n", ret);
    return 0;
}
