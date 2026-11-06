/**
 * @file demo_ap.c
 * @brief netmgr sys 层 AP 模块（net_mgr_ap）独立使用示例
 * @details 常驻交互演示，覆盖 ap 模块全部接口：
 *            s   查询 AP 是否在服务      → netmgr_ap_running
 *            t   启动 AP（写配置 + 拉起 hostapd/udhcpd） → netmgr_ap_start
 *            T   停止 AP（结束两个进程）  → netmgr_ap_stop
 *            h   帮助 / q 退出（退出时若仍在服务则自动停止）
 *          空闲每秒查询一次运行状态，仅在变化时打印。
 *
 * 前置条件（本示例只链接 ap 模块，不含 iface 能力，需手工准备）：
 *   ip link set wlan0 down
 *   iw dev wlan0 set type __ap
 *   ip link set wlan0 up
 *   ip addr add 192.168.4.1/24 dev wlan0
 * 需 root（写 /tmp 配置并启停 hostapd/udhcpd）。
 *
 * 编译：make demo_ap
 * 运行：./demo_ap [ifname] [ssid] [passwd]
 *       ifname 不传时 wlan0；ssid 不传时 netmgr-ap；passwd 不传时 12345678（8 位起）
 * 退出码：0 正常结束；1 启动失败
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include "net_mgr_ap.h"
#include "net_mgr_err.h"

/** 默认接口名 */
#define DEMO_IFNAME   "wlan0"
/** 默认热点名 */
#define DEMO_SSID     "netmgr-ap"
/** 默认口令 */
#define DEMO_PASSWD   "12345678"
/** 主循环轮询间隔（毫秒） */
#define DEMO_POLL_MS  1000
/** 命令行缓冲长度 */
#define DEMO_LINE_LEN 128
/** AP 默认本机地址 / 掩码 / 地址池（与 core 默认值一致） */
#define DEMO_AP_IP    "192.168.4.1"
#define DEMO_AP_MASK  "255.255.255.0"
#define DEMO_AP_START "192.168.4.100"
#define DEMO_AP_END   "192.168.4.200"

/**
 * @brief 打印命令帮助
 */
static void demo_help(void)
{
    printf("[ap] 命令: s=查询状态  t=启动  T=停止  h=帮助  q=退出（空闲每秒刷新，仅变化才打印）\r\n");
}

/**
 * @brief 打印可用性提示（前置条件不满足时的常见原因）
 * @param ret 接口返回码
 */
static void demo_hint(int ret)
{
    if (-NETMGRERR_CONF == ret)
        printf("[ap] 原因: 配置非法——ifname/ssid/ap_ip/ap_mask/地址池均不能为空\r\n");
    else if (-NETMGRERR_FILE == ret)
        printf("[ap] 原因: 配置写盘失败——检查 /tmp 是否可写\r\n");
    else if (-NETMGRERR_PROC == ret)
        printf("[ap] 原因: 进程拉起失败——检查 hostapd/udhcpd 是否在 PATH、接口是否已置为 AP 模式\r\n");
}

/**
 * @brief 按命令行参数填充 AP 配置
 * @param conf   输出配置，不可为空
 * @param ifname 接口名，不可为空
 * @param ssid   热点名，不可为空
 * @param passwd 口令，可为空（空串表示开放 AP）
 */
static void demo_conf_fill(NETMGR_APCONF* conf, const char* ifname,
                           const char* ssid, const char* passwd)
{
    (void)memset(conf, 0, sizeof(*conf));
    (void)snprintf(conf->ifname, sizeof(conf->ifname), "%s", ifname);
    (void)snprintf(conf->ssid, sizeof(conf->ssid), "%s", ssid);
    (void)snprintf(conf->passwd, sizeof(conf->passwd), "%s", (NULL != passwd) ? passwd : "");
    (void)snprintf(conf->ap_ip, sizeof(conf->ap_ip), "%s", DEMO_AP_IP);
    (void)snprintf(conf->ap_mask, sizeof(conf->ap_mask), "%s", DEMO_AP_MASK);
    (void)snprintf(conf->ap_start, sizeof(conf->ap_start), "%s", DEMO_AP_START);
    (void)snprintf(conf->ap_end, sizeof(conf->ap_end), "%s", DEMO_AP_END);
    /* dns 留空：不下发 DNS，客户端沿用自身设置 */
}

/**
 * @brief 示例入口：交互控制 AP 启停
 * @param argc 参数个数
 * @param argv 参数列表：[ifname] [ssid] [passwd]
 * @return 0 正常结束；1 启动失败
 */
int main(int argc, char** argv)
{
    const char* ifname = (argc > 1) ? argv[1] : DEMO_IFNAME; /* 目标接口名 */
    const char* ssid = (argc > 2) ? argv[2] : DEMO_SSID;     /* 热点名 */
    const char* passwd = (argc > 3) ? argv[3] : DEMO_PASSWD; /* 口令 */
    NETMGR_APCONF conf = {0};   /* AP 配置 */
    char line[DEMO_LINE_LEN] = {0}; /* 命令行缓冲 */
    struct pollfd pfd = {0};    /* 标准输入轮询 */
    int quit = 0;               /* 1=收到退出命令 */
    int last = -1;              /* 上轮运行状态，-1 表示尚未取过 */
    int now = 0;                /* 本轮运行状态 */
    int ret = 0;                /* 接口返回值 */

    demo_conf_fill(&conf, ifname, ssid, passwd);

    printf("[ap] 演示开始, ifname=%s ssid=%s 加密=%s ap_ip=%s pool=%s~%s\r\n",
           ifname, ssid, ('\0' != conf.passwd[0]) ? "是" : "否（开放）",
           conf.ap_ip, conf.ap_start, conf.ap_end);
    printf("[ap] 前置条件: 接口须已置为 AP 模式并有本机地址（见文件头说明）\r\n");

    last = netmgr_ap_running();
    printf("[ap] action=running 当前=%s\r\n", (0 != last) ? "在服务" : "未运行");
    demo_help();

    /* 主循环：命中命令即执行，空闲每秒查询状态，仅变化时打印 */
    while (0 == quit)
    {
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        ret = poll(&pfd, 1, DEMO_POLL_MS);
        if (ret > 0 && 0 != (pfd.revents & POLLIN))
        {
            if (NULL == fgets(line, (int)sizeof(line), stdin))
                break; /* stdin 关闭按退出处理 */

            line[strcspn(line, "\r\n")] = '\0';
            switch (line[0])
            {
            case 'h':
                demo_help();
                break;
            case 's':
                now = netmgr_ap_running();
                printf("[ap] action=running 当前=%s\r\n", (0 != now) ? "在服务" : "未运行");
                last = now;
                break;
            case 't':
                printf("[ap] action=start 开始, ifname=%s\r\n", ifname);
                ret = netmgr_ap_start(&conf);
                if (ret < 0)
                {
                    printf("[ap] action=start 失败, ifname=%s ret=%d\r\n", ifname, ret);
                    demo_hint(ret);
                }
                else
                    printf("[ap] action=start 成功（hostapd/udhcpd 已拉起）\r\n");
                break;
            case 'T':
                ret = netmgr_ap_stop();
                if (ret < 0)
                    printf("[ap] action=stop 失败, ret=%d\r\n", ret);
                else
                    printf("[ap] action=stop 成功（两个进程已结束）\r\n");
                break;
            case 'q':
                quit = 1;
                break;
            case '\0':
                break;
            default:
                printf("[ap] 未知命令 cmd='%c'，输入 h 查看帮助\r\n", line[0]);
                break;
            }
        }

        if (0 == quit)
        {
            now = netmgr_ap_running();
            if (now != last)
            {
                printf("[ap] action=running 变化=%s\r\n", (0 != now) ? "在服务" : "未运行");
                last = now;
            }
        }
    }

    /* 退出时若仍在服务则停止，避免示例留下常驻进程 */
    if (0 != netmgr_ap_running())
    {
        ret = netmgr_ap_stop();
        if (ret < 0)
            printf("[ap] 退出前停止失败, ret=%d\r\n", ret);
        else
            printf("[ap] 退出前已停止 AP\r\n");
    }

    printf("[ap] 演示结束\r\n");
    return 0;
}
