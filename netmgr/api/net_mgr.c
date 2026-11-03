/**
 * @file net_mgr.c
 * @brief netmgr 对外接口层实现：入参校验 + 转调仲裁核心
 * @details 本层不含业务逻辑与状态，只做「空指针挡在门口」与接口转发，
 *          保证 core 层可脱离本层被宿主按需直接使用。
 */
#include "net_mgr.h"

#include "net_mgr_core.h"

#include <stddef.h>

int netmgr_start(const NETMGR_CONF* conf, netmgr_event_cb cb, void* user)
{
    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }
    return netmgr_core_start(conf, cb, user);
}

int netmgr_stop(void)
{
    return netmgr_core_stop();
}

int netmgr_poll(void)
{
    return netmgr_core_poll();
}

int netmgr_prio_set(NETMGR_TYPE type, unsigned char prio)
{
    return netmgr_core_prio_set(type, prio);
}

int netmgr_link_enable(NETMGR_TYPE type, int on)
{
    return netmgr_core_link_enable(type, on);
}

int netmgr_prio_get(NETMGR_TYPE type, unsigned char* prio)
{
    return netmgr_core_prio_get(type, prio);
}

int netmgr_stat_get(NETMGR_LINK* list, int* num)
{
    return netmgr_core_stat_get(list, num);
}

int netmgr_eth_set(const NETMGR_ETH_CONF* conf)
{
    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }
    return netmgr_core_eth_set(conf);
}

int netmgr_wifi_set(const NETMGR_WIFI_CONF* conf)
{
    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }
    return netmgr_core_wifi_set(conf);
}

int netmgr_cell_set(const NETMGR_CELL_CONF* conf)
{
    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }
    return netmgr_core_cell_set(conf);
}

int netmgr_dns_set(const char* dns)
{
    return netmgr_core_dns_set(dns);
}

int netmgr_wifi_scan(NETMGR_WIFI_AP* list, int* num)
{
    return netmgr_core_wifi_scan(list, num);
}

int netmgr_wifi_scan_result(NETMGR_WIFI_AP* list, int* num)
{
    if (NULL == list || NULL == num)
    {
        return -NETMGRERR_PARAM;
    }
    return netmgr_core_wifi_scan_result(list, num);
}
