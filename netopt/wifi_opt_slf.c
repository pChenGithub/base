#include "wifi_opt.h"
#include "net_errno.h"
#include "file_opt.h"
#include <sys/socket.h>
#include <sys/un.h>

#define PROJECT_DIR_CONFIG "/home/rockchip"
#define WPA_SUPPLICANT_CONF PROJECT_DIR_CONFIG"/wpa.conf"   // WIFI 配置文件
#define WPA_CONNECT_FILE    "/var/run/wpa_supplicant"
#define WIFI_AP_CONF        PROJECT_DIR_CONFIG"/ap.conf"    // ap 配置文件
#define UDHCPD_CONF         PROJECT_DIR_CONFIG"/dhcpd.conf" // dhcp 配置文件

#define WIFI_IFACE_NODE "wlan0" // WIFI 节点
typedef struct {
    int sockfd;
    struct sockaddr_un addr;
    char interface[64];
    char temp_path[1024];
} WIFI_NODE; // 单个wifi节点
static WIFI_NODE node_wlan0;

static int connect_to_wpa(WIFI_NODE* node, const char* path) {
    // 创建Unix Domain Socket
    node->sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (node->sockfd < 0) {
        return -1;
    }
    // 设置服务器地址
    memset(&node->addr, 0, sizeof(node->addr));
    node->addr.sun_family = AF_UNIX;
    // 构建socket路径
    strncpy(node->addr.sun_path, path, sizeof(node->addr.sun_path) - 1);
    strncpy(node->interface, WIFI_IFACE_NODE, sizeof(node->interface) - 1);
    // 绑定客户端地址
    if (bind(node->sockfd, (struct sockaddr *)&client_addr, 
            offsetof(struct sockaddr_un, sun_path) + strlen(client_addr.sun_path) + 1) < 0) {
        perror("绑定客户端socket失败");
        close(ctrl->sockfd);
        free(ctrl);
        return NULL;
    }
    // 设置文件权限为600 (只有所有者可读写)
    if (chmod(ctrl->temp_path, 0600) < 0) {
        perror("设置文件权限失败");
        close(ctrl->sockfd);
        unlink(ctrl->temp_path);
        free(ctrl);
        return NULL;
    }
    // 设置超时时间
    struct timeval timeout;
    timeout.tv_sec = 10;  // 10秒超时
    timeout.tv_usec = 0;
    if (setsockopt(ctrl->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("设置接收超时失败");
        close(ctrl->sockfd);
        unlink(ctrl->temp_path);
        free(ctrl);
        return NULL;
    }

    return 0;
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


    // 检查是否存在 wpa_supplicant 配置文件
    if (0!=file_exist(WPA_SUPPLICANT_CONF)) {
        // 文件不存在，创建一个新的最小的文件
        if (wpa_conf_simple(WPA_SUPPLICANT_CONF)<0)
            return -NETERR_WPA_CONF_NONE;
    }

    int ret = connect_to_wpa(&node_wlan0, WPA_CONNECT_FILE"/"WIFI_IFACE_NODE);
    if (ret<0) {
        return ret;
    }

    return 0;
}
