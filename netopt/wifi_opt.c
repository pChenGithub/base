#include "wifi_opt.h"
#include "net_errno.h"
#include "file_opt.h"
#include "wpas/wpa_ctrl.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define WPA_SUPPLICANT_CONF ""
#define WPA_CONNECT_FILE    "/var/run/wpa_supplicant"
//
typedef struct {
    struct wpa_ctrl* ctrl_conn;     // 命令接口
    struct wpa_ctrl* monitor_conn;  // 监听接口
    char reply[4096];               // 回复消息
} WIFI_NODE; // 单个wifi节点
static WIFI_NODE node_wlan0;
// 建立和wpa的通信
static int connect_to_wpa(WIFI_NODE* node, const char* path) {
    int ret = 0;
    if (NULL==node || NULL==path)
        return -NETERR_CHECK_PARAM;
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

    // 正确返回
    return 0;

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
    return 0;
}

int wifi_sta_enable() {
    // 判断是否存在进程
    if (0==proc_is_run("wpa_supplicant")) {
        return -NETERR_WPA_IS_RUN;
    }
    // 判断当前是否是ap模式
    if (0==proc_is_run("hostapd")) {
        // 关闭
        system("killall hostapd");
    }
    if (0==proc_is_run("udhcpd")) {
        // 关闭
        system("killall udhcpd");
    }
    usleep(200000);
    // 检查是否存在 wpa_supplicant 配置文件
    if (0!=file_exist(WPA_SUPPLICANT_CONF)) {
        // 文件不存在，创建一个新的最小的文件
        return -NETERR_WPA_CONF_NONE;
    }
    // 启动wpa进程，有可能优化
    char cmd[] = {0};
    snprintf(cmd, sizeof(cmd), "wpa_supplicant -Dwext -iwlan0 -c "WPA_SUPPLICANT_CONF" -B");
    system(cmd);
    // 启动完wpa进程之后，连接wpa通信
    int ret = connect_to_wpa(&node_wlan0, WPA_CONNECT_FILE);
    if (ret<0) {
        return ret;
    }
    // 启动线程监听wpa

    return 0;
}

int wifi_sta_disable() {
    // 判断是否存在进程
    if (0!=proc_is_run("wpa_supplicant")) {
        return -NETERR_WPA_NOT_RUN;
    }
    // 关闭监听线程
    // 关闭wpa通信
    disconnect_to_wpa(&node_wlan0);
    // 关闭进程 wpa_supplicant
}

int wifi_sta_scan()
{
    int reply_len = 0;
    if (NULL==node_wlan0.ctrl_conn)
        return -NETERR_CHECK_PARAM;
    int ret = wpa_ctrl_request(node_wlan0.ctrl_conn, "SCAN", strlen("SCAN"),
                               node_wlan0.reply, &reply_len, NULL);
    if (-2==ret) {
        // 超时
    } else if (ret<0 || 0==strncmp(node_wlan0.reply, "FAIL", 4)) {
        // 失败
    }
    if (strncmp(cmd, "PING", 4) == 0)
        node_wlan0.reply[*reply_len] = '\0';

    return 0;
}
