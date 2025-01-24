#include "wifi_opt.h"
#include "net_errno.h"
#include "file_opt.h"
#include "wpas/wpa_ctrl.h"
#include "net_opt.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define WPA_SUPPLICANT_CONF ""
#define WPA_CONNECT_FILE    "/var/run/wpa_supplicant"
#define WIFI_AP_CONF        "/home/rockchip/zytk_gate/data/ap.conf"
#define UDHCPD_CONF         "/home/rockchip/zytk_gate/data/dhcpd.conf"
//
typedef struct {
    struct wpa_ctrl* ctrl_conn;     // 命令接口
    struct wpa_ctrl* monitor_conn;  // 监听接口
    char reply[4096];               // 回复消息
} WIFI_NODE; // 单个wifi节点
static WIFI_NODE node_wlan0;
//
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

// 启动进程
static int proc_run(const char* cmd) {
    system(cmd);
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
#if 0
    if (strncmp(cmd, "PING", 4) == 0)
        node_wlan0.reply[reply_len] = '\0';
#endif

    return 0;
}

int wifi_ap_enable(WIFI_AP_CONFIG *conf)
{
    //int ret = 0;
    // 判断是否存在进程
    if (0==proc_is_run("hostapd")) {
        system("killall hostapd");
    }
    if (0==proc_is_run("udhcpd")) {
        system("killall udhcpd");
    }
    // 检查 wpa_supplicant，关闭
    if (0==proc_is_run("wpa_supplicant")) {
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
        fputs("interface=wlan0\n", fp);
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
        fclose(fp);
        fp = NULL;

        // 创建 udhcpd 配置文件，如果有文件，覆盖
        fp = fopen(UDHCPD_CONF, "w+");
        if (NULL==fp) {
            return -NETERR_FOPEN_FAIL;
        }
        // 写入
        fputs("interface wlan0\n", fp);
        fprintf(fp, "start %s\n", conf->dhcpd.startIP);
        fprintf(fp, "end %s\n", conf->dhcpd.endIP);
        fprintf(fp, "max_leases %d\n", conf->dhcpd.max_leases);
        fprintf(fp, "opt router %s\n", conf->dhcpd.opt_router);
        fflush(fp);
        fclose(fp);
    }
    usleep(300000);
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
    system("ifconfig wlan0 172.6.6.1 netmask 255.255.255.0 up");
    // 启动 udhcpd
    //printf("udhcpd -f " UDHCPD_CONF " &\n");
    proc_run("udhcpd -f " UDHCPD_CONF " &");
    return 0;
}

int wifi_ap_disable()
{
    // 关闭 hostapd 和 udhcpd
    // 检查 hostapd
    if (0==proc_is_run("hostapd")) {
        // 关闭
        system("killall hostapd");
    }
    // 检查 udhcpd
    if (0==proc_is_run("udhcpd")) {
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
