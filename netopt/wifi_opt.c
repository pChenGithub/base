#include "wifi_opt.h"
#include "net_errno.h"
#include "file_opt.h"
#if 0
#include "global.h"
#else
#define PROJECT_DIR_CONFIG "/home/rockchip"
#endif
#include "wpas/wpa_ctrl.h"
#include "net_opt.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define WPA_SUPPLICANT_CONF PROJECT_DIR_CONFIG"/wpa.conf"   // WIFI 配置文件
#define WPA_CONNECT_FILE    "/var/run/wpa_supplicant"
#define WIFI_AP_CONF        PROJECT_DIR_CONFIG"/ap.conf"    // ap 配置文件
#define UDHCPD_CONF         PROJECT_DIR_CONFIG"/dhcpd.conf" // dhcp 配置文件
//
#define PROC_IS_EXIST   0   // 进程存在
//
#define WIFI_IFACE_NODE "wlan0" // WIFI 节点
//
typedef enum {
    WIFI_EVT_CONNECTING = 0,        // wifi连接中
    WIFI_EVT_CONNECTED,                 // wifi连接成功
    WIFI_EVT_DISCONNECTED,          // wifi连接断开
    WIFI_EVT_PASSWORD_ERROR,    // 密码错误
    WIFI_EVT_SCAN_COMPLETED,    // 扫描成功
    WIFI_EVT_SCAN_FAILED,               // 扫描失败
} WIFI_EVT_TYPE;
typedef int (*WIFI_EV_HANDLE)(WIFI_EVT_TYPE ev, void* arg);
typedef struct {
    struct wpa_ctrl* ctrl_conn;     // 命令接口
    char reply[4096];               // 回复消息
    struct wpa_ctrl* monitor_conn;  // 监听接口
    pthread_t monitor_thread;           // 监听线程
    WIFI_EV_HANDLE func;            // 
} WIFI_NODE; // 单个wifi节点
static WIFI_NODE node_wlan0;
// 统一使用 ctrl_conn 来发送命令，考虑加锁
static int wpa_cmd(WIFI_NODE* node, const char *cmd) {
    int ret = 0;
    if (NULL==node)
        return -NETERR_CHECK_PARAM;
    if (NULL==node->ctrl_conn)
        return -NETERR_WPA_NO_CTRL;
    size_t len = sizeof(node->reply);
    ret = wpa_ctrl_request(node->ctrl_conn, cmd, strlen(cmd), node->reply, &len, NULL);
    if (-2==ret)
        return -NETERR_CLI_CMD_TIMEOUT;
    if (ret<0 || 0==strncmp(node->reply, "FAIL", 4))
        return -NETERR_CLI_CMD_ERR;

    // 处理回复消息
    return 0;
}
// 解析wifi扫描结果
static int parse_scan_result(WIFI_NODE* node) {
    int ret = wpa_cmd(node, "SCAN_RESULTS");
    if (ret<0)
        return ret;
    // 返回内容在 node->reply 中
    node->func(WIFI_EVT_SCAN_COMPLETED, NULL);
    return 0;
}
// 处理wifi消息线程
static void* wifi_monitor_thread(void* arg) {
    char buf[2048] = {0};
    // arg 必不为NULL
    int ret = 0;
    WIFI_NODE* node = (WIFI_NODE*)arg;
    int fd = wpa_ctrl_get_fd(node->monitor_conn);
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        ret = select(fd + 1, &rfds, NULL, NULL, NULL);
        if (ret<0) {
            // 错误，退出线程
            printf("select error: %s\n", strerror(errno));
            break;
        }
        // 不是fd可读，返回
        if (!FD_ISSET(fd, &rfds)) continue;
        // 读取消息
        size_t len  = sizeof(buf);
        if (wpa_ctrl_recv(node->monitor_conn, buf, &len) < 0)
            continue;

        // 事件处理，调用回调...
        if (strstr(buf, "SCAN_COMPLETED"))
            parse_scan_result(node);    // 解析扫描结果
        else if (strstr(buf, "CONNECTING"))
            node->func(WIFI_EVT_CONNECTING, NULL);
        else if (strstr(buf, "CONNECTED"))
            node->func(WIFI_EVT_CONNECTED, NULL);
        else if (strstr(buf, "DISCONNECTED"))
            node->func(WIFI_EVT_DISCONNECTED, NULL);
        else if (strstr(buf, "AUTH_FAILED"))
            node->func(WIFI_EVT_PASSWORD_ERROR, NULL);
    }
    return NULL;
}
// 建立和wpa的通信
static int connect_to_wpa(WIFI_NODE* node, const char* path) {
    int ret = 0;
    if (NULL==node || NULL==path)
        return -NETERR_CHECK_PARAM;
    printf("通信接口文件 %s\n", path);
    node->ctrl_conn = wpa_ctrl_open(path);
    if (NULL==node->ctrl_conn) {
        return -NETERR_WPA_CONNECT_FAIL;
    }
    // 创建监听通信节点
    node->monitor_conn = wpa_ctrl_open(path);
    if (NULL==node->monitor_conn) {
        ret = -NETERR_WPA_CONNECT_FAIL;
        goto ctrl_close;
    }
    // 绑定监听
    if (0!=wpa_ctrl_attach(node->monitor_conn)) {
        ret = -NETERR_WPA_ATTACH_FAIL;
        goto monitor_close;
    }
    // 创建线程监听wifi状态
    if (0 != pthread_create(&node->monitor_thread, NULL, wifi_monitor_thread, node)) {
        ret = -NETERR_PTHREADCREATE_FAIL;
        goto monitor_detach;
    }

    // 正确返回
    return 0;

monitor_detach:
    wpa_ctrl_detach(node->monitor_conn);
monitor_close:
    wpa_ctrl_close(node->monitor_conn);
    node->monitor_conn= NULL;
ctrl_close:
    wpa_ctrl_close(node->ctrl_conn);
    node->ctrl_conn= NULL;

    return ret;
}

static int disconnect_to_wpa(WIFI_NODE* node) {
    if (NULL==node)
        return -NETERR_CHECK_PARAM;
    // 取消线程
    pthread_cancel(node->monitor_thread);
    // 等待线程
    pthread_join(node->monitor_thread, NULL);
    wpa_ctrl_detach(node->monitor_conn);
    wpa_ctrl_close(node->monitor_conn);
    wpa_ctrl_close(node->ctrl_conn);
    return 0;
}

// 判断进程是否存在，返回0表示存在，或者返回错误码
static int proc_is_run(const char *process) {
    if (NULL==process)
        return -NETERR_CHECK_PARAM;
    FILE *fp = NULL;
    int count = 0;
    char cmd[64] = {0};
    /*
     * 命令
     * wc命令是Linux中用于统计文件的字节数、单词数和行数的工具
     * -c 或 --bytes 或 --chars：只显示字节数
     * -l 或 --lines：显示行数
     * -w 或 --words：只显示单词数。
     * -m：显示字符数。
     * -L 或 --max-line-length：显示最长行的长度。
     */
    snprintf(cmd, sizeof(cmd), "ps -ef|grep %s|grep -v grep|wc -l", process);
    fp = popen(cmd, "r");
    if (fp == NULL) {
        return -NETERR_FOPEN_FAIL;
    }

    if ((fgets(cmd, sizeof(cmd), fp)) != NULL)
        count = atoi(cmd);
    pclose(fp);
    // 判断是否有行数
    if (0==count)
        return -NETERR_PRO_NOT_EXIST;
    return PROC_IS_EXIST;
}

// 启动进程
static int proc_run(const char* cmd) {
    if (NULL==cmd)
        return -NETERR_CHECK_PARAM;
    //printf("cmd: %s\n", cmd);
    system(cmd);
    return 0;
}
// 关闭进程
static void proc_kill(const char* name) {
    if (NULL==name)
        return ;
    char cmd[32] = "killall ";
    strcat(cmd, name);
    system(cmd);
}

// 创建最简单的 wpa 配置文件
static int wpa_conf_simple(const char* file)
{
    if (NULL==file)
        return -NETERR_CHECK_PARAM;
    // 创建 hostapd 配置文件，如果有文件，覆盖
    FILE* fp = fopen(WPA_SUPPLICANT_CONF, "w+");
    if (NULL==fp) {
        return -NETERR_FOPEN_FAIL;
    }
    //
    fputs("update_config=1\n", fp);
    fputs("ctrl_interface="WPA_CONNECT_FILE"\n", fp);
    fputs("country=CN\n", fp);
    // 关闭文件
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    fp = NULL;
    //
    return 0;
}

int wifi_sta_enable() {
    // 判断是否存在进程
    if (PROC_IS_EXIST==proc_is_run("wpa_supplicant")) {
        proc_kill("wpa_supplicant");
    }
    // 判断当前是否是ap模式
    if (PROC_IS_EXIST==proc_is_run("hostapd")) {
        // 关闭
        system("killall hostapd");
    }
    if (PROC_IS_EXIST==proc_is_run("udhcpd")) {
        // 关闭
        system("killall udhcpd");
    }
    // 检查是否存在 wpa_supplicant 配置文件
    if (0!=file_exist(WPA_SUPPLICANT_CONF)) {
        // 文件不存在，创建一个新的最小的文件
        if (wpa_conf_simple(WPA_SUPPLICANT_CONF)<0)
            return -NETERR_WPA_CONF_NONE;
    }
    usleep(200000);
    // 启动wpa进程，有可能优化
    proc_run("wpa_supplicant -Dwext -i"WIFI_IFACE_NODE" -c "WPA_SUPPLICANT_CONF" -B");
    // 启动完wpa进程之后，连接wpa通信
    //printf("xxxxxxxxxxxxxxxxx %d\n", __LINE__);
    int ret = connect_to_wpa(&node_wlan0, WPA_CONNECT_FILE"/"WIFI_IFACE_NODE);
    if (ret<0) {
        return ret;
    }
    // 启动线程监听wpa

    return 0;
}

int wifi_sta_disable() {
    // 判断是否存在进程
    if (PROC_IS_EXIST!=proc_is_run("wpa_supplicant")) {
        return -NETERR_WPA_NOT_RUN;
    }
    // 关闭监听线程
    // 关闭wpa通信
    disconnect_to_wpa(&node_wlan0);
    // 关闭进程 wpa_supplicant
    proc_kill("wpa_supplicant");
    return 0;
}

int wifi_sta_scan()
{
    size_t reply_len = 0;
    if (NULL==node_wlan0.ctrl_conn)
        return -NETERR_CHECK_PARAM;
    int ret = wpa_ctrl_request(node_wlan0.ctrl_conn, "SCAN", strlen("SCAN"),
                               node_wlan0.reply, &reply_len, NULL);
    printf("<==[%s][%d] 返回内容 %s，长度 %d\n", __func__, __LINE__, node_wlan0.reply, reply_len);
    if (-2==ret) {
        // 超时
        return -NETERR_CLI_CMD_TIMEOUT;
    } else if (ret<0 || 0==strncmp(node_wlan0.reply, "FAIL", 4)) {
        // 失败
        return -NETERR_CLI_CMD_ERR;
    }
    // 判断返回结果
    if (0!=strncmp(node_wlan0.reply, "OK", 2)) {
        // 命令发送没有成功
        return -NETERR_CLI_CMD_ERR;
    }
#if 0
    if (strncmp(cmd, "PING", 4) == 0)
        node_wlan0.reply[reply_len] = '\0';
#endif
    // 发送 scan 完成
    // 发送 scan_result
    ret = wpa_ctrl_request(node_wlan0.ctrl_conn, "SCAN_RESULT", strlen("SCAN_RESULT"),
                               node_wlan0.reply, &reply_len, NULL);
    //printf("<==[%s][%d] %s\n", __func__, __LINE__, node_wlan0.reply);
    if (-2==ret) {
        // 超时
        return -NETERR_CLI_CMD_TIMEOUT;
    } else if (ret<0 || 0==strncmp(node_wlan0.reply, "FAIL", 4)) {
        // 失败
        return -NETERR_CLI_CMD_ERR;
    }

    return 0;
}

int wifi_ap_enable(WIFI_AP_CONFIG *conf)
{
    int ret = 0;
    // 判断是否存在进程
    if (PROC_IS_EXIST==proc_is_run("hostapd")) {
        system("killall hostapd");
    }
    if (PROC_IS_EXIST==proc_is_run("udhcpd")) {
        system("killall udhcpd");
    }
    // 检查 wpa_supplicant，关闭
    if (PROC_IS_EXIST==proc_is_run("wpa_supplicant")) {
        // 关闭
        system("killall wpa_supplicant");
    }
    // 如果指定的配置，按照配置创建配置文件
    if (NULL!=conf) {
        // 创建 hostapd 配置文件，如果有文件，覆盖
        FILE* fp = fopen(WIFI_AP_CONF, "w+");
        if (NULL==fp) {
            return -NETERR_FOPEN_FAIL;
        }
        // 写入内容
        fputs("interface="WIFI_IFACE_NODE"\n", fp);
        fputs("driver=nl80211\n", fp);
        fputs("ieee80211n=1\n", fp);
        fputs("hw_mode=g\n", fp);
        fputs("channel=6\n", fp);
        fputs("ignore_broadcast_ssid=0\n", fp);
        // ssid=815_lab
        fprintf(fp, "ssid=%s\n", conf->ap.ssid);
        switch (conf->ap.security) {
        case WIFI_WPA_PSK:
            // wpa_passphrase=12345678
            fprintf(fp, "wpa_passphrase=%s\n", conf->ap.passwd);
            fputs("wpa=1\n", fp);
            fputs("wpa_pairwise=CCMP\n", fp);
            fputs("rsn_pairwise=CCMP\n", fp);
            fputs("wpa_key_mgmt=WPA-PSK\n", fp);
            break;
        case WIFI_WPA2_PSK:
            // wpa_passphrase=12345678
            fprintf(fp, "wpa_passphrase=%s\n", conf->ap.passwd);
            fputs("wpa=2\n", fp);
            fputs("wpa_pairwise=CCMP\n", fp);
            fputs("rsn_pairwise=CCMP\n", fp);
            fputs("wpa_key_mgmt=WPA-PSK\n", fp);
            break;
        case WIFI_OPEN:
        default:
            break;
        }
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
        fp = NULL;

        // 创建 udhcpd 配置文件，如果有文件，覆盖
        fp = fopen(UDHCPD_CONF, "w+");
        if (NULL==fp) {
            return -NETERR_FOPEN_FAIL;
        }
        // 写入
        fputs("interface "WIFI_IFACE_NODE"\n", fp);
        fprintf(fp, "start %s\n", conf->dhcpd.startIP);
        fprintf(fp, "end %s\n", conf->dhcpd.endIP);
        fprintf(fp, "max_leases %d\n", conf->dhcpd.max_leases);
        fprintf(fp, "opt router %s\n", conf->dhcpd.opt_router);
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
    }
    usleep(3000000);
    // 检查是否有配置文件
    if (0!=file_exist(WIFI_AP_CONF)) {
        // 没有配置文件
        return -NETERR_HOSTAP_CONF_NONE;
    }
    if (0!=file_exist(UDHCPD_CONF)) {
        // 没有配置文件
        return -NETERR_UDHCPD_CONF_NONE;
    }
    // 启动 hostapd
    //printf("-->> hostapd " WIFI_AP_CONF " -B\n");
    proc_run("hostapd " WIFI_AP_CONF " -B");
    // 设置ip
    ret = set_enable(WIFI_IFACE_NODE, NET_LINK_UP);
    if (ret<0)
        return ret;
    ret = set_ip(WIFI_IFACE_NODE, conf->dhcpd.opt_router);
    if (ret<0)
        return ret;
    ret = set_mask(WIFI_IFACE_NODE, conf->dhcpd.mask);
    if (ret<0)
        return ret;
    // 启动 udhcpd
    //printf("udhcpd -f " UDHCPD_CONF " &\n");
    proc_run("udhcpd -f " UDHCPD_CONF " &");
    return 0;
}

int wifi_ap_disable()
{
    // 关闭 hostapd 和 udhcpd
    // 检查 hostapd
    if (PROC_IS_EXIST==proc_is_run("hostapd")) {
        // 关闭
        system("killall hostapd");
    }
    // 检查 udhcpd
    if (PROC_IS_EXIST==proc_is_run("udhcpd")) {
        // 关闭
        system("killall udhcpd");
    }
    return 0;
}

int wifi_ap_reload(WIFI_AP_CONFIG *conf)
{
    if (NULL==conf)
        return -NETERR_CHECK_PARAM;
    wifi_ap_disable();
    usleep(100000);
    return wifi_ap_enable(conf);
}
