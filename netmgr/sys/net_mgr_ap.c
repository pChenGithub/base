/**
 * @file net_mgr_ap.c
 * @brief netmgr 系统适配层：WiFi AP 模式（hostapd + udhcpd）控制实现
 * @details 仅依赖 common 层（错误码 + 进程控制）与 libc；配置写到 /tmp，
 *          进程用 netmgr_proc_spawn 拉起（双 fork，不阻塞调用线程）。
 */
#include "net_mgr_ap.h"

#include "net_mgr_err.h"
#include "net_mgr_proc.h"

#include <stdio.h>
#include <string.h>

/** hostapd 配置文件路径 */
#define AP_CONF_PATH   "/tmp/netmgr_ap.conf"
/** AP 侧 udhcpd 配置文件路径 */
#define AP_DHCP_PATH   "/tmp/netmgr_dhcpd.conf"
/** 固定信道：单射频下 AP 与 STA 共用同一信道，取 2.4G 常用信道 6 */
#define AP_CHANNEL     "6"
/** 进程名：用于结束残留实例与存活判定 */
#define AP_PROC        "hostapd"
/** DHCP 服务进程名 */
#define AP_DHCP_PROC   "udhcpd"

/**
 * @brief 写 hostapd 配置
 * @param conf AP 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int ap_conf_write(const NETMGR_APCONF* conf)
{
    FILE* fp = NULL; /* 配置文件句柄 */

    fp = fopen(AP_CONF_PATH, "w");
    if (NULL == fp)
        return -NETMGRERR_FILE;

    /* 基础参数：接口、驱动、SSID 与固定信道 */
    if (fprintf(fp, "interface=%s\ndriver=nl80211\nssid=%s\nhw_mode=g\nchannel=%s\n",
                conf->ifname, conf->ssid, AP_CHANNEL) < 0)
    {
        fclose(fp);
        return -NETMGRERR_FILE;
    }

    /* 口令非空才写 WPA 段：空口令按开放 AP 输出 */
    if ('\0' != conf->passwd[0] &&
        fprintf(fp, "wpa=2\nwpa_passphrase=%s\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n",
                conf->passwd) < 0)
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
 * @brief 写 AP 侧 udhcpd 配置
 * @details 地址池与 router/subnet 均取自配置；DNS 未配置时不写该选项，
 *          由客户端沿用自身设置
 * @param conf AP 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int ap_dhcp_conf(const NETMGR_APCONF* conf)
{
    FILE* fp = NULL; /* 配置文件句柄 */

    fp = fopen(AP_DHCP_PATH, "w");
    if (NULL == fp)
        return -NETMGRERR_FILE;

    if (fprintf(fp, "start %s\nend %s\ninterface %s\nopt router %s\nopt subnet %s\n",
                conf->ap_start, conf->ap_end, conf->ifname,
                conf->ap_ip, conf->ap_mask) < 0)
    {
        fclose(fp);
        return -NETMGRERR_FILE;
    }
    if ('\0' != conf->dns[0] && fprintf(fp, "opt dns %s\n", conf->dns) < 0)
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
 * @brief 拉起一个常驻进程（argv 以 NULL 结尾）
 * @param file 可执行文件名，不可为空
 * @param arg1 第一个参数（配置路径），不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
static int ap_spawn(const char* file, const char* arg1)
{
    char* argv[4] = {0}; /* 参数表：程序名 + 可选 -B + 配置路径 */

    argv[0] = (char*)file;
    if (0 == strcmp(file, AP_PROC))
    {
        argv[1] = (char*)"-B";
        argv[2] = (char*)arg1;
    }
    else
    {
        argv[1] = (char*)arg1;
    }
    return netmgr_proc_spawn(file, argv);
}

int netmgr_ap_start(const NETMGR_APCONF* conf)
{
    int ret = 0; /* 子步骤返回值 */

    if (NULL == conf || '\0' == conf->ifname[0] || '\0' == conf->ssid[0] ||
        '\0' == conf->ap_ip[0] || '\0' == conf->ap_mask[0] ||
        '\0' == conf->ap_start[0] || '\0' == conf->ap_end[0])
    {
        return -NETMGRERR_CONF;
    }

    /* 第 1 步：先停旧实例，保证按新配置重建，不残留旧进程 */
    (void)netmgr_ap_stop();

    /* 第 2 步：写配置（hostapd 与 udhcpd 各自一份） */
    ret = ap_conf_write(conf);
    if (ret < 0)
        return ret;
    ret = ap_dhcp_conf(conf);
    if (ret < 0)
        return ret;

    /* 第 3 步：拉起 hostapd（信标与关联），失败即回收 */
    ret = ap_spawn(AP_PROC, AP_CONF_PATH);
    if (ret < 0)
    {
        (void)netmgr_ap_stop();
        return -NETMGRERR_PROC;
    }

    /* 第 4 步：拉起 udhcpd（地址分配），失败时连 hostapd 一起回收 */
    ret = ap_spawn(AP_DHCP_PROC, AP_DHCP_PATH);
    if (ret < 0)
    {
        (void)netmgr_ap_stop();
        return -NETMGRERR_PROC;
    }
    return 0;
}

int netmgr_ap_stop(void)
{
    int err = 0; /* 首个错误码，0 表示全部成功 */
    int ret = 0; /* 子步骤返回值 */

    /* 先停 DHCP 服务再停信标：避免新客户端接入后拿不到地址 */
    ret = netmgr_proc_kill(AP_DHCP_PROC);
    if (ret < 0)
        err = ret;
    ret = netmgr_proc_kill(AP_PROC);
    if (ret < 0 && 0 == err)
        err = ret;
    return err;
}

int netmgr_ap_running(void)
{
    return netmgr_proc_exist(AP_PROC);
}
