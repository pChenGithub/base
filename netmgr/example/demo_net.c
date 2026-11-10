/**
 * @file demo_net.c
 * @brief netmgr/net 层示例程序：交互式命令演示以太网、WiFi（STA/AP）、4G 全部对外接口
 *
 * 用法：直接运行进入交互循环，启动时按 g_demo_conf 拉起服务；输入 h 查看命令，q 退出
 *      （退出前自动 netm_stop）。命令覆盖 net_m_def.h 全部接口：
 *      netm_start / netm_stop / netm_link_get / netm_link_list / netm_active_get /
 *      netm_prio_get / netm_prio_set / netm_prio_check / netm_link_enable /
 *      netm_eth_set / netm_wifi_set / netm_cell_set / netm_dns_set /
 *      netm_wifi_scan / netm_wifi_scan_result / netm_wifi_status
 *
 * 优先级约定：以太网 10、WiFi 20、4G 30，数值越小越优先（等同路由 metric）。
 *
 * 测试项与命令行操作命令对照表（每项一条命令，逐项执行即可对比结果）：
 *   T01  t                                   启动服务：netm_start
 *   T02  T                                   停止服务：netm_stop
 *   T03  s                                   全链路快照：netm_link_list（附 netm_active_get）
 *   T04  g <eth|wifi|cell>                   单链路快照：netm_link_get
 *   T05  m                                   主链路查询：netm_active_get
 *   T06  p <eth|wifi|cell>                   优先级查询：netm_prio_get
 *   T07  p <eth|wifi|cell> <prio>            优先级设置：netm_prio_set
 *   T08  c <eth|wifi|cell> <prio>            优先级预检查：netm_prio_check
 *   T09  e <eth|wifi|cell> <0|1>             链路开关：netm_link_enable
 *   T10  E dhcp [dns]                        以太网 DHCP 重配：netm_eth_set
 *   T11  E static <ip> <mask> <gate> [dns]   以太网静态重配：netm_eth_set
 *   T12  W <ssid> <passwd>                   WiFi STA 重配：netm_wifi_set
 *   T13  A <ssid> [passwd]                   WiFi AP 重配：netm_wifi_set
 *   T14  C <ifname> [dns]                    4G（ECM）重配：netm_cell_set
 *   T15  d <ip>|-                            DNS 下发/清除：netm_dns_set
 *   T16  w                                   WiFi 扫描：netm_wifi_scan
 *   T17  r                                   扫描结果读取：netm_wifi_scan_result
 *   T18  i                                   WiFi 连接信息：netm_wifi_status
 *   T19  （空闲无输入，自动）                链路变化检测：netm_link_get 周期对比
 *   T20  （物理插拔网线，无命令）            载波事件：NET_M_EV_CARRIER_UP / CARRIER_DOWN
 *
 * 系统同效果命令不在此处罗列，一律写在对应 netm_* 调用代码旁边（"系统对照：" 开头），
 * 便于边看代码边执行系统命令核对系统真实状态。
 *
 * 对比口径：T03/T04/T05 对同一链路应给出相同状态与地址，且与各自标注的系统命令输出一致；
 *          T17 的 r 结果与 T16 的 w 结果应一致；T19 打印的"变化"内容须与事件回调
 *          [event] 的前后值一致；T02 后不再出现事件即为停止成功。
 *          T20：插拔网线只出 CARR+/CARR-（带 carrier=0/1），地址变化只出 DHCP/INFO，
 *          两者不再混在一条 UP/DOWN 事件里；插线后取到地址才补一条 UP。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

#include "net_m_def.h"

/** 命令行缓冲长度 */
#define DEMO_LINE_LEN 256
/** 单条命令最多拆分的参数个数 */
#define DEMO_ARG_MAX 8
/** 标准输入轮询超时（毫秒） */
#define DEMO_POLL_MS 1000
/** 状态变化检测周期（秒） */
#define DEMO_WATCH_S 5
/** WiFi 扫描结果缓存条数上限（与 sys 层默认上限一致） */
#define DEMO_SCAN_MAX 16

/**
 * 启动配置（文件级只读配置，交互命令 E / W / A / C 均以它为基线复制修改）
 * 单位/范围：prio 取 0~255（小者优先）；useDhcp 取 0/1；地址均为点分十进制字符串
 * 默认值依据：三条链路全部启用并按 DHCP/自动探测取址，优先级 10/20/30
 * 写入方/读取方：仅本文件读取，运行期不修改（static const）
 */
static const NET_M_CONF g_demo_conf = {
    .eth = {
        .enable = 1,                   /* 启用以太网 */
        .prio = 10,                    /* 优先级 10：三链路中最优 */
        .useDhcp = 1,                  /* 1=DHCP 取址 */
        .ifname = "",                  /* 空=使用默认 eth0 */
        .ip = "",                      /* 静态 IP，DHCP 取址时不生效 */
        .mask = "",                    /* 静态掩码，DHCP 取址时不生效 */
        .gate = "",                    /* 静态网关，DHCP 取址时不生效 */
        .dns = "",                     /* 空=不参与 DNS 下发 */
    },
    .wifi = {
        .enable = 1,                   /* 启用 WiFi */
        .prio = 20,                    /* 优先级 20：次优 */
        .mode = NET_M_MODE_STA,        /* STA 模式：连接外部热点 */
        .ifname = "",                  /* 空=使用默认 wlan0 */
        /* STA 候选热点：prio=0 表示按扫描信号强度择优，多热点按 candNum 生效 */
        .cand = {
            { .ssid = "your_ssid", .passwd = "your_passwd", .prio = 0 },
        },
        .candNum = 1,                  /* 有效候选数，STA 模式必须大于 0 */
        .useDhcp = 1,                  /* 1=DHCP 取址；0=静态，需填 ip/mask/gate */
        .ip = "",
        .mask = "",
        .gate = "",
        .apSsid = "",                  /* AP 热点名称，仅 AP 模式必填 */
        .apPasswd = "",                /* AP 口令，空串表示开放热点 */
        .apIp = "",                    /* 空=192.168.4.1 */
        .apMask = "",                  /* 空=255.255.255.0 */
        .apStart = "",                 /* 空=192.168.4.100 */
        .apEnd = "",                   /* 空=192.168.4.200 */
        .dns = "",
    },
    .cell = {
        .enable = 1,                   /* 启用 4G */
        .prio = 30,                    /* 优先级 30：最低 */
        .mode = NET_M_CELL_ECM,        /* ECM 网卡形态，走 DHCP */
        .ifname = "",                  /* 空=自动探测 usb0/wwan0/eth1 */
        .apn = "",                     /* 预留（PPP / QMI） */
        .user = "",                    /* 预留（PPP / QMI） */
        .passwd = "",                  /* 预留（PPP / QMI） */
        .dns = "",
    },
    .tickMs = 500,                     /* 仲裁轮询间隔（毫秒），<=0 取默认 500 */
};

/**
 * @brief 打印命令帮助
 */
static void demo_help(void)
{
    printf("[net] 命令（覆盖 net_m_def.h 全部接口）：\r\n");
    printf("[net]   s               全部链路快照 + 主链路\r\n");
    printf("[net]   g <eth|wifi|cell>  单链路快照        m            主链路查询\r\n");
    printf("[net]   p <type> [prio]    优先级查询/设置  c <type> <prio>  优先级预检查\r\n");
    printf("[net]   e <type> <0|1>     关闭/启用链路\r\n");
    printf("[net]   E dhcp [dns]       以太网 DHCP 重配\r\n");
    printf("[net]   E static <ip> <mask> <gate> [dns]  以太网静态重配\r\n");
    printf("[net]   W <ssid> <passwd>   WiFi STA 重配    A <ssid> [passwd]  WiFi AP 重配\r\n");
    printf("[net]   C <ifname> [dns]    4G（ECM）重配    d <ip>|-        DNS 下发/清除\r\n");
    printf("[net]   w                WiFi 扫描        r            最近一次扫描结果\r\n");
    printf("[net]   i                WiFi 连接信息    t/T          启动/停止服务\r\n");
    printf("[net]   h               帮助             q            退出（自动 stop）\r\n");
    printf("[net] 空闲时每 %d 秒检测一次链路变化，仅变化才打印\r\n", DEMO_WATCH_S);
}

/**
 * @brief 链路类型转可读名称
 * @param type 链路类型
 * @return 常量字符串，未知类型返回 UNKNOWN
 */
static const char* demo_type_name(NET_M_TYPE type)
{
    switch (type)
    {
    case NET_M_ETH:
        return "ETH";
    case NET_M_WIFI:
        return "WIFI";
    case NET_M_CELL:
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
static const char* demo_ev_name(NET_M_EV ev)
{
    switch (ev)
    {
    case NET_M_EV_UP:
        return "UP";
    case NET_M_EV_DOWN:
        return "DOWN";
    case NET_M_EV_SWITCH:
        return "SWITCH";
    case NET_M_EV_ENABLED:
        return "ENABLED";
    case NET_M_EV_DISABLED:
        return "DISABLED";
    case NET_M_EV_INFO_CHANGE:
        return "INFO";
    case NET_M_EV_ROUTE_CHANGE:
        return "ROUTE";
    case NET_M_EV_DHCP_RUNNING:
        return "DHCP>";
    case NET_M_EV_DHCP_SUCC:
        return "DHCP+";
    case NET_M_EV_DHCP_FAIL:
        return "DHCP-";
    case NET_M_EV_DHCP_LOST:
        return "DHCP!";
    case NET_M_EV_WIFI_CONNECTED:
        return "WIFI+";
    case NET_M_EV_WIFI_DISCONNECTED:
        return "WIFI-";
    case NET_M_EV_WIFI_SCAN_DONE:
        return "SCAN";
    case NET_M_EV_FAULT:
        return "FAULT";
    case NET_M_EV_DEGRADED:
        return "DEGRADE";
    case NET_M_EV_CARRIER_UP:
        return "CARR+";
    case NET_M_EV_CARRIER_DOWN:
        return "CARR-";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 链路状态转可读名称
 * @param state 链路状态
 * @return 常量字符串，未知状态返回 UNKNOWN
 */
static const char* demo_state_name(NET_M_STATE state)
{
    switch (state)
    {
    case NET_M_ST_DISABLED:
        return "DISABLED";
    case NET_M_ST_DOWN:
        return "DOWN";
    case NET_M_ST_READY:
        return "READY";
    case NET_M_ST_ACTIVE:
        return "ACTIVE";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 统一事件回调：打印链路状态、信息变化、取址过程、故障等全部事件
 * @param ev   事件数据（类型 + 按类型有效的载荷）
 * @param user 注册时传入的用户指针，本示例未使用
 * @return 恒返回 0
 */
static int demo_event(const NET_M_EVENT* ev, void* user)
{
    (void)user;
    if (NULL == ev)
        return 0;

    switch (ev->ev)
    {
    case NET_M_EV_SWITCH:
        /* 主链路切换：只关心从哪条切到哪条 */
        printf("[event] SWITCH %s -> %s\r\n",
               demo_type_name(ev->u.sw.from), demo_type_name(ev->u.sw.to));
        break;
    case NET_M_EV_INFO_CHANGE:
        /* 信息变化：位图标注哪些字段变了，前后快照都已给出 */
        printf("[event] INFO   changed=%s%s%s\r\n",
               (0 != (ev->u.info.changed & NET_M_LINK_F_IP)) ? "IP " : "",
               (0 != (ev->u.info.changed & NET_M_LINK_F_MASK)) ? "MASK " : "",
               (0 != (ev->u.info.changed & NET_M_LINK_F_GATE)) ? "GATE" : "");
        printf("[event]        ip=%s -> %s mask=%s -> %s gate=%s -> %s\r\n",
               ev->u.info.prev.ip, ev->u.info.cur.ip,
               ev->u.info.prev.mask, ev->u.info.cur.mask,
               ev->u.info.prev.gate, ev->u.info.cur.gate);
        break;
    case NET_M_EV_ROUTE_CHANGE:
        printf("[event] ROUTE  %s ifname=%s gate=%s\r\n",
               (0 != ev->u.route.isAdd) ? "add" : "del",
               ev->u.route.ref.ifname, ev->u.route.gate);
        break;
    case NET_M_EV_DHCP_RUNNING:
    case NET_M_EV_DHCP_SUCC:
    case NET_M_EV_DHCP_FAIL:
    case NET_M_EV_DHCP_LOST:
        printf("[event] %-6s ifname=%s err=%d\r\n", demo_ev_name(ev->ev),
               ev->u.dhcp.ref.ifname, ev->u.dhcp.err);
        break;
    case NET_M_EV_WIFI_CONNECTED:
    case NET_M_EV_WIFI_DISCONNECTED:
        printf("[event] %-6s ifname=%s 连接状态=%s ssid=%s signal=%ddBm\r\n",
               demo_ev_name(ev->ev), ev->u.wifi.ref.ifname,
               ev->u.wifi.info.connected ? "已连接" : "未连接",
               ev->u.wifi.info.ssid, ev->u.wifi.info.signal);
        break;
    case NET_M_EV_WIFI_SCAN_DONE:
        printf("[event] SCAN   完成=%s apNum=%d\r\n",
               ev->u.scan.complete ? "是" : "否（超时或失败）", ev->u.scan.apNum);
        break;
    case NET_M_EV_FAULT:
    case NET_M_EV_DEGRADED:
        /* 内部故障与链路降级：打印关联链路、故障阶段与错误码 */
        printf("[event] %-7s type=%s stage=%d code=%d\r\n", demo_ev_name(ev->ev),
               demo_type_name(ev->u.fault.type), (int)ev->u.fault.stage, ev->u.fault.code);
        break;
    case NET_M_EV_CARRIER_UP:
    case NET_M_EV_CARRIER_DOWN:
        /* 网线插拔/载波变化：独立事件，仅说明物理连通，地址信息另行看 INFO/DHCP 事件 */
        printf("[event] %-6s %-6s ifname=%-6s carrier=%d\r\n", demo_ev_name(ev->ev),
               demo_type_name(ev->u.link.type), ev->u.link.ifname, ev->u.link.carrier);
        break;
    case NET_M_EV_UP:
    case NET_M_EV_DOWN:
    case NET_M_EV_ENABLED:
    case NET_M_EV_DISABLED:
        /* 以完整链路快照承载的事件 */
        printf("[event] %-6s %-6s ifname=%-6s ip=%-15s mask=%-15s gate=%-15s prio=%u err=%d fault=%d carrier=%d\r\n",
               demo_ev_name(ev->ev), demo_type_name(ev->u.link.type), ev->u.link.ifname,
               ev->u.link.ip, ev->u.link.mask, ev->u.link.gate,
               (unsigned int)ev->u.link.prio, ev->u.link.err, ev->u.link.fault,
               ev->u.link.carrier);
        break;
    default:
        /* 防御：库新增事件而示例未接入时显式暴露，避免误按链路载荷解读 */
        printf("[event] 未识别事件 type=%d（示例未覆盖，请更新 demo_net 的事件打印）\r\n", (int)ev->ev);
        break;
    }
    return 0;
}

/**
 * @brief 链路类型字符串转枚举
 * @param name 类型名，取 eth / wifi / cell
 * @param out  类型输出，不可为空
 * @return 0 识别成功；-1 未知类型或参数非法
 */
static int demo_type_parse(const char* name, NET_M_TYPE* out)
{
    if (NULL == name || NULL == out)
        return -1;
    if (0 == strcmp(name, "eth"))
    {
        *out = NET_M_ETH;
        return 0;
    }
    if (0 == strcmp(name, "wifi"))
    {
        *out = NET_M_WIFI;
        return 0;
    }
    if (0 == strcmp(name, "cell"))
    {
        *out = NET_M_CELL;
        return 0;
    }
    return -1;
}

/**
 * @brief 按空白拆分一行命令（原地写入 '\0'）
 * @param line 输入行，不可为空，会被修改
 * @param args 参数指针数组，不可为空
 * @param max  数组容量
 * @return 拆分出的参数个数
 */
static int demo_split(char* line, char** args, int max)
{
    char* p = line; /* 扫描位置 */
    int num = 0;    /* 已拆出的参数个数 */

    if (NULL == line || NULL == args || max <= 0)
        return 0;

    while ('\0' != *p && num < max)
    {
        while (' ' == *p || '\t' == *p)
            ++p;
        if ('\0' == *p)
            break;

        args[num] = p;
        ++num;
        while ('\0' != *p && ' ' != *p && '\t' != *p)
            ++p;
        if ('\0' != *p)
        {
            *p = '\0';
            ++p;
        }
    }
    return num;
}

/**
 * @brief 带长度校验的字符串复制
 * @param dst    目标数组，不可为空
 * @param dstLen 目标容量
 * @param src    源字符串，不可为空
 * @param name   参数名，用于错误提示，不可为空
 * @return 0 成功；-1 源字符串超长
 */
static int demo_copy(char* dst, int dstLen, const char* src, const char* name)
{
    if (NULL == dst || NULL == src || NULL == name || dstLen <= 0)
        return -1;
    if (strlen(src) + 1 > (size_t)dstLen)
    {
        printf("[net] 参数过长: %s（上限 %d 字节含结束符）\r\n", name, dstLen);
        return -1;
    }
    (void)snprintf(dst, (size_t)dstLen, "%s", src);
    return 0;
}

/**
 * @brief 判断两份链路快照的可观测字段是否一致
 * @param a 快照 A，不可为空
 * @param b 快照 B，不可为空
 * @return 1 一致；0 有变化
 */
static int demo_link_same(const NET_M_LINK* a, const NET_M_LINK* b)
{
    if (NULL == a || NULL == b)
        return 0;
    return (a->state == b->state && a->prio == b->prio && a->err == b->err &&
            a->fault == b->fault && a->carrier == b->carrier &&
            0 == strcmp(a->ip, b->ip) && 0 == strcmp(a->mask, b->mask) &&
            0 == strcmp(a->gate, b->gate));
}

/**
 * @brief 打印一条链路快照
 * @param tag  行首标记（如 状态 / 变化），不可为空
 * @param link 链路快照，不可为空
 */
static void demo_print_link(const char* tag, const NET_M_LINK* link)
{
    if (NULL == tag || NULL == link)
        return;
    printf("[net] %-6s %-6s %-8s ifname=%-6s carrier=%d ip=%-15s mask=%-15s gate=%-15s prio=%u err=%d fault=%d\r\n",
           tag, demo_type_name(link->type), demo_state_name(link->state), link->ifname,
           link->carrier, link->ip, link->mask, link->gate,
           (unsigned int)link->prio, link->err, link->fault);
}

/**
 * @brief 打印全部链路快照与当前主链路（netm_link_list + netm_active_get）
 */
static void demo_cmd_dump(void)
{
    NET_M_LINK list[NET_M_TYPE_MAX] = {0};  /* 快照数组，容量等于链路类型数 */
    NET_M_TYPE active = NET_M_TYPE_MAX;     /* 主链路，无主链路时为最大枚举值 */
    int num = (int)(sizeof(list) / sizeof(list[0])); /* 入参容量 / 出参条数 */
    int ret = 0;                            /* 接口返回值 */
    int i = 0;                              /* 快照下标，从 0 开始 */

    /* 测试项 T03：全链路快照，命令 s；系统对照：ip -br addr show; ip route show default */
    ret = netm_link_list(list, &num);
    if (ret < 0)
    {
        printf("[net] 获取链路列表失败, action=link_list ret=%d\r\n", ret);
        return;
    }

    printf("[net] 链路快照（%d 条）：\r\n", num);
    for (i = 0; i < num; ++i)
        demo_print_link("状态", &list[i]);

    /* 测试项 T05：主链路查询（s 附带输出），命令 m 单独查询；系统对照：ip route get 8.8.8.8 */
    ret = netm_active_get(&active);
    if (ret < 0)
        printf("[net] 获取主链路失败, action=active_get ret=%d\r\n", ret);
    else if (NET_M_TYPE_MAX == active)
        printf("[net] 主链路: 无（没有就绪链路）\r\n");
    else
        printf("[net] 主链路: %s\r\n", demo_type_name(active));
}

/**
 * @brief 周期检测链路变化，仅在变化时打印（事件之外的兜底观察手段）
 * @param last 上轮快照基线，按链路类型下标存放，不可为空；函数返回后更新为本轮快照
 */
static void demo_watch(NET_M_LINK* last)
{
    NET_M_LINK cur = {0}; /* 本轮读到的链路快照 */
    int i = 0;            /* 链路类型下标，从 0 开始 */

    if (NULL == last)
        return;

    for (i = 0; i < NET_M_TYPE_MAX; ++i)
    {
        /* 测试项 T19：链路变化检测，无命令字，空闲每 DEMO_WATCH_S 秒自动执行；
           系统对照：另开终端 ip monitor link，逐条对比"变化"打印 */
        if (netm_link_get((NET_M_TYPE)i, &cur) < 0)
            continue;
        if (0 != demo_link_same(&last[i], &cur))
        {
            last[i] = cur;
            continue;
        }
        demo_print_link("变化", &cur);
        last[i] = cur;
    }
}

/**
 * @brief 读取并打印 WiFi 扫描结果（w 与 r 共用打印格式）
 * @param list 扫描结果数组，不可为空
 * @param num  有效条数
 */
static void demo_print_scan(const NET_M_WIFI_AP* list, int num)
{
    int i = 0; /* 结果下标，从 0 开始 */

    if (NULL == list || num < 0)
        return;
    if (0 == num)
    {
        printf("[net] 扫描结果为空\r\n");
        return;
    }
    for (i = 0; i < num; ++i)
    {
        printf("[net]   ssid=%-32s signal=%ddBm 加密=%s bssid=%s\r\n",
               list[i].ssid, list[i].signal, (0 != list[i].secure) ? "是" : "否", list[i].bssid);
    }
}

/**
 * @brief 执行链路查询/开关类命令：g / m / p / c / e
 * @param av 参数数组，av[0] 为命令字，不可为空
 * @param n  参数个数
 */
static void demo_cmd_link(char** av, int n)
{
    NET_M_TYPE type = NET_M_ETH;      /* 目标链路类型 */
    unsigned char prio = 0;           /* 优先级查询输出 / 设置入参 */
    NET_M_LINK link = {0};            /* 单链路快照 */
    int ret = 0;                      /* 接口返回值 */

    if ('m' == av[0][0])
    {
        /* 测试项 T05：主链路查询，命令 m（无附加参数）；系统对照：ip route get 8.8.8.8 */
        ret = netm_active_get(&type);
        if (ret < 0)
            printf("[net] 主链路查询失败, action=active_get ret=%d\r\n", ret);
        else if (NET_M_TYPE_MAX == type)
            printf("[net] 主链路: 无（没有就绪链路）\r\n");
        else
            printf("[net] 主链路: %s\r\n", demo_type_name(type));
        return;
    }

    if (n < 2 || 0 != demo_type_parse(av[1], &type))
    {
        printf("[net] 用法: %s <eth|wifi|cell>%s\r\n", av[0],
               ('p' == av[0][0]) ? " [prio]" :
               ('c' == av[0][0]) ? " <prio>" :
               ('e' == av[0][0]) ? " <0|1>" : "");
        return;
    }

    if ('g' == av[0][0])
    {
        /* 测试项 T04：单链路快照，命令 g <eth|wifi|cell>；系统对照：ip -br addr show dev <ifname> */
        ret = netm_link_get(type, &link);
        if (ret < 0)
            printf("[net] 获取链路快照失败, action=link_get type=%s ret=%d\r\n",
                   demo_type_name(type), ret);
        else
            demo_print_link("状态", &link);
        return;
    }

    if ('p' == av[0][0] && n < 3)
    {
        /* 测试项 T06：优先级查询，命令 p <eth|wifi|cell>；系统对照：ip route show default */
        ret = netm_prio_get(type, &prio);
        if (ret < 0)
            printf("[net] 优先级查询失败, action=prio_get type=%s ret=%d\r\n",
                   demo_type_name(type), ret);
        else
            printf("[net] %s 当前优先级=%u\r\n", demo_type_name(type), (unsigned int)prio);
        return;
    }

    if ('p' == av[0][0])
    {
        prio = (unsigned char)atoi(av[2]);
        /* 测试项 T07：优先级设置，命令 p <eth|wifi|cell> <prio>；
           系统对照：设置后 ip route show default 看对应 metric 是否改变 */
        ret = netm_prio_set(type, prio);
        if (ret < 0)
            printf("[net] 优先级设置失败, action=prio_set type=%s prio=%u ret=%d\r\n",
                   demo_type_name(type), (unsigned int)prio, ret);
        else
            printf("[net] 优先级设置成功, action=prio_set type=%s prio=%u\r\n",
                   demo_type_name(type), (unsigned int)prio);
        return;
    }

    if ('c' == av[0][0])
    {
        if (n < 3)
        {
            printf("[net] 用法: c <eth|wifi|cell> <prio>\r\n");
            return;
        }
        prio = (unsigned char)atoi(av[2]);
        /* 测试项 T08：优先级预检查，命令 c <eth|wifi|cell> <prio>（不修改状态）；
           系统对照：无，可先用 ip route show default 列出各链路 metric 作对照 */
        ret = netm_prio_check(type, prio);
        if (ret < 0)
            printf("[net] 优先级不可用, action=prio_check type=%s prio=%u ret=%d（与其它启用链路冲突）\r\n",
                   demo_type_name(type), (unsigned int)prio, ret);
        else
            printf("[net] 优先级可用, action=prio_check type=%s prio=%u\r\n",
                   demo_type_name(type), (unsigned int)prio);
        return;
    }

    /* 剩余为 e：启用 / 关闭链路 */
    if (n < 3)
    {
        printf("[net] 用法: e <eth|wifi|cell> <0|1>\r\n");
        return;
    }
    /* 测试项 T09：链路开关，命令 e <eth|wifi|cell> <0|1>；
       系统对照：ip link set <ifname> down|up，再 ip link show <ifname> 看 state
       （库内同一效果：关闭时清 IFF_UP，地址保留） */
    ret = netm_link_enable(type, (0 != atoi(av[2])) ? 1 : 0);
    if (ret < 0)
        printf("[net] 链路开关失败, action=link_enable type=%s on=%d ret=%d\r\n",
               demo_type_name(type), (0 != atoi(av[2])) ? 1 : 0, ret);
    else
        printf("[net] 链路开关成功, action=link_enable type=%s on=%d\r\n",
               demo_type_name(type), (0 != atoi(av[2])) ? 1 : 0);
}

/**
 * @brief 执行 WiFi 查询类命令：w 扫描 / r 最近结果 / i 连接信息
 * @param av 参数数组，av[0] 为命令字，不可为空
 * @param n  参数个数
 */
static void demo_cmd_wifi(char** av, int n)
{
    NET_M_WIFI_AP list[DEMO_SCAN_MAX] = {0}; /* 扫描结果缓存 */
    NET_M_WIFI_INFO info = {0};              /* 当前连接信息 */
    int num = (int)(sizeof(list) / sizeof(list[0])); /* 入参容量 / 出参条数 */
    int ret = 0;                             /* 接口返回值 */

    (void)n;

    if ('w' == av[0][0])
    {
        /* 测试项 T16：WiFi 扫描，命令 w；系统对照：iw dev wlan0 scan */
        ret = netm_wifi_scan(list, &num);
        if (ret < 0)
        {
            printf("[net] WiFi 扫描失败, action=wifi_scan ret=%d\r\n", ret);
            return;
        }
        printf("[net] WiFi 扫描完成, 条数=%d\r\n", num);
        demo_print_scan(list, num);
        return;
    }

    if ('r' == av[0][0])
    {
        /* 测试项 T17：扫描结果读取，命令 r（与 w 的即时结果对比应一致）；
           系统对照：wpa_cli -i wlan0 scan_results */
        ret = netm_wifi_scan_result(list, &num);
        if (ret < 0)
        {
            printf("[net] 读取扫描结果失败, action=wifi_scan_result ret=%d\r\n", ret);
            return;
        }
        printf("[net] 最近一次扫描结果, 条数=%d\r\n", num);
        demo_print_scan(list, num);
        return;
    }

    /* 剩余为 i：连接信息，测试项 T18，命令 i；系统对照：iw dev wlan0 link */
    ret = netm_wifi_status(&info);
    if (ret < 0)
    {
        printf("[net] 读取 WiFi 连接信息失败, action=wifi_status ret=%d\r\n", ret);
        return;
    }
    printf("[net] WiFi 连接=%s ssid=%s bssid=%s freq=%dMHz signal=%ddBm rate=%dMbps\r\n",
           (0 != info.connected) ? "是" : "否", info.ssid, info.bssid,
           info.freq, info.signal, info.linkSpeed);
}

/**
 * @brief 执行 WiFi 重配命令：W（STA）/ A（AP）
 * @param av 参数数组，av[0] 为命令字，不可为空
 * @param n  参数个数
 */
static void demo_cmd_wifi_set(char** av, int n)
{
    NET_M_WIFI_CONF conf = g_demo_conf.wifi; /* 以启动配置为基线 */
    const char* passwd = NULL;               /* 口令，缺省按空串（开放）处理 */
    int ret = 0;                             /* 接口返回值 */

    if (n < 2)
    {
        printf("[net] 用法: %c <ssid> %s\r\n", av[0][0],
               ('W' == av[0][0]) ? "<passwd>" : "[passwd]");
        return;
    }
    passwd = (n >= 3) ? av[2] : "";

    if ('W' == av[0][0])
    {
        /* 测试项 T12：WiFi STA 重配，命令 W <ssid> <passwd>；
           STA：候选热点列表只保留本次这一个，prio=0 按信号强度择优；
           命令立即返回（只重配并启用），关联与取址由内部自愈按退避完成 */
        conf.mode = NET_M_MODE_STA;
        conf.candNum = 1;
        conf.cand[0].prio = 0;
        if (0 != demo_copy(conf.cand[0].ssid, (int)sizeof(conf.cand[0].ssid), av[1], "ssid") ||
            0 != demo_copy(conf.cand[0].passwd, (int)sizeof(conf.cand[0].passwd), passwd, "passwd"))
            return;
    }
    else
    {
        /* 测试项 T13：WiFi AP 重配，命令 A <ssid> [passwd]；
           AP：热点名与口令，其余地址字段沿用启动配置（空串表示用默认值） */
        conf.mode = NET_M_MODE_AP;
        if (0 != demo_copy(conf.apSsid, (int)sizeof(conf.apSsid), av[1], "apSsid") ||
            0 != demo_copy(conf.apPasswd, (int)sizeof(conf.apPasswd), passwd, "apPasswd"))
            return;
    }

    /* 测试项 T12（按 W 进入）/ T13（按 A 进入）：下发 WiFi 配置；
       系统对照：STA 用 iw dev wlan0 link（或 wpa_cli -i wlan0 status），
                 AP 用 iw dev wlan0 info 与 iw dev wlan0 station dump */
    ret = netm_wifi_set(&conf);
    if (ret < 0)
        printf("[net] WiFi 重配失败, action=wifi_set mode=%s ssid=%s ret=%d\r\n",
               ('W' == av[0][0]) ? "STA" : "AP", av[1], ret);
    else
        printf("[net] WiFi 重配成功, action=wifi_set mode=%s ssid=%s\r\n",
               ('W' == av[0][0]) ? "STA" : "AP", av[1]);
}

/**
 * @brief 执行以太网重配命令：E dhcp [dns] / E static <ip> <mask> <gate> [dns]
 * @param av 参数数组，av[0] 为命令字，不可为空
 * @param n  参数个数
 */
static void demo_cmd_eth_set(char** av, int n)
{
    NET_M_ETH_CONF conf = g_demo_conf.eth; /* 以启动配置为基线 */
    int dnsIdx = 0;                        /* dns 参数下标，0 表示未指定 */
    int ret = 0;                           /* 接口返回值 */

    /* 测试项 T10：以太网 DHCP 重配，命令 E dhcp [dns] */
    if (n >= 2 && 0 == strcmp(av[1], "dhcp"))
    {
        conf.useDhcp = 1;
        dnsIdx = 2;
    }
    /* 测试项 T11：以太网静态重配，命令 E static <ip> <mask> <gate> [dns] */
    else if (n >= 5 && 0 == strcmp(av[1], "static"))
    {
        conf.useDhcp = 0;
        if (0 != demo_copy(conf.ip, (int)sizeof(conf.ip), av[2], "ip") ||
            0 != demo_copy(conf.mask, (int)sizeof(conf.mask), av[3], "mask") ||
            0 != demo_copy(conf.gate, (int)sizeof(conf.gate), av[4], "gate"))
            return;
        dnsIdx = 5;
    }
    else
    {
        printf("[net] 用法: E dhcp [dns] | E static <ip> <mask> <gate> [dns]\r\n");
        return;
    }

    if (dnsIdx > 0 && n > dnsIdx)
    {
        if (0 != demo_copy(conf.dns, (int)sizeof(conf.dns), av[dnsIdx], "dns"))
            return;
    }

    /* 测试项 T10（按 dhcp 进入）/ T11（按 static 进入）：下发以太网配置；
       系统对照：DHCP 用 udhcpc -i eth0 后 ip -4 addr show dev eth0，
                 静态用 ip addr flush/add dev eth0 与 ip route add default via <gate>
                 （掩码需换算前缀，如 255.255.255.0 -> /24） */
    ret = netm_eth_set(&conf);
    if (ret < 0)
        printf("[net] 以太网重配失败, action=eth_set mode=%s ret=%d\r\n",
               (0 != conf.useDhcp) ? "dhcp" : "static", ret);
    else
        printf("[net] 以太网重配成功, action=eth_set mode=%s\r\n",
               (0 != conf.useDhcp) ? "dhcp" : "static");
}

/**
 * @brief 执行 4G 重配命令：C <ifname> [dns]
 * @param av 参数数组，av[0] 为命令字，不可为空
 * @param n  参数个数
 */
static void demo_cmd_cell_set(char** av, int n)
{
    NET_M_CELL_CONF conf = g_demo_conf.cell; /* 以启动配置为基线 */
    int ret = 0;                             /* 接口返回值 */

    if (n < 2)
    {
        printf("[net] 用法: C <ifname> [dns]\r\n");
        return;
    }
    if (0 != demo_copy(conf.ifname, (int)sizeof(conf.ifname), av[1], "ifname"))
        return;
    if (n >= 3 && 0 != demo_copy(conf.dns, (int)sizeof(conf.dns), av[2], "dns"))
        return;

    /* 测试项 T14：4G（ECM）重配，命令 C <ifname> [dns]；
       系统对照：ip -br addr show dev usb0; udhcpc -i usb0（接口名以实际枚举为准） */
    ret = netm_cell_set(&conf);
    if (ret < 0)
        printf("[net] 4G 重配失败, action=cell_set ifname=%s ret=%d\r\n", conf.ifname, ret);
    else
        printf("[net] 4G 重配成功, action=cell_set ifname=%s\r\n", conf.ifname);
}

/**
 * @brief 执行服务与 DNS 类命令：d DNS / t 启动 / T 停止
 * @param av 参数数组，av[0] 为命令字，不可为空
 * @param n  参数个数
 */
static void demo_cmd_conf(char** av, int n)
{
    int ret = 0; /* 接口返回值 */

    if ('d' == av[0][0])
    {
        /* d <ip> 下发；d - 或 d 清除（传 NULL） */
        const char* dns = (n >= 2 && 0 != strcmp(av[1], "-")) ? av[1] : NULL;

        /* 测试项 T15：DNS 下发/清除，命令 d <ip> 或 d -（清除）；
           系统对照：cat /etc/resolv.conf（nameserver 行应与下发值一致） */
        ret = netm_dns_set(dns);
        if (ret < 0)
            printf("[net] DNS 下发失败, action=dns_set dns=%s ret=%d\r\n",
                   (NULL != dns) ? dns : "(清除)", ret);
        else
            printf("[net] DNS 下发成功, action=dns_set dns=%s\r\n",
                   (NULL != dns) ? dns : "(清除)");
        return;
    }

    if ('t' == av[0][0])
    {
        /* 测试项 T01：启动服务，命令 t；
           系统对照：ip -4 addr show; ps | grep -E "udhcpc|wpa_supplicant|hostapd" */
        ret = netm_start(&g_demo_conf, demo_event, NULL);
        if (ret < 0)
        {
            printf("[net] 启动服务失败, action=start ret=%d（检查链路配置与优先级是否冲突）\r\n", ret);
            return;
        }
        printf("[net] 启动服务成功, action=start（管理线程已运行）\r\n");
        return;
    }

    /* 剩余为 T：停止服务，测试项 T02，命令 T；
       系统对照：同 T01，停止后应无残留地址、默认路由与相关守护进程 */
    ret = netm_stop();
    if (ret < 0)
        printf("[net] 停止服务失败, action=stop ret=%d\r\n", ret);
    else
        printf("[net] 停止服务成功, action=stop\r\n");
}

/**
 * @brief 按命令字分派
 * @param av 参数数组，av[0] 为命令字，不可为空
 * @param n  参数个数
 * @return 1 表示退出主循环；0 继续
 */
static int demo_cmd_run(char** av, int n)
{
    if (NULL == av || n <= 0)
        return 0;

    switch (av[0][0])
    {
    case 'h':
        demo_help();
        break;
    case 's':
        demo_cmd_dump();
        break;
    case 'q':
        return 1;
    case 'g':
    case 'm':
    case 'p':
    case 'c':
    case 'e':
        demo_cmd_link(av, n);
        break;
    case 'w':
    case 'r':
    case 'i':
        demo_cmd_wifi(av, n);
        break;
    case 'W':
    case 'A':
        demo_cmd_wifi_set(av, n);
        break;
    case 'E':
        demo_cmd_eth_set(av, n);
        break;
    case 'C':
        demo_cmd_cell_set(av, n);
        break;
    case 'd':
    case 't':
    case 'T':
        demo_cmd_conf(av, n);
        break;
    default:
        printf("[net] 未知命令 %s，输入 h 查看帮助\r\n", av[0]);
        break;
    }
    return 0;
}

/**
 * @brief 按启动配置拉起服务，并建立链路快照基线
 * @param last 快照基线数组，按链路类型下标存放，不可为空
 * @return 0 成功；1 失败
 */
static int demo_start(NET_M_LINK* last)
{
    int ret = 0; /* 接口返回值 */
    int i = 0;   /* 链路类型下标，从 0 开始 */

    if (NULL == last)
        return 1;

    /* 测试项 T01：启动服务（启动即执行一次，等价命令 t）；
       系统对照：ip -4 addr show; ps | grep -E "udhcpc|wpa_supplicant|hostapd" */
    ret = netm_start(&g_demo_conf, demo_event, NULL);
    if (ret < 0)
    {
        printf("[net] 启动服务失败, action=start ret=%d\r\n", ret);
        printf("[net] 可按提示修改配置后用命令 t 重试\r\n");
        return 1;
    }
    printf("[net] 启动服务成功, action=start（管理线程已运行）\r\n");

    /* 快照基线（与命令 g 同口径）；
       系统对照：ip -br addr show dev <ifname> */
    for (i = 0; i < NET_M_TYPE_MAX; ++i)
        (void)netm_link_get((NET_M_TYPE)i, &last[i]);
    return 0;
}

/**
 * @brief 示例入口：拉起服务 → 交互主循环（q / EOF 退出，退出前 stop）
 * @return 0 正常结束；1 启动失败
 */
int main(void)
{
    char line[DEMO_LINE_LEN] = {0};    /* 命令行缓冲 */
    char* av[DEMO_ARG_MAX] = {NULL};   /* 拆分后的参数 */
    NET_M_LINK last[NET_M_TYPE_MAX] = {0}; /* 链路快照基线，按类型下标存放 */
    struct pollfd pfd = {0};           /* 标准输入轮询 */
    int argc = 0;                      /* 拆分出的参数个数 */
    int ticks = 0;                     /* 空闲计时（秒），用于变化检测 */
    int quit = 0;                      /* 1=收到退出命令 */
    int ret = 0;                       /* 接口返回值 */

    printf("[net] 网络管理演示：交互式命令覆盖 net_m_def.h 全部接口\r\n");
    printf("[net] 优先级: eth=10 wifi=20 cell=30（小者优先，作为默认路由 metric）\r\n");

    /* 第 1 步：启动服务并建立快照基线，失败仍进入交互（可改配置后 t 重试） */
    (void)demo_start(last);
    demo_help();

    /* 第 2 步：交互主循环——有命令即执行，空闲每 DEMO_WATCH_S 秒检测变化 */
    while (0 == quit)
    {
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        ret = poll(&pfd, 1, DEMO_POLL_MS);
        if (ret > 0 && 0 != (pfd.revents & POLLIN))
        {
            if (NULL == fgets(line, (int)sizeof(line), stdin))
                break; /* 标准输入关闭（EOF / Ctrl+D）按退出处理 */

            line[strcspn(line, "\r\n")] = '\0';
            argc = demo_split(line, av, (int)(sizeof(av) / sizeof(av[0])));
            if (argc > 0)
                quit = demo_cmd_run(av, argc);
            ticks = 0;
            continue;
        }

        if (0 != ret)
            continue; /* poll 出错或被信号打断，下一轮重试 */

        ++ticks;
        if (ticks >= DEMO_WATCH_S)
        {
            ticks = 0;
            demo_watch(last);
        }
    }

    /* 第 3 步：收尾——停止服务（内部撤路由、关链路、回收管理线程），
       测试项 T02，等价命令 T；
       系统对照：同 T01，停止后应无残留地址、默认路由与相关守护进程 */
    ret = netm_stop();
    if (ret < 0)
        printf("[net] 停止服务失败, action=stop ret=%d\r\n", ret);
    else
        printf("[net] 停止服务成功, action=stop\r\n");
    return 0;
}
