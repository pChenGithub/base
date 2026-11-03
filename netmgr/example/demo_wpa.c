/**
 * @file demo_wpa.c
 * @brief netmgr wpa_supplicant 控制接口模块独立示例（sys 层）
 * @details 演示「连接 / 状态 / 扫描 / 结果 / 断开」五类能力；
 *          本示例不启动 wpa_supplicant，需目标机上该进程已在运行，
 *          否则各接口按错误码返回（-NETMGRERR_NOTFOUND / -NETMGRERR_WPA）。
 *
 * 编译：make demo_wpa
 * 运行：./demo_wpa [ifname]        （不传接口名时使用 wlan0）
 *
 * 只读演示：不调用 netmgr_wpa_connect，避免误改设备的无线配置；
 * 连接用法见文件末尾注释。
 */
#include <stdio.h>
#include <string.h>

#include "net_mgr_wpa.h"

/** 默认演示接口名 */
#define DEMO_IFNAME "wlan0"
/** 扫描结果数组容量 */
#define DEMO_AP_MAX 16

/**
 * @brief 打印一次接口返回值
 * @param what 动作名称，不可为空
 * @param ret  接口返回值
 */
static void demo_ret(const char* what, int ret)
{
    if (ret < 0)
        printf("%-12s 失败, ret=%d\r\n", what, ret);
    else
        printf("%-12s 成功\r\n", what);
}

/**
 * @brief 示例入口
 * @param argc 参数个数
 * @param argv 参数列表：argv[1] 可选，接口名
 * @return 恒返回 0
 */
int main(int argc, char** argv)
{
    const char* ifname = (argc > 1) ? argv[1] : DEMO_IFNAME; /* 目标接口名 */
    NETMGR_WPA_STATE state = NETMGR_WPA_UNKNOWN;             /* 工作状态 */
    NETMGR_WIFI_INFO info = {0};                             /* 连接信息 */
    NETMGR_WIFI_AP list[DEMO_AP_MAX] = {0};                  /* 扫描结果 */
    int num = DEMO_AP_MAX;                                   /* 结果条数 */
    int ret = 0;                                             /* 接口返回值 */
    int i = 0;                                               /* 结果下标，从 0 开始 */

    printf("== wpa 控制接口演示, 接口=%s ==\r\n", ifname);

    /* 1. 建立控制通道 */
    ret = netmgr_wpa_open(ifname);
    demo_ret("open", ret);
    if (ret < 0)
    {
        printf("提示: 需 wpa_supplicant 已运行且 /var/run/wpa_supplicant/%s 存在\r\n", ifname);
        return 0;
    }

    /* 2. 读状态与连接信息 */
    if (0 == netmgr_wpa_state(&state))
        printf("state       = %d (0=未知 1=扫描中 2=关联中 3=已连接 4=已断开)\r\n", (int)state);
    if (0 == netmgr_wpa_status(&info))
        printf("status      connected=%d ssid=%s bssid=%s freq=%d signal=%d speed=%d\r\n",
               info.connected, info.ssid, info.bssid, info.freq, info.signal, info.linkSpeed);

    /* 3. 扫描并取结果 */
    ret = netmgr_wpa_scan(3000);
    demo_ret("scan", ret);
    if (0 == ret)
    {
        num = DEMO_AP_MAX;
        if (0 == netmgr_wpa_scan_result(list, &num))
        {
            printf("扫描到 %d 个热点\r\n", num);
            for (i = 0; i < num; ++i)
                printf("  [%2d] ssid=%-20s bssid=%s freq=%4d signal=%4d secure=%d\r\n",
                       i, list[i].ssid, list[i].bssid, list[i].freq, list[i].signal,
                       list[i].secure);
        }
        else
        {
            printf("读取扫描结果失败\r\n");
        }
    }

    /* 4. 断开并关闭通道（断开具有幂等性，未连接时同样返回 0） */
    demo_ret("disconnect", netmgr_wpa_disconnect());
    netmgr_wpa_close();
    return 0;
}

/*
 * 连接用法（谨慎：会修改设备的无线配置）：
 *
 *   netmgr_wpa_open("wlan0");
 *   netmgr_wpa_connect("SSID", "passwd", 10000);
 *   ...
 *   netmgr_wpa_disconnect();
 *   netmgr_wpa_close();
 */
