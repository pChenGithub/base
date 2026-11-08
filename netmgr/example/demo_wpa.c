/**
 * @file demo_wpa.c
 * @brief netmgr wpa_supplicant 控制接口模块独立示例（sys 层，覆盖全部对外接口）
 * @details 常驻交互演示：启动后建立通道并进入主循环，按键操作、空闲时每秒刷新状态，
 *          按 q 退出。命令与接口对应关系：
 *            s            状态 + 连接信息          → netmgr_wpa_state / netmgr_wpa_status
 *            w            扫描并打印结果            → netmgr_wpa_scan / netmgr_wpa_scan_result
 *            c <ssid> [psk] 连接（psk 省略则为开放网络） → netmgr_wpa_connect
 *            d            断开                    → netmgr_wpa_disconnect
 *            o            打开控制通道             → netmgr_wpa_open
 *            i            查询通道是否打开          → netmgr_wpa_is_open
 *            x            关闭控制通道             → netmgr_wpa_close
 *            f [country]  写 wpa 配置文件           → netmgr_wpa_conf_write
 *            a [timeoutMs] 等控制口就绪             → netmgr_wpa_wait
 *            t            拉起 wpa_supplicant      → netmgr_wpa_start
 *            T            停止 wpa_supplicant      → netmgr_wpa_stop
 *            e            开/关事件通道            → netmgr_wpa_event_open / event_close
 *            v            原始协议自检（诊断）        → 示例层自检，不经库内通道
 *            h            帮助 / q 退出
 *
 * 编译：make demo_wpa
 * 运行：./demo_wpa [ifname] [--start] [--diag]
 *       ifname  不传时使用 wlan0
 *       --start 启动时先由本示例拉起 wpa_supplicant（需 root；有 systemd 托管的板子建议不加）
 *       --diag  启动前做一次原始协议自检（connect/send/recv 的 errno 与回包）
 * 退出码：0 正常结束；1 控制通道建立或进程拉起失败
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "net_mgr_wpa.h"
#include "net_mgr_err.h"

/** 默认演示接口名 */
#define DEMO_IFNAME "wlan0"
/** 扫描结果数组容量 */
#define DEMO_AP_MAX 16
/** 主循环轮询间隔（毫秒）：无输入时用于周期性刷新状态 */
#define DEMO_POLL_MS 1000
/** 命令行缓冲长度：需容纳 “c <ssid> <psk>” */
#define DEMO_LINE_LEN 128
/** `--start` 演示时写出的 wpa_supplicant 最小配置路径 */
#define DEMO_CONF   "/tmp/netmgr_wpa_demo.conf"

/** 目标接口名（由启动参数决定） */
static const char* g_ifname = DEMO_IFNAME;
/** 1=由本示例拉起 wpa_supplicant（--start） */
static int g_startProc = 0;
/** 1=事件通道已打开 */
static int g_evOn = 0;

/**
 * @brief wpa 工作状态转可读名称
 * @param state 工作状态
 * @return 常量字符串，未知状态返回“未知”
 */
static const char* demo_state_name(NETMGR_WPA_STATE state)
{
    switch (state)
    {
    case NETMGR_WPA_SCANNING:
        return "扫描中";
    case NETMGR_WPA_ASSOCIATING:
        return "关联中";
    case NETMGR_WPA_COMPLETED:
        return "已连接";
    case NETMGR_WPA_DISCONNECTED:
        return "已断开";
    default:
        return "未知";
    }
}

/**
 * @brief wpa 事件类别转可读名称
 * @param type 事件类别
 * @return 常量字符串
 */
static const char* demo_ev_name(NETMGR_WEV_TYPE type)
{
    switch (type)
    {
    case NETMGR_WEV_DISCONNECTED:
        return "DISCONNECTED";
    case NETMGR_WEV_CONNECTED:
        return "CONNECTED";
    case NETMGR_WEV_SCAN_DONE:
        return "SCAN_DONE";
    case NETMGR_WEV_ASSOC_REJECT:
        return "ASSOC_REJECT";
    case NETMGR_WEV_TERMINATING:
        return "TERMINATING";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 事件回调：打印事件类别与关联字段（在事件接收线程上下文执行）
 * @param ev   事件内容，仅在回调期间有效
 * @param user 未使用
 * @return 恒返回 0，继续监听
 */
static int demo_ev_cb(const NETMGR_WEV* ev, void* user)
{
    (void)user;

    if (NULL == ev)
        return 0;

    /* 按类别取对应联合体载荷：不同类别的字段互斥，禁止跨类型读取 */
    switch (ev->type)
    {
    case NETMGR_WEV_CONNECTED:
        /* CONNECTED 事件不带 SSID（需查 STATUS），此处只打印 BSSID */
        printf("[event] %-12s bssid=%s\r\n", demo_ev_name(ev->type), ev->bssid);
        break;
    case NETMGR_WEV_DISCONNECTED:
        printf("[event] %-12s bssid=%s reason=%d\r\n",
               demo_ev_name(ev->type), ev->bssid, ev->u.disc.reason);
        break;
    case NETMGR_WEV_ASSOC_REJECT:
        printf("[event] %-12s bssid=%s status=%d\r\n",
               demo_ev_name(ev->type), ev->bssid, ev->u.reject.status);
        break;
    default:
        /* SCAN_DONE / TERMINATING 无载荷 */
        printf("[event] %-12s\r\n", demo_ev_name(ev->type));
        break;
    }
    return 0;
}

/**
 * @brief 读取并打印工作状态与连接信息
 * @details 单项读取失败只报原因、不中断（后续扫描仍可进行）
 * @param ifname 接口名，不可为空
 */
static void demo_state_show(const char* ifname)
{
    NETMGR_WPA_STATE state = NETMGR_WPA_UNKNOWN; /* 工作状态 */
    NETMGR_WIFI_INFO info = {0};                /* 连接信息 */
    int ret = 0;                                /* 接口返回值 */

    ret = netmgr_wpa_state(&state);
    if (ret < 0)
        printf("[wpa] ifname=%s action=state 失败, ret=%d\r\n", ifname, ret);
    else
        printf("[wpa] ifname=%s state=%s\r\n", ifname, demo_state_name(state));

    ret = netmgr_wpa_status(&info);
    if (ret < 0)
    {
        printf("[wpa] ifname=%s action=status 失败, ret=%d\r\n", ifname, ret);
        return;
    }
    printf("[wpa] ifname=%s 连接状态=%s ssid=%s bssid=%s freq=%dMHz signal=%ddBm speed=%d\r\n",
           ifname, info.connected ? "已连接" : "未连接", info.ssid, info.bssid,
           info.freq, info.signal, info.linkSpeed);
}

/**
 * @brief 触发一次扫描并逐条打印结果
 * @details 扫描超时取模块默认值；失败与结果读取失败都会打印结果码
 * @param ifname 接口名，不可为空
 */
static void demo_scan(const char* ifname)
{
    NETMGR_WIFI_AP list[DEMO_AP_MAX] = {0}; /* 扫描结果数组 */
    int num = DEMO_AP_MAX;                  /* 入参容量 / 出参条数 */
    int ret = 0;                            /* 接口返回值 */
    int i = 0;                              /* 结果下标，从 0 开始 */

    printf("[wpa] ifname=%s action=scan 开始（等待扫描结束，最长 8 秒）\r\n", ifname);
    ret = netmgr_wpa_scan(0);
    if (ret < 0)
    {
        printf("[wpa] ifname=%s action=scan 失败, ret=%d（超时为 -NETMGRERR_TIMEOUT）\r\n",
               ifname, ret);
        return;
    }

    ret = netmgr_wpa_scan_result(list, &num);
    if (ret < 0)
    {
        printf("[wpa] ifname=%s action=scan_result 失败, ret=%d\r\n", ifname, ret);
        return;
    }

    printf("[wpa] ifname=%s 扫描完成, apNum=%d\r\n", ifname, num);
    for (i = 0; i < num; ++i)
    {
        printf("[wpa]   [%2d] ssid=%-20s bssid=%s freq=%4dMHz signal=%4ddBm 加密=%s\r\n",
               i, list[i].ssid, list[i].bssid, list[i].freq, list[i].signal,
               list[i].secure ? "是" : "否");
    }
}

/**
 * @brief 直连控制口做一次最小协议自检（示例层诊断，不经库内通道）
 * @details 依次打印 connect / send / recv 的返回值与 errno，并原样打印回包，
 *          用于区分「通道不可达」「请求被忽略（协议不符）」「回包被丢弃」三类问题；
 *          先发裸命令 PING，再发 CTRL-REQ 包裹形式，最后再做一次绑定本地地址的对照
 * @param ifname 接口名，不可为空
 */
static void demo_proto_diag(const char* ifname)
{
    struct sockaddr_un addr = {0}; /* 控制口地址 */
    struct timeval tv = {0};       /* 接收超时 */
    char path[sizeof(addr.sun_path)] = {0}; /* 控制口路径，与 sockaddr_un 容量一致 */
    char buf[512] = {0};           /* 回包缓冲 */
    int fd = -1;                   /* 临时 socket */
    int ret = 0;                   /* 各步骤返回值 */

    (void)snprintf(path, sizeof(path), "/var/run/wpa_supplicant/%s", ifname);
    printf("[diag] path=%s\r\n", path);

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    printf("[diag] socket=%d errno=%d\r\n", fd, errno);
    if (fd < 0)
        return;

    tv.tv_sec = 2;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));

    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    errno = 0;
    ret = connect(fd, (struct sockaddr*)&addr, (socklen_t)sizeof(addr));
    printf("[diag] connect=%d errno=%d\r\n", ret, errno);

    /* 第 1 次：裸命令（本库实际发送的形式） */
    errno = 0;
    ret = (int)send(fd, "PING", 4, 0);
    printf("[diag] send(PING)=%d errno=%d\r\n", ret, errno);
    errno = 0;
    ret = (int)recv(fd, buf, sizeof(buf) - 1, 0);
    printf("[diag] recv(裸命令)=%d errno=%d", ret, errno);
    if (ret > 0)
    {
        buf[ret] = '\0';
        printf(" 回包=[%s]", buf);
    }
    printf("\r\n");

    /* 第 2 次：CTRL-REQ 包裹形式，wpa_cli 走的就是这种 */
    errno = 0;
    ret = (int)send(fd, "CTRL-REQ-1-PING", 15, 0);
    printf("[diag] send(CTRL-REQ)=%d errno=%d\r\n", ret, errno);
    errno = 0;
    ret = (int)recv(fd, buf, sizeof(buf) - 1, 0);
    printf("[diag] recv(CTRL-REQ)=%d errno=%d", ret, errno);
    if (ret > 0)
    {
        buf[ret] = '\0';
        printf(" 回包=[%s]", buf);
    }
    printf("\r\n");

    close(fd);

    /* 第 3 次：绑定到本地路径后再连（wpa 只能往“有地址的客户端”回包） */
    {
        struct sockaddr_un self = {0}; /* 本端地址 */
        const char* selfPath = "/tmp/netmgr_wpa_diag.sock";

        (void)unlink(selfPath);
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0)
            return;

        self.sun_family = AF_UNIX;
        (void)snprintf(self.sun_path, sizeof(self.sun_path), "%s", selfPath);
        errno = 0;
        ret = bind(fd, (struct sockaddr*)&self, (socklen_t)sizeof(self));
        printf("[diag] bind(%s)=%d errno=%d\r\n", selfPath, ret, errno);

        tv.tv_sec = 2;
        tv.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));
        errno = 0;
        ret = connect(fd, (struct sockaddr*)&addr, (socklen_t)sizeof(addr));
        printf("[diag] connect(已绑定)=%d errno=%d\r\n", ret, errno);

        errno = 0;
        ret = (int)send(fd, "PING", 4, 0);
        printf("[diag] send(已绑定)=%d errno=%d\r\n", ret, errno);
        errno = 0;
        ret = (int)recv(fd, buf, sizeof(buf) - 1, 0);
        printf("[diag] recv(已绑定)=%d errno=%d", ret, errno);
        if (ret > 0)
        {
            buf[ret] = '\0';
            printf(" 回包=[%s]", buf);
        }
        printf("\r\n");

        close(fd);
        (void)unlink(selfPath);
    }
}

/**
 * @brief 打印命令帮助
 */
static void demo_help(void)
{
    printf("[wpa] 命令（覆盖模块全部接口）：\r\n");
    printf("[wpa]   s            状态+连接信息   w            扫描并打印结果\r\n");
    printf("[wpa]   c<ssid> [psk] 连接           d            断开\r\n");
    printf("[wpa]   o            打开通道        i            查询通道状态\r\n");
    printf("[wpa]   x            关闭通道        f [country]  写配置文件\r\n");
    printf("[wpa]   a [timeoutMs] 等控制口        t            拉起进程（start）\r\n");
    printf("[wpa]   T            停止进程（stop） e            开/关事件通道\r\n");
    printf("[wpa]   v            协议自检        h            帮助    q  退出\r\n");
    printf("[wpa] 空闲时每秒刷新状态，仅变化才打印\r\n");
}

/**
 * @brief 周期刷新工作状态，仅在变化时打印
 * @param ifname 接口名，不可为空
 * @param last   上一次状态（入参为基线，出参更新为本次状态），不可为空
 */
static void demo_watch(const char* ifname, NETMGR_WPA_STATE* last)
{
    NETMGR_WPA_STATE state = NETMGR_WPA_UNKNOWN; /* 本次读到的工作状态 */

    if (0 != netmgr_wpa_state(&state))
        return;
    if (state == *last)
        return;

    printf("[wpa] ifname=%s state=%s（变化）\r\n", ifname, demo_state_name(state));
    *last = state;
}

/**
 * @brief 执行一条交互命令
 * @details 命令字取首字符，其余部分按命令需要解析；参数缺失时给出用法提示
 * @param line 整行输入（已去掉换行），不可为空
 * @param last 状态基线，供 s 命令刷新，不可为空
 * @return 1 表示退出主循环；0 继续
 */
static int demo_cmd_run(char* line, NETMGR_WPA_STATE* last)
{
    char* arg = line + 1; /* 命令参数起点 */
    int ret = 0;          /* 接口返回值 */

    while (' ' == *arg)
        ++arg;

    switch (line[0])
    {
    case 'h':
        demo_help();
        break;
    case 's':
        demo_state_show(g_ifname);
        (void)netmgr_wpa_state(last);
        break;
    case 'w':
        demo_scan(g_ifname);
        break;
    case 'c':
    {
        /* c <ssid> [psk]：psk 省略表示开放网络 */
        char* ssid = arg;      /* SSID 起点 */
        char* psk = NULL;      /* 口令起点，可为空 */
        char* sep = strchr(ssid, ' ');

        if ('\0' == ssid[0])
        {
            printf("[wpa] 用法: c <ssid> [psk]\r\n");
            break;
        }
        if (NULL != sep)
        {
            *sep = '\0';
            psk = sep + 1;
            while (' ' == *psk)
                ++psk;
            if ('\0' == psk[0])
                psk = NULL;
        }

        printf("[wpa] ifname=%s action=connect 开始, ssid=%s 加密=%s\r\n",
               g_ifname, ssid, (NULL != psk) ? "是" : "否（开放网络）");
        ret = netmgr_wpa_connect(ssid, psk, 0);
        if (ret < 0)
            printf("[wpa] ifname=%s action=connect 失败, ssid=%s ret=%d\r\n", g_ifname, ssid, ret);
        else
            printf("[wpa] ifname=%s action=connect 成功, ssid=%s\r\n", g_ifname, ssid);
        break;
    }
    case 'd':
        ret = netmgr_wpa_disconnect();
        if (ret < 0)
            printf("[wpa] ifname=%s action=disconnect 失败, ret=%d\r\n", g_ifname, ret);
        else
            printf("[wpa] ifname=%s action=disconnect 成功\r\n", g_ifname);
        break;
    case 'o':
        ret = netmgr_wpa_open(g_ifname);
        if (ret < 0)
            printf("[wpa] ifname=%s action=open 失败, ret=%d\r\n", g_ifname, ret);
        else
            printf("[wpa] ifname=%s action=open 成功\r\n", g_ifname);
        break;
    case 'i':
        printf("[wpa] action=is_open 返回=%s\r\n", (0 != netmgr_wpa_is_open()) ? "已打开" : "未打开");
        break;
    case 'x':
        if (0 != g_evOn)
        {
            (void)netmgr_wpa_event_close();
            g_evOn = 0;
            printf("[wpa] action=event_close 完成（关闭通道前先停事件）\r\n");
        }
        netmgr_wpa_close();
        printf("[wpa] ifname=%s action=close 完成\r\n", g_ifname);
        break;
    case 'f':
        ret = netmgr_wpa_conf_write(DEMO_CONF, ('\0' != arg[0]) ? arg : NULL);
        if (ret < 0)
            printf("[wpa] action=conf_write 失败, path=%s ret=%d\r\n", DEMO_CONF, ret);
        else
            printf("[wpa] action=conf_write 成功, path=%s country=%s\r\n",
                   DEMO_CONF, ('\0' != arg[0]) ? arg : "(未写入)");
        break;
    case 'a':
        ret = netmgr_wpa_wait(g_ifname, ('\0' != arg[0]) ? atoi(arg) : 0);
        if (ret < 0)
            printf("[wpa] ifname=%s action=wait 失败, ret=%d（控制口不可用：确认进程已拉起）\r\n",
                   g_ifname, ret);
        else
            printf("[wpa] ifname=%s action=wait 成功（控制口已就绪）\r\n", g_ifname);
        break;
    case 't':
        ret = netmgr_wpa_start(g_ifname, DEMO_CONF, NULL, 0);
        if (ret < 0)
            printf("[wpa] ifname=%s action=start 失败, ret=%d\r\n", g_ifname, ret);
        else
            printf("[wpa] ifname=%s action=start 成功, conf=%s\r\n", g_ifname, DEMO_CONF);
        break;
    case 'T':
        ret = netmgr_wpa_stop();
        g_evOn = 0; /* stop 内部会关闭事件通道与命令通道 */
        if (ret < 0)
            printf("[wpa] action=stop 失败, ret=%d\r\n", ret);
        else
            printf("[wpa] action=stop 成功（进程已结束）\r\n");
        break;
    case 'e':
        if (0 == g_evOn)
        {
            ret = netmgr_wpa_event_open(demo_ev_cb, NULL);
            if (ret < 0)
                printf("[wpa] action=event_open 失败, ret=%d（须先建立命令通道）\r\n", ret);
            else
            {
                g_evOn = 1;
                printf("[wpa] action=event_open 成功（连接/断开/扫描事件将实时打印）\r\n");
            }
        }
        else
        {
            (void)netmgr_wpa_event_close();
            g_evOn = 0;
            printf("[wpa] action=event_close 成功\r\n");
        }
        break;
    case 'v':
        demo_proto_diag(g_ifname);
        break;
    case 'q':
        return 1;
    default:
        printf("[wpa] 未知命令 cmd='%c'，输入 h 查看帮助\r\n", line[0]);
        break;
    }
    return 0;
}

/**
 * @brief 启动时建立通道：--start 拉起进程后连通道，否则连接已运行实例
 * @return 0 成功；1 失败
 */
static int demo_channel_up(void)
{
    int ret = 0; /* 接口返回值 */

    if (0 != g_startProc)
    {
        /* 拉起 wpa_supplicant（会结束现有实例；国家码传 NULL 表示不写 country 行） */
        ret = netmgr_wpa_start(g_ifname, DEMO_CONF, NULL, 0);
        if (ret < 0)
        {
            printf("[wpa] ifname=%s action=start 失败, ret=%d\r\n", g_ifname, ret);
            if (-NETMGRERR_TIMEOUT == ret)
                printf("[wpa] 原因: 控制口未出现或连上后不应答——确认 wpa_supplicant 能以该配置"
                       "启动、接口未被其它实例占用\r\n");
            else if (-NETMGRERR_SOCKET == ret)
                printf("[wpa] 原因: /var/run/wpa_supplicant/%s 存在但无人监听\r\n", g_ifname);
            else if (-NETMGRERR_PROC == ret)
                printf("[wpa] 原因: 进程拉起失败——检查 wpa_supplicant 是否在 PATH、是否有执行权限\r\n");
            return 1;
        }
        printf("[wpa] ifname=%s action=start 成功, conf=%s\r\n", g_ifname, DEMO_CONF);
        return 0;
    }

    ret = netmgr_wpa_open(g_ifname);
    if (ret < 0)
    {
        printf("[wpa] ifname=%s action=open 失败, ret=%d（需 wpa_supplicant 已运行且 "
               "/var/run/wpa_supplicant/%s 存在；可加 --start 由本示例拉起）\r\n",
               g_ifname, ret, g_ifname);
        return 1;
    }
    printf("[wpa] ifname=%s action=open 成功\r\n", g_ifname);
    return 0;
}

/**
 * @brief 示例入口：建立通道 → 交互主循环（q 退出）
 * @param argc 参数个数
 * @param argv 参数列表：接口名 / --start / --diag
 * @return 0 正常结束；1 建立通道失败
 */
int main(int argc, char** argv)
{
    int quit = 0;                               /* 1=收到退出命令 */
    int diag = 0;                               /* 1=启动前做一次协议自检 */
    int ret = 0;                                /* 接口返回值 */
    int i = 0;                                  /* 参数下标，从 1 开始 */
    char line[DEMO_LINE_LEN] = {0};             /* 命令行缓冲 */
    struct pollfd pfd = {0};                    /* 标准输入轮询 */
    NETMGR_WPA_STATE last = NETMGR_WPA_UNKNOWN; /* 上轮状态，用于变化打印 */

    /* 参数解析：--start / --diag 为开关，其余非选项参数视为接口名 */
    for (i = 1; i < argc; ++i)
    {
        if (0 == strcmp(argv[i], "--start"))
        {
            g_startProc = 1;
            continue;
        }
        if (0 == strcmp(argv[i], "--diag"))
        {
            diag = 1;
            continue;
        }
        if (0 == strncmp(argv[i], "--", 2))
        {
            printf("[wpa] 忽略未知选项: %s\r\n", argv[i]);
            continue;
        }
        g_ifname = argv[i];
    }

    if (0 != diag)
        demo_proto_diag(g_ifname);

    printf("[wpa] 控制接口演示, ifname=%s 进程模式=%s\r\n", g_ifname,
           (0 != g_startProc) ? "start/stop（本示例拉起并停止）" : "attach（连接已运行实例）");
    /* 版本标记：确认运行的是最新构建（客户端已绑定本地地址，命令不再超时） */
    printf("[wpa] 构建标记: 全接口覆盖 / 常驻交互 / 客户端已绑定本地地址\r\n");

    /* 第 1 步：建立通道（--start 时先拉起进程），失败即退出 */
    if (0 != demo_channel_up())
        return 1;

    /* 第 2 步：打印一次初始状态，并建立变化检测基线 */
    demo_state_show(g_ifname);
    (void)netmgr_wpa_state(&last);
    demo_help();

    /* 第 3 步：主循环——命中命令即执行，空闲按秒刷新状态 */
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
            if ('\0' != line[0])
                quit = demo_cmd_run(line, &last);
        }

        if (0 == quit)
            demo_watch(g_ifname, &last);
    }

    /* 第 4 步：收尾。--start 模式停止进程（内部含断开与关通道）；否则断开并关闭通道 */
    if (0 != g_evOn)
    {
        (void)netmgr_wpa_event_close();
        g_evOn = 0;
    }

    if (0 != g_startProc)
    {
        ret = netmgr_wpa_stop();
        if (ret < 0)
            printf("[wpa] ifname=%s action=stop 失败, ret=%d\r\n", g_ifname, ret);
        else
            printf("[wpa] ifname=%s action=stop 成功（进程已结束）\r\n", g_ifname);
    }
    else
    {
        ret = netmgr_wpa_disconnect();
        if (ret < 0)
            printf("[wpa] ifname=%s action=disconnect 失败, ret=%d\r\n", g_ifname, ret);
        else
            printf("[wpa] ifname=%s action=disconnect 成功\r\n", g_ifname);
        netmgr_wpa_close();
    }

    printf("[wpa] ifname=%s 演示结束\r\n", g_ifname);
    return 0;
}

/*
 * 事件监听用法（示例中按 e 命令即走这条路径）：
 *
 *   netmgr_wpa_open("wlan0");                 // 事件通道依赖命令通道，必须先 open
 *   netmgr_wpa_event_open(demo_ev_cb, NULL);  // 内部起 1 个接收线程，回调在该线程上下文执行
 *   ...                                       // 回调里只打印/投递，不做阻塞操作
 *   netmgr_wpa_event_close();                 // 先停线程再 DETACH，之后才可关命令通道
 *
 * 事件类别：CONNECTED / DISCONNECTED(reason) / SCAN_DONE / ASSOC_REJECT(status) / TERMINATING
 * 注意：事件是服务端主动推送，客户端 socket 必须已绑定本地地址，否则一条也收不到。
 */
