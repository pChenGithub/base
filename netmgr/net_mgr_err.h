/**
 * @file net_mgr_err.h
 * @brief netmgr 网络管理服务的错误码定义
 * @details 编码从 200 起，避开 netopt 模块已使用的 0~110 区间；
 *          所有对外接口出错时返回对应错误码的负值。
 */
#ifndef __NET_MGR_ERR_H__
#define __NET_MGR_ERR_H__

/** 入参非法（空指针、越界值、长度不足） */
#define NETMGRERR_PARAM     200
/** 模块状态不允许该操作（未启动、重复启动） */
#define NETMGRERR_STATE     201
/** 线程创建/回收失败 */
#define NETMGRERR_THREAD    202
/** socket 创建、bind、send、recv 失败 */
#define NETMGRERR_SOCKET    203
/** 网络接口 ioctl 操作失败 */
#define NETMGRERR_IOCTL     204
/** 未找到目标对象（接口不存在、配置项缺失） */
#define NETMGRERR_NOTFOUND  205
/** 路由下发或删除失败 */
#define NETMGRERR_ROUTE     206
/** DHCP 取址失败或实例不可用 */
#define NETMGRERR_DHCP      207
/** wpa_supplicant/hostapd 控制通道操作失败 */
#define NETMGRERR_WPA       208
/** 外部进程拉起、探测或结束失败 */
#define NETMGRERR_PROC      209
/** 文件打开、写入、替换失败 */
#define NETMGRERR_FILE      210
/** 缓冲区长度不足 */
#define NETMGRERR_BUFF      211
/** 配置内容非法或不受支持 */
#define NETMGRERR_CONF      212
/** 等待链路就绪超时 */
#define NETMGRERR_TIMEOUT   213
/** 动态内存申请失败 */
#define NETMGRERR_MEM       214

#endif
