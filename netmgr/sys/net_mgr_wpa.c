/**
 * @file net_mgr_wpa.c
 * @brief netmgr 系统适配层：wpa_supplicant 控制接口实现（sys 层）
 * @details 控制协议只做「发一行命令、收一条回复」：
 *          协议为裸命令/裸回包（与 wpa_cli 一致）：命令正文直接下发，回包正文直接解析，
 *          失败以 FAIL 起头；客户端必须 bind 本地地址，否则服务端回包无处投递；
 *          正文以 FAIL 开头即视为命令失败。本层不关心业务策略，
 *          wpa_supplicant 的启动/结束与进程生命周期归调用方编排，本层只提供协议与通道能力。
 *          回复超长时以错误码上抛，不做静默截断；扫描结果量级较大，用堆缓冲接收。
 *          事件通道（CTRL-EVENT-*）与命令通道使用两条 socket：ATTACH 后 wpa 会把事件
 *          推送到监听连接，与命令回复混流，故监听须独立建连（同 wpa_ctrl 的双通道做法）。
 */
#include "net_mgr_wpa.h"

#include "net_mgr_err.h"
#include "net_mgr_proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/un.h>

/** wpa_supplicant 控制接口目录 */
#define WPA_DIR         "/var/run/wpa_supplicant"
/** wpa_supplicant 进程名，用于独占启动与结束（比对上 /proc/<pid>/comm） */
#define WPA_PROC_NAME   "wpa_supplicant"
/** 控制接口路径长度，与 sockaddr_un.sun_path 对齐 */
#define WPA_PATH_LEN    108
/** 命令与短回复缓冲长度（STATUS / ADD_NETWORK 等） */
#define WPA_BUF_LEN     1024
/** 扫描结果回复缓冲上限；AP 较多时超过 1KB，用堆缓冲避免截断 */
#define WPA_RSP_MAX     8192
/** 拼接命令的缓冲长度 */
#define WPA_CMD_LEN     128
/** 网络编号缓冲长度 */
#define WPA_ID_LEN      16
/** 控制接口轮询间隔（微秒） */
#define WPA_POLL_US     100000
/** 控制接口就绪默认等待时长（毫秒）：按挂钟计，覆盖慢板与 P2P 设备初始化 */
#define WPA_OPEN_MS     10000
/** 通道可用性探测次数（每次间隔 WPA_POLL_US）：覆盖 wpa 初始化未就绪的窗口 */
#define WPA_PROBE_MAX   10
/** 客户端本地地址路径：按进程号与通道区分，避免同机多实例冲突 */
#define WPA_CLI_PATH    "/tmp/netmgr_wpa.%d.%s.sock"
/** 探测单次收发超时（毫秒）：通道不可用时不必等满命令超时 */
#define WPA_PROBE_TIMEOUT_MS 300
/** 关联完成默认等待次数，共 10 秒 */
#define WPA_CONN_MAX    100
/** 扫描结束默认等待次数，共 8 秒；全信道/双频扫描耗时 4~8 秒，取大值避免误报超时 */
#define WPA_SCAN_MAX    80
/** 事件接收线程单次等待时长（毫秒），超时回环检查运行标志，保证 close 能及时回收 */
#define WPA_EV_TIMEOUT_MS 500
/** WPA 预共享密钥最小长度 */
#define WPA_PSK_MIN     8
/** 回复失败标记长度（"FAIL"） */
#define WPA_FAIL_LEN    4
/** 控制通道收发超时（秒） */
#define WPA_SOCK_TIMEOUT_S 3

/** wpa 控制通道上下文（进程内单实例） */
typedef struct {
    int sock;                   /**< 控制 socket，-1 表示未打开 */
    char ifname[NETMGR_IF_LEN]; /**< 当前绑定的接口名 */
    pthread_mutex_t lock;       /**< 命令收发互斥锁，保证一问一答不交叉 */
} NETMGR_WPA;

static NETMGR_WPA g_wpa = {
    .sock = -1,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/** wpa 事件监听上下文（进程内单实例） */
typedef struct {
    int sock;                /**< 事件监听 socket，-1 表示未启动 */
    int running;             /**< 接收线程运行标志，close 时置 0 */
    pthread_t thread;        /**< 接收线程 */
    netmgr_wev_cb cb;        /**< 事件回调 */
    void* user;              /**< 回调用户指针 */
    pthread_mutex_t lock;    /**< 监听上下文互斥锁，保护启停与字段 */
} NETMGR_WEVCTX;

static NETMGR_WEVCTX g_wpaEv = {
    .sock = -1,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/**
 * @brief 拼出指定接口的控制接口路径
 * @param ifname 接口名，不可为空
 * @param out    输出缓冲，不可为空
 * @param len    输出缓冲长度
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int wpa_path(const char* ifname, char* out, int len)
{
    int n = 0; /* 格式化结果长度 */

    if (NULL == ifname || NULL == out || len <= 0)
        return -NETMGRERR_PARAM;

    n = snprintf(out, (size_t)len, "%s/%s", WPA_DIR, ifname);
    if (n < 0 || n >= len)
        return -NETMGRERR_BUFF;
    return 0;
}

/**
 * @brief 生成客户端本地地址路径
 * @param tag 通道标识（"cmd" / "ev" / "probe"），不可为空
 * @param out 输出缓冲，不可为空
 * @param len 输出缓冲长度
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int wpa_cli_path(const char* tag, char* out, int len)
{
    int n = 0; /* 写入长度 */

    if (NULL == tag || NULL == out || len <= 0)
        return -NETMGRERR_PARAM;

    n = snprintf(out, (size_t)len, WPA_CLI_PATH, (int)getpid(), tag);
    if (n < 0 || n >= len)
        return -NETMGRERR_BUFF;
    return 0;
}

/**
 * @brief 绑定客户端本地地址
 * @details AF_UNIX 数据报客户端必须先有地址，服务端才能把回包/事件送回来；
 *          不绑定则 connect 成功、send 成功，但永远收不到回应（命令全部超时）
 * @param fd  已创建的数据报 socket，>=0
 * @param tag 通道标识，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int wpa_bind_client(int fd, const char* tag)
{
    struct sockaddr_un self = {0}; /* 本端地址 */
    char path[WPA_PATH_LEN] = {0}; /* 本端路径 */
    int ret = 0;                   /* 路径生成结果 */

    if (fd < 0)
        return -NETMGRERR_PARAM;

    ret = wpa_cli_path(tag, path, (int)sizeof(path));
    if (ret < 0)
        return ret;

    self.sun_family = AF_UNIX;
    (void)snprintf(self.sun_path, sizeof(self.sun_path), "%s", path);
    (void)unlink(path); /* 清理上次异常退出留下的同名文件 */
    if (0 != bind(fd, (struct sockaddr*)&self, (socklen_t)sizeof(self)))
        return -NETMGRERR_SOCKET;
    return 0;
}

/**
 * @brief 解绑并删除客户端本地地址文件
 * @param tag 通道标识，不可为空
 */
static void wpa_unbind_client(const char* tag)
{
    char path[WPA_PATH_LEN] = {0}; /* 本端路径 */

    if (0 == wpa_cli_path(tag, path, (int)sizeof(path)))
        (void)unlink(path);
}

/**
 * @brief 取单调时钟毫秒数
 * @details 用 CLOCK_MONOTONIC：系统时间被回拨也不影响等待超时计算
 * @return 毫秒数
 */
static long long wpa_now_ms(void)
{
    struct timespec ts = {0}; /* 单调时钟读数 */

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
}

/**
 * @brief 在指定 socket 上收发一条命令，回复正文就地前移到缓冲头部（调用方须已持锁）
 * @param fd  目标 socket（命令通道或事件通道）
 * @param cmd 命令文本，不可为空
 * @param buf 收发缓冲，不可为空
 * @param len 缓冲长度
 * @return 0 成功；负值为 NETMGRERR_* 错误码（回复超出缓冲返回 -NETMGRERR_BUFF）
 */
static int wpa_rsp(int fd, const char* cmd, char* buf, int len)
{
    struct msghdr msg = {0}; /* 接收消息头，用 MSG_TRUNC 判断是否截断 */
    struct iovec iov = {0};  /* 接收区间 */
    ssize_t n = 0;           /* 收发字节数 */

    if (NULL == cmd || NULL == buf || len <= 0)
        return -NETMGRERR_PARAM;
    if (fd < 0)
        return -NETMGRERR_STATE;

    if (0 > send(fd, cmd, strlen(cmd), 0))
        return -NETMGRERR_WPA;

    iov.iov_base = buf;
    iov.iov_len = (size_t)len - 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* MSG_TRUNC 让内核回传原始报文长度：超长即报错，禁止截断后继续解析 */
    n = recvmsg(fd, &msg, MSG_TRUNC);
    if (n <= 0)
        return -NETMGRERR_WPA;
    if (0 != (msg.msg_flags & MSG_TRUNC) || n > (ssize_t)(len - 1))
        return -NETMGRERR_BUFF;

    buf[n] = '\0';

    /* 本层发送的是裸命令，wpa_supplicant 对该形态直接回正文（无前缀、无 OK 包裹）：
       失败以 FAIL 起头，其余正文即命令输出；事件推送另走 CTRL-EVENT-*，不经过本函数 */
    if (0 == strncmp(buf, "FAIL", WPA_FAIL_LEN))
        return -NETMGRERR_WPA;
    return 0;
}

/**
 * @brief 加锁发送一条短命令并取回复正文
 * @param cmd 命令文本，不可为空
 * @param out 回复正文输出缓冲，可为空（仅执行命令不取正文）
 * @param len 输出缓冲长度
 * @return 0 成功；负值为 NETMGRERR_* 错误码（正文超长返回 -NETMGRERR_BUFF）
 */
/**
 * @brief 重连控制通道：丢弃当前 socket 后按记录的接口名重新 connect
 * @details 用于对端失效场景（wpa_supplicant 重启、控制口被其它实例替换）
 * @return 0 成功；负值为 NETMGRERR_* 错误码（无可用记录时返回 -NETMGRERR_STATE）
 */
static int wpa_reconnect(void)
{
    char ifname[NETMGR_IF_LEN] = {0}; /* 重连使用的接口名 */

    pthread_mutex_lock(&g_wpa.lock);
    if (g_wpa.sock < 0 || '\0' == g_wpa.ifname[0])
    {
        pthread_mutex_unlock(&g_wpa.lock);
        return -NETMGRERR_STATE;
    }
    (void)snprintf(ifname, sizeof(ifname), "%s", g_wpa.ifname);
    close(g_wpa.sock);
    g_wpa.sock = -1;
    g_wpa.ifname[0] = '\0';
    pthread_mutex_unlock(&g_wpa.lock);

    return netmgr_wpa_open(ifname);
}

/**
 * @brief 探测控制通道是否可用（PING/PONG）
 * @details 用短超时收发：通道不可用时立即判失败，避免 start 阶段被长时间拖住
 * @return 0 通道可用；负值为 NETMGRERR_* 错误码
 */
static int wpa_probe(void)
{
    char buf[64] = {0};        /* PONG 回复缓冲 */
    struct timeval tv = {0};   /* 探测专用收发超时 */
    int fd = -1;               /* 当前控制 socket */
    int ret = 0;               /* 探测结果 */

    pthread_mutex_lock(&g_wpa.lock);
    fd = g_wpa.sock;
    pthread_mutex_unlock(&g_wpa.lock);
    if (fd < 0)
        return -NETMGRERR_STATE;

    tv.tv_usec = WPA_PROBE_TIMEOUT_MS * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));

    pthread_mutex_lock(&g_wpa.lock);
    ret = wpa_rsp(fd, "PING", buf, (int)sizeof(buf));
    pthread_mutex_unlock(&g_wpa.lock);

    /* 恢复常规超时，后续命令按 WPA_SOCK_TIMEOUT_S 等待 */
    tv.tv_sec = WPA_SOCK_TIMEOUT_S;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));
    return ret;
}

static int wpa_cmd(const char* cmd, char* out, int len)
{
    char buf[WPA_BUF_LEN] = {0}; /* 收发缓冲 */
    int ret = 0;                 /* 收发结果 */

    pthread_mutex_lock(&g_wpa.lock);
    ret = wpa_rsp(g_wpa.sock, cmd, buf, (int)sizeof(buf));
    pthread_mutex_unlock(&g_wpa.lock);

    /* 对端失效（wpa_supplicant 重启、控制口文件被重建）时收发直接报错，
       重连一次再重试，避免把一次重启当成持续故障 */
    if (-NETMGRERR_WPA == ret && 0 == wpa_reconnect())
    {
        pthread_mutex_lock(&g_wpa.lock);
        memset(buf, 0, sizeof(buf));
        ret = wpa_rsp(g_wpa.sock, cmd, buf, (int)sizeof(buf));
        pthread_mutex_unlock(&g_wpa.lock);
    }

    if (0 == ret && NULL != out && len > 0)
    {
        /* 正文比调用方缓冲长时报错，避免截断后误用半个值 */
        if ((int)strlen(buf) >= len)
            ret = -NETMGRERR_BUFF;
        else
            (void)snprintf(out, (size_t)len, "%s", buf);
    }
    return ret;
}

/**
 * @brief 删除指定网络编号（连接失败回滚用；网络不存在时 wpa 返回 FAIL，忽略）
 * @param id 网络编号文本，可为空（空则直接返回）
 */
static void wpa_net_del(const char* id)
{
    char cmd[WPA_CMD_LEN] = {0}; /* 拼接后的命令 */

    if (NULL == id || '\0' == id[0])
        return;

    (void)snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %s", id);
    (void)wpa_cmd(cmd, NULL, 0);
}

/**
 * @brief 按前缀取事件文本中某个「键=值」字段的值（空格分隔）
 * @param text 事件正文，不可为空
 * @param key  键名（含等号），不可为空
 * @param out  输出缓冲，不可为空
 * @param len  输出缓冲长度
 * @return 1 取到；0 未找到
 */
static int wpa_ev_field(const char* text, const char* key, char* out, int len)
{
    const char* pos = NULL; /* 当前查找位置 */
    const char* end = NULL; /* 值结尾（下一个空格） */
    int n = 0;              /* 值长度 */

    if (NULL == text || NULL == key || NULL == out || len <= 0)
        return 0;

    pos = text;
    while (NULL != (pos = strstr(pos, key)))
    {
        /* 只接受字段起点匹配（行首或空格后），避免命中其它键的子串 */
        if (pos != text && ' ' != pos[-1])
        {
            pos += strlen(key);
            continue;
        }
        pos += strlen(key);
        end = strchr(pos, ' ');
        n = (NULL == end) ? (int)strlen(pos) : (int)(end - pos);
        if (n >= len)
            n = len - 1;
        if (n > 0)
            (void)memcpy(out, pos, (size_t)n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

/**
 * @brief 解析一条 CTRL-EVENT-* 事件文本
 * @param text 事件正文（已去掉优先级前缀），不可为空
 * @param ev   事件输出，不可为空
 * @return 1 为可上报事件；0 无法识别，调用方应忽略
 */
static int wpa_ev_parse(const char* text, NETMGR_WEV* ev)
{
    char num[WPA_ID_LEN] = {0}; /* 数值字段取值缓冲 */
    const char* name = text;    /* 事件名起点（前缀之后） */

    if (NULL == text || NULL == ev)
        return 0;

    (void)memset(ev, 0, sizeof(*ev));

    /* 事件正文形如 "CTRL-EVENT-DISCONNECTED bssid=... reason=..." */
    if (0 != strncmp(name, "CTRL-EVENT-", strlen("CTRL-EVENT-")))
        return 0;
    name += strlen("CTRL-EVENT-");

    if (0 == strncmp(name, "CONNECTED", strlen("CONNECTED")))
    {
        ev->type = NETMGR_WEV_CONNECTED;
        (void)wpa_ev_field(text, "ssid=", ev->ssid, (int)sizeof(ev->ssid));
        (void)wpa_ev_field(text, "bssid=", ev->bssid, (int)sizeof(ev->bssid));
        return 1;
    }
    if (0 == strncmp(name, "DISCONNECTED", strlen("DISCONNECTED")))
    {
        ev->type = NETMGR_WEV_DISCONNECTED;
        (void)wpa_ev_field(text, "bssid=", ev->bssid, (int)sizeof(ev->bssid));
        if (wpa_ev_field(text, "reason=", num, (int)sizeof(num)))
            ev->reason = atoi(num);
        return 1;
    }
    if (0 == strncmp(name, "SCAN-RESULTS", strlen("SCAN-RESULTS")))
    {
        ev->type = NETMGR_WEV_SCAN_DONE;
        return 1;
    }
    if (0 == strncmp(name, "ASSOC-REJECT", strlen("ASSOC-REJECT")))
    {
        ev->type = NETMGR_WEV_ASSOC_REJECT;
        (void)wpa_ev_field(text, "bssid=", ev->bssid, (int)sizeof(ev->bssid));
        if (wpa_ev_field(text, "status_code=", num, (int)sizeof(num)))
            ev->status = atoi(num);
        return 1;
    }
    if (0 == strncmp(name, "TERMINATING", strlen("TERMINATING")))
    {
        ev->type = NETMGR_WEV_TERMINATING;
        return 1;
    }
    return 0;
}

/**
 * @brief wpa 事件接收线程：阻塞收取事件并回调分发
 * @param arg 未使用
 * @return NULL
 */
static void* wpa_ev_thread(void* arg)
{
    char buf[WPA_BUF_LEN] = {0}; /* 事件原始缓冲 */
    struct msghdr msg = {0};     /* 接收消息头 */
    struct iovec iov = {0};      /* 接收区间 */
    NETMGR_WEV ev;               /* 解析后的事件 */
    const char* text = NULL;     /* 事件正文起点 */
    char* end = NULL;            /* 优先级前缀结尾 */
    ssize_t n = 0;               /* 接收字节数 */

    (void)arg;

    while (g_wpaEv.running)
    {
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf) - 1;
        (void)memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        /* 收发超时保证回环检查 running，close 时能及时退出 */
        n = recvmsg(g_wpaEv.sock, &msg, 0);
        if (n <= 0)
            continue;
        if (0 != (msg.msg_flags & MSG_TRUNC))
            continue; /* 事件均为短行，超长视为异常直接丢弃 */
        buf[n] = '\0';

        /* wpa 事件带优先级前缀 <N>，跳过后才是事件正文 */
        text = buf;
        if ('<' == text[0])
        {
            end = strchr(text, '>');
            if (NULL == end)
                continue;
            text = end + 1;
        }

        if (0 == wpa_ev_parse(text, &ev))
            continue;
        if (NULL != g_wpaEv.cb)
            (void)g_wpaEv.cb(&ev, g_wpaEv.user);
    }
    return NULL;
}

/**
 * @brief 把 wpa_state 文本映射为工作状态
 * @param text wpa_state 取值，可为空
 * @return 对应状态，无法识别时返回 NETMGR_WPA_UNKNOWN
 */
static NETMGR_WPA_STATE wpa_state_map(const char* text)
{
    if (NULL == text)
        return NETMGR_WPA_UNKNOWN;
    if (0 == strcmp(text, "SCANNING"))
        return NETMGR_WPA_SCANNING;
    if (0 == strcmp(text, "ASSOCIATING") || 0 == strcmp(text, "ASSOCIATED") ||
        0 == strcmp(text, "4WAY_HANDSHAKE") || 0 == strcmp(text, "GROUP_HANDSHAKE"))
        return NETMGR_WPA_ASSOCIATING;
    if (0 == strcmp(text, "COMPLETED"))
        return NETMGR_WPA_COMPLETED;
    if (0 == strcmp(text, "DISCONNECTED") || 0 == strcmp(text, "INACTIVE") ||
        0 == strcmp(text, "INTERFACE_DISABLED"))
        return NETMGR_WPA_DISCONNECTED;
    return NETMGR_WPA_UNKNOWN;
}

/**
 * @brief 按前缀取 STATUS 回复中某个键的值
 * @param text STATUS 回复正文，不可为空
 * @param key  键名（含等号），不可为空
 * @param out  输出缓冲，不可为空
 * @param len  输出缓冲长度
 * @return 1 取到；0 未找到
 */
static int wpa_field(const char* text, const char* key, char* out, int len)
{
    const char* pos = NULL; /* 当前查找位置 */
    const char* end = NULL; /* 行尾 */
    int n = 0;              /* 值长度 */

    if (NULL == text || NULL == key || NULL == out || len <= 0)
        return 0;

    pos = text;
    while (NULL != (pos = strstr(pos, key)))
    {
        /* 只接受行首匹配，避免命中其它键的子串 */
        if (pos != text && '\n' != pos[-1])
        {
            pos += strlen(key);
            continue;
        }
        pos += strlen(key);
        end = strchr(pos, '\n');
        n = (NULL == end) ? (int)strlen(pos) : (int)(end - pos);
        if (n >= len)
            n = len - 1;
        if (n > 0)
            (void)memcpy(out, pos, (size_t)n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

/**
 * @brief 读取并解析一次 STATUS
 * @param info  连接信息输出，可为空
 * @param state 工作状态输出，可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int wpa_read_status(NETMGR_WIFI_INFO* info, NETMGR_WPA_STATE* state)
{
    char out[WPA_BUF_LEN] = {0};        /* STATUS / SIGNAL_POLL 回复 */
    char text[WPA_ID_LEN] = {0};        /* 字段取值缓冲 */
    char ssid[NETMGR_SSID_LEN] = {0};   /* SSID 取值 */
    char bssid[NETMGR_BSSID_LEN] = {0}; /* BSSID 取值 */
    int ret = 0;                        /* 命令结果 */

    ret = wpa_cmd("STATUS", out, (int)sizeof(out));
    if (ret < 0)
        return ret;

    if (NULL != state)
    {
        *state = NETMGR_WPA_UNKNOWN;
        if (wpa_field(out, "wpa_state=", text, (int)sizeof(text)))
            *state = wpa_state_map(text);
    }

    if (NULL == info)
        return 0;

    memset(info, 0, sizeof(*info));
    if (wpa_field(out, "ssid=", ssid, (int)sizeof(ssid)))
        (void)snprintf(info->ssid, sizeof(info->ssid), "%s", ssid);
    if (wpa_field(out, "bssid=", bssid, (int)sizeof(bssid)))
        (void)snprintf(info->bssid, sizeof(info->bssid), "%s", bssid);
    if (wpa_field(out, "freq=", text, (int)sizeof(text)))
        info->freq = atoi(text);
    if (wpa_field(out, "link_speed=", text, (int)sizeof(text)))
        info->linkSpeed = atoi(text);

    info->connected = ('\0' != info->ssid[0] && '\0' != info->bssid[0]) ? 1 : 0;

    /* STATUS 不含 dBm，用 SIGNAL_POLL 的 RSSI 补齐；取不到时保持 0，不影响连接信息 */
    if (1 == info->connected &&
        0 == wpa_cmd("SIGNAL_POLL", out, (int)sizeof(out)) &&
        1 == wpa_field(out, "RSSI=", text, (int)sizeof(text)))
        info->signal = atoi(text);

    return 0;
}

/**
 * @brief 写 wpa_supplicant 最小配置（控制接口目录、调用方指定国家码）
 * @param path    配置文件输出路径，不可为空
 * @param country 国家码（如 "CN"），NULL 或空串表示不写 country 行（由 wpa 默认 regulatory 决定）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_conf_write(const char* path, const char* country)
{
    FILE* fp = NULL; /* 配置文件句柄 */

    if (NULL == path)
        return -NETMGRERR_PARAM;

    fp = fopen(path, "w");
    if (NULL == fp)
        return -NETMGRERR_FILE;

    /* 不写 update_config：网络配置由调用方每次连接下发，不做持久化，避免两处事实源 */
    if (fprintf(fp, "ctrl_interface=%s\n", WPA_DIR) < 0)
    {
        fclose(fp);
        return -NETMGRERR_FILE;
    }

    /* 国家码属调用方业务参数：未给则不写，避免库内硬编码；wpa 按系统 regulatory 配置工作 */
    if (NULL != country && '\0' != country[0] &&
        fprintf(fp, "country=%s\n", country) < 0)
    {
        fclose(fp);
        return -NETMGRERR_FILE;
    }

    if (0 != fflush(fp))
    {
        fclose(fp);
        return -NETMGRERR_FILE;
    }
    fclose(fp);
    return 0;
}

/**
 * @brief 探测指定接口的控制口是否在服务（用临时 socket，不触碰主通道）
 * @details socket 文件存在不代表可用，按三种结果区分：
 *          连不上说明无人监听（残留文件）；连上但没应答说明进程在但忙
 * @param ifname 接口名，不可为空
 * @return 1 可用（收到应答）；0 无可服务进程（无文件或 connect 失败）；
 *         -1 连接成功但未应答（进程可能在初始化）
 */
static int wpa_socket_alive(const char* ifname)
{
    struct sockaddr_un addr = {0}; /* 控制口地址 */
    struct timeval tv = {0};       /* 短超时，避免阻塞等待循环 */
    char path[WPA_PATH_LEN] = {0}; /* 控制口路径 */
    char buf[64] = {0};            /* PONG 回复 */
    int fd = -1;                   /* 临时 socket */
    int alive = 0;                 /* 探测结果 */

    if (0 != wpa_path(ifname, path, (int)sizeof(path)))
        return 0;
    if (0 != access(path, F_OK))
        return 0;

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;

    /* 探测同样需要本地地址，否则收不到 PONG */
    if (0 != wpa_bind_client(fd, "probe"))
    {
        close(fd);
        return 0;
    }

    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    tv.tv_usec = WPA_PROBE_TIMEOUT_MS * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));

    if (0 != connect(fd, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)))
    {
        close(fd);
        wpa_unbind_client("probe");
        return 0; /* 连不上：无人监听，属残留文件 */
    }

    /* 连得上：再有应答才算可用；没应答视为“在但忙”，不可当残留删除 */
    if (0 < send(fd, "PING", 4, 0) && 0 < recv(fd, buf, sizeof(buf) - 1, 0))
        alive = 1;
    else
        alive = -1;
    close(fd);
    wpa_unbind_client("probe");
    return alive;
}

/**
 * @brief 清理残留的控制口文件（上次实例被强制结束时可能留下）
 * @details 控制口仍被服务则保留；连不上说明是残留文件，删除后新实例才能 bind
 * @param ifname 接口名，不可为空
 */
static void wpa_cleanup_socket(const char* ifname)
{
    char path[WPA_PATH_LEN] = {0}; /* 控制口路径 */

    if (0 != wpa_path(ifname, path, (int)sizeof(path)))
        return;
    if (0 != access(path, F_OK))
        return;
    /* 只有“连不上”才判定为残留；连得上但忙（-1）时保留，避免误删在服务的接口 */
    if (0 != wpa_socket_alive(ifname))
        return;

    (void)unlink(path);
}

/**
 * @brief 等待 wpa_supplicant 控制接口出现
 * @param ifname    接口名，不可为空
 * @param timeoutMs 等待超时（毫秒），0 表示使用默认超时
 * @return 0 就绪；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_wait(const char* ifname, int timeoutMs)
{
    char path[WPA_PATH_LEN] = {0}; /* 控制接口路径 */
    long long deadline = 0;        /* 等待截止时刻（单调时钟毫秒） */
    int ret = 0;                   /* 路径拼装结果 */

    if (NULL == ifname)
        return -NETMGRERR_PARAM;

    ret = wpa_path(ifname, path, (int)sizeof(path));
    if (ret < 0)
        return ret;

    if (timeoutMs <= 0)
        timeoutMs = WPA_OPEN_MS;

    /* 按挂钟计时：每轮探测本身要花时间，按轮次会显著压缩真实等待窗口 */
    deadline = wpa_now_ms() + timeoutMs;
    while (wpa_now_ms() < deadline)
    {
        /* 文件存在还不够：必须能连上并应答，避免把残留 socket 当成就绪 */
        if (1 == wpa_socket_alive(ifname))
            return 0;
        usleep(WPA_POLL_US);
    }
    return -NETMGRERR_TIMEOUT;
}

/**
 * @brief 建立控制通道（已连接同一接口时直接返回成功）
 * @param ifname 接口名，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_open(const char* ifname)
{
    struct sockaddr_un addr = {0}; /* 控制接口地址 */
    struct timeval tv = {0};       /* 收发超时 */
    char path[WPA_PATH_LEN] = {0}; /* 控制接口路径 */
    int fd = -1;                   /* 控制 socket */
    int ret = 0;                   /* 子步骤返回值 */

    if (NULL == ifname)
        return -NETMGRERR_PARAM;

    ret = wpa_path(ifname, path, (int)sizeof(path));
    if (ret < 0)
        return ret;

    pthread_mutex_lock(&g_wpa.lock);
    if (g_wpa.sock >= 0 && 0 == strcmp(g_wpa.ifname, ifname))
    {
        pthread_mutex_unlock(&g_wpa.lock);
        return 0;
    }
    if (g_wpa.sock >= 0)
    {
        /* 事件通道与接口绑定：换接口时先停事件监听，避免线程读旧接口 */
        pthread_mutex_unlock(&g_wpa.lock);
        (void)netmgr_wpa_event_close();
        pthread_mutex_lock(&g_wpa.lock);

        close(g_wpa.sock);
        g_wpa.sock = -1;
        wpa_unbind_client("cmd");
        g_wpa.ifname[0] = '\0';
    }

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        pthread_mutex_unlock(&g_wpa.lock);
        return -NETMGRERR_SOCKET;
    }

    /* 绑定本端地址：不绑定则 wpa 无法把回包送回来，命令会全部超时 */
    if (0 != wpa_bind_client(fd, "cmd"))
    {
        close(fd);
        pthread_mutex_unlock(&g_wpa.lock);
        return -NETMGRERR_SOCKET;
    }

    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (0 != connect(fd, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)))
    {
        close(fd);
        wpa_unbind_client("cmd");
        pthread_mutex_unlock(&g_wpa.lock);
        return -NETMGRERR_WPA;
    }

    /* 收发超时避免 wpa 无响应时调用线程被永久阻塞 */
    tv.tv_sec = WPA_SOCK_TIMEOUT_S;
    tv.tv_usec = 0;
    if (0 != setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ||
        0 != setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)))
    {
        close(fd);
        pthread_mutex_unlock(&g_wpa.lock);
        return -NETMGRERR_SOCKET;
    }

    g_wpa.sock = fd;
    (void)snprintf(g_wpa.ifname, sizeof(g_wpa.ifname), "%s", ifname);
    pthread_mutex_unlock(&g_wpa.lock);
    return 0;
}

/**
 * @brief 查询控制通道是否已打开
 * @return 1 已打开；0 未打开
 */
int netmgr_wpa_is_open(void)
{
    int isOpen = 0; /* 查询结果 */

    pthread_mutex_lock(&g_wpa.lock);
    isOpen = (g_wpa.sock >= 0) ? 1 : 0;
    pthread_mutex_unlock(&g_wpa.lock);
    return isOpen;
}

/**
 * @brief 关闭控制通道（同时停止事件监听；未打开时直接返回）
 */
void netmgr_wpa_close(void)
{
    /* 事件通道与命令通道同接口：先停事件线程，再关命令通道 */
    (void)netmgr_wpa_event_close();

    pthread_mutex_lock(&g_wpa.lock);
    if (g_wpa.sock >= 0)
    {
        close(g_wpa.sock);
        g_wpa.sock = -1;
        wpa_unbind_client("cmd");
    }
    g_wpa.ifname[0] = '\0';
    pthread_mutex_unlock(&g_wpa.lock);
}

/**
 * @brief 连接指定热点（配置网络、使能、选中并等待关联完成）
 * @param ssid      热点名称，不可为空
 * @param passwd    密码，空串表示开放网络
 * @param timeoutMs 关联完成等待超时（毫秒），0 表示使用默认超时
 * @return 0 已关联；负值为 NETMGRERR_* 错误码（通道未打开返回 -NETMGRERR_STATE）
 */
int netmgr_wpa_connect(const char* ssid, const char* passwd, int timeoutMs)
{
    char id[WPA_ID_LEN] = {0};   /* wpa 分配的网络编号 */
    char cmd[WPA_CMD_LEN] = {0}; /* 拼接后的命令 */
    char out[WPA_BUF_LEN] = {0}; /* STATUS 回复 */
    int times = WPA_CONN_MAX;    /* 等待预算（轮次 × 轮询间隔 = 毫秒） */
    int ret = 0;                 /* 子步骤返回值 */
    long long deadline = 0;      /* 等待截止时刻（单调时钟毫秒） */

    if (NULL == ssid || '\0' == ssid[0])
        return -NETMGRERR_PARAM;
    if (NULL != passwd && '\0' != passwd[0] && (int)strlen(passwd) < WPA_PSK_MIN)
        return -NETMGRERR_CONF;

    /* 清理旧网络属幂等操作，无旧网络时 wpa 返回 FAIL，不作为异常 */
    (void)wpa_cmd("REMOVE_NETWORK all", NULL, 0);

    ret = wpa_cmd("ADD_NETWORK", id, (int)sizeof(id));
    if (ret < 0)
        return ret;
    id[strcspn(id, "\r\n")] = '\0';

    /* 网络已建：以下任一步失败都删除本次新增的网络后上抛，避免半配置残留 */
    (void)snprintf(cmd, sizeof(cmd), "SET_NETWORK %s ssid \"%s\"", id, ssid);
    ret = wpa_cmd(cmd, NULL, 0);
    if (ret < 0)
        goto clean;

    if (NULL != passwd && '\0' != passwd[0])
        (void)snprintf(cmd, sizeof(cmd), "SET_NETWORK %s psk \"%s\"", id, passwd);
    else
        (void)snprintf(cmd, sizeof(cmd), "SET_NETWORK %s key_mgmt NONE", id);
    ret = wpa_cmd(cmd, NULL, 0);
    if (ret < 0)
        goto clean;

    (void)snprintf(cmd, sizeof(cmd), "ENABLE_NETWORK %s", id);
    ret = wpa_cmd(cmd, NULL, 0);
    if (ret < 0)
        goto clean;

    (void)snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %s", id);
    ret = wpa_cmd(cmd, NULL, 0);
    if (ret < 0)
        goto clean;

    if (timeoutMs > 0)
        times = timeoutMs * 1000 / WPA_POLL_US;
    if (times <= 0)
        times = 1;

    /* 超时按挂钟计时：单次 STATUS 可能耗到收发超时（3 秒），
       若按“轮次 × 间隔”计时，10 秒预算会被放大到数分钟 */
    deadline = wpa_now_ms() + (long long)times * (WPA_POLL_US / 1000);
    while (wpa_now_ms() < deadline)
    {
        out[0] = '\0';
        if (0 == wpa_cmd("STATUS", out, (int)sizeof(out)) &&
            NULL != strstr(out, "wpa_state=COMPLETED"))
        {
            return 0;
        }
        usleep(WPA_POLL_US);
    }
    ret = -NETMGRERR_TIMEOUT;

clean:
    /* 未关联成功即回收网络配置，避免残留网络被 wpa 反复尝试 */
    wpa_net_del(id);
    return ret;
}

/**
 * @brief 断开当前热点并清空 wpa 内网络配置
 * @return 0 成功（wpa 未连接热点时同样返回 0）；负值为 NETMGRERR_* 错误码
 *         （通道未打开返回 -NETMGRERR_STATE）
 */
int netmgr_wpa_disconnect(void)
{
    /* 仅 DISCONNECT 时 wpa 仍持有网络配置会自行重连，故断开后清空全部网络 */
    (void)wpa_cmd("DISCONNECT", NULL, 0);
    return wpa_cmd("REMOVE_NETWORK all", NULL, 0);
}

/**
 * @brief 读取当前工作状态
 * @param state 输出状态，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码（通道未打开返回 -NETMGRERR_STATE）
 */
int netmgr_wpa_state(NETMGR_WPA_STATE* state)
{
    if (NULL == state)
        return -NETMGRERR_PARAM;

    return wpa_read_status(NULL, state);
}

/**
 * @brief 读取当前连接信息
 * @param info 输出连接信息，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码（通道未打开返回 -NETMGRERR_STATE）
 */
int netmgr_wpa_status(NETMGR_WIFI_INFO* info)
{
    if (NULL == info)
        return -NETMGRERR_PARAM;

    return wpa_read_status(info, NULL);
}

/**
 * @brief 触发一次扫描并等待扫描结束
 * @param timeoutMs 扫描结束等待超时（毫秒），0 表示使用默认超时
 * @return 0 扫描完成；负值为 NETMGRERR_* 错误码（超时返回 -NETMGRERR_TIMEOUT，
 *         此时扫描仍在后台进行、结果仍可读取）
 */
int netmgr_wpa_scan(int timeoutMs)
{
    char out[WPA_BUF_LEN] = {0}; /* STATUS 回复 */
    int times = WPA_SCAN_MAX;    /* 等待预算（轮次 × 轮询间隔 = 毫秒） */
    int ret = 0;                 /* 子步骤返回值 */
    long long deadline = 0;      /* 等待截止时刻（单调时钟毫秒） */

    ret = wpa_cmd("SCAN", NULL, 0);
    if (ret < 0)
        return ret;

    if (timeoutMs > 0)
        times = timeoutMs * 1000 / WPA_POLL_US;
    if (times <= 0)
        times = 1;

    /* 扫描期间 wpa_state 为 SCANNING，离开该状态即认为本轮结束。
       超时同样按挂钟计时，避免单次 STATUS 收发超时把预算放大 */
    deadline = wpa_now_ms() + (long long)times * (WPA_POLL_US / 1000);
    while (wpa_now_ms() < deadline)
    {
        out[0] = '\0';
        if (0 == wpa_cmd("STATUS", out, (int)sizeof(out)) &&
            NULL == strstr(out, "wpa_state=SCANNING"))
        {
            return 0;
        }
        usleep(WPA_POLL_US);
    }
    return -NETMGRERR_TIMEOUT;
}

/**
 * @brief 解析 SCAN_RESULTS 的一行
 * @param line 一行文本（制表符分隔：BSSID、频率、信号、标志、SSID），不可为空
 * @param ap   输出接入点，不可为空
 * @return 1 解析成功；0 该行无效
 */
static int wpa_parse_ap(const char* line, NETMGR_WIFI_AP* ap)
{
    char work[WPA_BUF_LEN] = {0}; /* 行副本，供分段解析 */
    char* field[5] = {NULL};      /* 五个字段的起点 */
    char* pos = NULL;             /* 当前解析位置 */
    int i = 0;                    /* 字段下标，从 0 开始 */

    if (NULL == line || NULL == ap)
        return 0;

    (void)snprintf(work, sizeof(work), "%s", line);
    work[strcspn(work, "\r\n")] = '\0';

    pos = work;
    for (i = 0; i < 5; ++i)
    {
        field[i] = pos;
        pos = strchr(pos, '\t');
        if (NULL == pos)
        {
            /* 末段允许没有制表符收尾 */
            if (4 == i)
                break;
            return 0;
        }
        *pos = '\0';
        ++pos;
    }

    if (NULL == field[0] || NULL == field[1] || NULL == field[2])
        return 0;

    (void)snprintf(ap->bssid, sizeof(ap->bssid), "%s", field[0]);
    ap->freq = atoi(field[1]);
    ap->signal = atoi(field[2]);

    /* 标志串里 WPA（WPA/WPA2-PSK 等）、RSN（WPA2/WPA3 企业或 SAE）、WEP 均属加密 */
    ap->secure = 0;
    if (NULL != field[3] &&
        (NULL != strstr(field[3], "WPA") || NULL != strstr(field[3], "RSN") ||
         NULL != strstr(field[3], "WEP")))
        ap->secure = 1;

    if (NULL != field[4])
        (void)snprintf(ap->ssid, sizeof(ap->ssid), "%s", field[4]);
    return 1;
}

/**
 * @brief 读取最近一次扫描结果
 * @param list 输出数组，容量由 *num 传入
 * @param num  入参为数组容量，出参为实际写入条数
 * @return 0 成功；负值为 NETMGRERR_* 错误码（结果超出缓冲返回 -NETMGRERR_BUFF）
 */
int netmgr_wpa_scan_result(NETMGR_WIFI_AP* list, int* num)
{
    char* out = NULL;   /* SCAN_RESULTS 回复（AP 多时超过 1KB，用堆缓冲） */
    char* line = NULL;  /* 当前行 */
    char* save = NULL;  /* 分行游标 */
    int cap = 0;        /* 调用方给的容量 */
    int count = 0;      /* 已填充条数 */
    int ret = 0;        /* 子步骤返回值 */

    if (NULL == list || NULL == num || *num <= 0)
        return -NETMGRERR_PARAM;

    cap = (*num < NETMGR_SCAN_MAX) ? *num : NETMGR_SCAN_MAX;
    memset(list, 0, (size_t)cap * sizeof(list[0]));

    out = (char*)malloc((size_t)WPA_RSP_MAX);
    if (NULL == out)
        return -NETMGRERR_MEM;

    pthread_mutex_lock(&g_wpa.lock);
    ret = wpa_rsp(g_wpa.sock, "SCAN_RESULTS", out, WPA_RSP_MAX);
    pthread_mutex_unlock(&g_wpa.lock);
    if (ret < 0)
    {
        free(out);
        return ret; /* 超长时报 -NETMGRERR_BUFF，由调用方决定是否加大缓冲 */
    }

    /* 首行为表头（bssid / frequency / signal level / flags / ssid），跳过 */
    line = strtok_r(out, "\n", &save);
    if (NULL == line)
    {
        free(out);
        *num = 0;
        return 0;
    }

    while (NULL != (line = strtok_r(NULL, "\n", &save)))
    {
        if (count >= cap)
            break;
        if (0 == wpa_parse_ap(line, &list[count]))
            continue;
        ++count;
    }

    free(out);
    out = NULL;
    *num = count;
    return 0;
}

/**
 * @brief 启动 wpa 事件监听（须先 netmgr_wpa_open；重复调用直接返回成功）
 * @param cb   事件回调，可为空（只订阅不关心内容时）
 * @param user 回调透传的用户指针，可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码（通道未打开返回 -NETMGRERR_STATE）
 */
int netmgr_wpa_event_open(netmgr_wev_cb cb, void* user)
{
    struct sockaddr_un addr = {0}; /* 事件监听地址 */
    struct timeval tv = {0};       /* 接收超时，供回环检查运行标志 */
    char ifname[NETMGR_IF_LEN] = {0}; /* 当前绑定接口名 */
    char path[WPA_PATH_LEN] = {0};    /* 事件监听路径 */
    char buf[WPA_BUF_LEN] = {0};      /* ATTACH 回复 */
    int fd = -1;                   /* 事件监听 socket */
    int ret = 0;                   /* 子步骤返回值 */

    /* 事件通道与命令通道同接口：命令通道未建立时无法确定监听路径 */
    pthread_mutex_lock(&g_wpa.lock);
    if (g_wpa.sock < 0 || '\0' == g_wpa.ifname[0])
    {
        pthread_mutex_unlock(&g_wpa.lock);
        return -NETMGRERR_STATE;
    }
    (void)snprintf(ifname, sizeof(ifname), "%s", g_wpa.ifname);
    pthread_mutex_unlock(&g_wpa.lock);

    ret = wpa_path(ifname, path, (int)sizeof(path));
    if (ret < 0)
        return ret;

    pthread_mutex_lock(&g_wpaEv.lock);
    if (g_wpaEv.sock >= 0)
    {
        pthread_mutex_unlock(&g_wpaEv.lock);
        return 0;
    }

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        pthread_mutex_unlock(&g_wpaEv.lock);
        return -NETMGRERR_SOCKET;
    }

    /* 事件是服务端主动推送，客户端必须有地址才收得到 */
    if (0 != wpa_bind_client(fd, "ev"))
    {
        close(fd);
        pthread_mutex_unlock(&g_wpaEv.lock);
        return -NETMGRERR_SOCKET;
    }

    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (0 != connect(fd, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)))
    {
        close(fd);
        wpa_unbind_client("ev");
        pthread_mutex_unlock(&g_wpaEv.lock);
        return -NETMGRERR_WPA;
    }

    /* 接收超时供线程回环检查运行标志；发送走同一超时约束 */
    tv.tv_sec = WPA_EV_TIMEOUT_MS / 1000;
    tv.tv_usec = (WPA_EV_TIMEOUT_MS % 1000) * 1000;
    if (0 != setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ||
        0 != setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)))
    {
        close(fd);
        pthread_mutex_unlock(&g_wpaEv.lock);
        return -NETMGRERR_SOCKET;
    }

    /* ATTACH 后 wpa 才向本连接推送 CTRL-EVENT-* 事件 */
    ret = wpa_rsp(fd, "ATTACH", buf, (int)sizeof(buf));
    if (ret < 0)
    {
        close(fd);
        pthread_mutex_unlock(&g_wpaEv.lock);
        return ret;
    }

    g_wpaEv.sock = fd;
    g_wpaEv.cb = cb;
    g_wpaEv.user = user;
    g_wpaEv.running = 1;
    ret = pthread_create(&g_wpaEv.thread, NULL, wpa_ev_thread, NULL);
    if (0 != ret)
    {
        g_wpaEv.sock = -1;
        g_wpaEv.cb = NULL;
        g_wpaEv.user = NULL;
        g_wpaEv.running = 0;
        close(fd);
        pthread_mutex_unlock(&g_wpaEv.lock);
        return -NETMGRERR_THREAD;
    }
    pthread_mutex_unlock(&g_wpaEv.lock);
    return 0;
}

/**
 * @brief 停止 wpa 事件监听并回收接收线程（未启动时同样返回 0）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_event_close(void)
{
    char buf[WPA_BUF_LEN] = {0}; /* DETACH 回复 */

    pthread_mutex_lock(&g_wpaEv.lock);
    if (g_wpaEv.sock < 0)
    {
        pthread_mutex_unlock(&g_wpaEv.lock);
        return 0;
    }

    /* 接收线程不持本锁，先停线程再注销订阅，避免注销期间还在收事件 */
    g_wpaEv.running = 0;
    pthread_join(g_wpaEv.thread, NULL);

    (void)wpa_rsp(g_wpaEv.sock, "DETACH", buf, (int)sizeof(buf));
    close(g_wpaEv.sock);
    g_wpaEv.sock = -1;
    wpa_unbind_client("ev");
    g_wpaEv.cb = NULL;
    g_wpaEv.user = NULL;
    pthread_mutex_unlock(&g_wpaEv.lock);
    return 0;
}

/**
 * @brief 拉起 wpa_supplicant 并打开控制通道
 * @param ifname    接口名，不可为空
 * @param confPath  配置文件输出路径，不可为空
 * @param country   国家码，可为空
 * @param timeoutMs 等待控制接口的超时（毫秒），0 表示默认
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_start(const char* ifname, const char* confPath, const char* country, int timeoutMs)
{
    int ret = 0; /* 子步骤返回值 */
    int i = 0;   /* 通道探测轮次，从 0 开始 */

    if (NULL == ifname || '\0' == ifname[0] || NULL == confPath || '\0' == confPath[0])
        return -NETMGRERR_PARAM;

    /* 已建立通道时先完整停止，保证后续步骤按新参数重建 */
    if (0 != netmgr_wpa_is_open())
        (void)netmgr_wpa_stop();

    /* 第 1 步：独占启动，结束残留实例，避免沿用未知配置 */
    ret = netmgr_proc_kill(WPA_PROC_NAME);
    if (ret < 0)
        return ret;

    /* 第 1.5 步：清理残留控制口，避免新实例 bind 时 EADDRINUSE 直接退出 */
    wpa_cleanup_socket(ifname);

    /* 第 2 步：写最小配置（控制接口目录 + 可选国家码） */
    ret = netmgr_wpa_conf_write(confPath, country);
    if (ret < 0)
        return ret;

    /* 第 3 步：后台拉起。用 spawn 而非 system：system 会等命令结束，而
       wpa_supplicant 的 -B 守护化时序不保证快速退出，调用线程会被拖住；
       spawn 由 common 层双 fork 脱离会话，并重定向其输出，立即返回 */
    {
        char* argv[6] = {0}; /* wpa_supplicant 参数表，末尾 NULL 结尾 */

        argv[0] = (char*)"wpa_supplicant";
        argv[1] = (char*)"-i";
        argv[2] = (char*)ifname;
        argv[3] = (char*)"-c";
        argv[4] = (char*)confPath;
        ret = netmgr_proc_spawn("wpa_supplicant", argv);
    }
    if (ret < 0)
        return -NETMGRERR_PROC;

    /* 第 4 步：等控制接口出现并建立通道；失败时回收进程，不留孤儿 */
    ret = netmgr_wpa_wait(ifname, timeoutMs);
    if (ret < 0)
    {
        (void)netmgr_proc_kill(WPA_PROC_NAME);
        return ret;
    }

    ret = netmgr_wpa_open(ifname);
    if (ret < 0)
    {
        /* 控制口文件存在但连不上（无人监听/被替换）：与“连上但不应答”区分上报 */
        (void)netmgr_proc_kill(WPA_PROC_NAME);
        return -NETMGRERR_SOCKET;
    }

    /* 第 5 步：确认控制通道真的可用（PING/PONG）。socket 文件存在不代表能收发：
       进程未就绪、接口未被接管、控制口被其它实例替换时，文件仍在但命令全失败；
       提前判失败并回收进程，避免上层拿到“假成功”后逐个命令报 -NETMGRERR_WPA */
    for (i = 0; i < WPA_PROBE_MAX; ++i)
    {
        if (0 == wpa_probe())
            return 0;
        usleep(WPA_POLL_US);
    }

    (void)netmgr_wpa_close();
    (void)netmgr_proc_kill(WPA_PROC_NAME);
    return -NETMGRERR_TIMEOUT;
}

/**
 * @brief 断开连接、关闭控制通道并结束 wpa_supplicant 进程
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wpa_stop(void)
{
    int err = 0; /* 首个错误码，0 表示尚无失败 */
    int ret = 0; /* 子步骤返回值 */

    /* 第 1 步：先经通道断开并清空网络配置，避免进程被结束前 wpa 自动重连 */
    if (0 != netmgr_wpa_is_open())
    {
        ret = netmgr_wpa_disconnect();
        if (ret < 0)
            err = ret;
        netmgr_wpa_close();
    }

    /* 第 2 步：结束进程；未运行时同样视为成功 */
    ret = netmgr_proc_kill(WPA_PROC_NAME);
    if (ret < 0 && 0 == err)
        err = ret;
    return err;
}
