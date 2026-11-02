/**
 * @file net_mgr.c
 * @brief netmgr 对外接口层实现：入参校验 + 转调仲裁核心
 * @details 本层不含业务逻辑与状态，只做「空指针挡在门口」与接口转发，
 *          保证 core 层可脱离本层被宿主按需直接使用。
 */
#include "net_mgr.h"

#include "net_mgr_core.h"

#include <stddef.h>

/**
 * @brief 启动网络管理服务，按配置拉起全部启用的链路
 * @param conf 总配置，不可为空
 * @param cb   链路状态回调，可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_start(const NETMGR_CONF* conf, netmgr_ev_cb cb)
{
    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }
    return netmgr_core_start(conf, cb);
}

/**
 * @brief 停止网络管理服务，关闭全部链路并删除本模块下发的默认路由
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_stop(void)
{
    return netmgr_core_stop();
}

/**
 * @brief 运行时修改指定类型链路的优先级
 * @param type 链路类型
 * @param prio 新优先级（0-255，小者优先）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_prio_set(NETMGR_TYPE type, unsigned char prio)
{
    return netmgr_core_prio_set(type, prio);
}

/**
 * @brief 读取指定类型链路的当前优先级
 * @param type 链路类型
 * @param prio 输出当前优先级，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_prio_get(NETMGR_TYPE type, unsigned char* prio)
{
    return netmgr_core_prio_get(type, prio);
}

/**
 * @brief 查询全部链路的状态快照
 * @param list 输出数组，容量由 *num 传入
 * @param num  入参为数组容量，出参为实际写入条数
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_stat_get(NETMGR_LINK* list, int* num)
{
    return netmgr_core_stat_get(list, num);
}

/**
 * @brief 重新配置以太网链路
 * @param conf 以太网配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_eth_set(const NETMGR_ETH_CONF* conf)
{
    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }
    return netmgr_core_eth_set(conf);
}

/**
 * @brief 重新配置 WiFi 链路
 * @param conf WiFi 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_wifi_set(const NETMGR_WIFI_CONF* conf)
{
    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }
    return netmgr_core_wifi_set(conf);
}

/**
 * @brief 重新配置 4G 链路
 * @param conf 4G 配置，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_cell_set(const NETMGR_CELL_CONF* conf)
{
    if (NULL == conf)
    {
        return -NETMGRERR_PARAM;
    }
    return netmgr_core_cell_set(conf);
}

/**
 * @brief 下发 DNS 服务器地址
 * @param dns 单个 IPv4 地址字符串，为空或空串表示清除已下发的 DNS
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_dns_set(const char* dns)
{
    return netmgr_core_dns_set(dns);
}
