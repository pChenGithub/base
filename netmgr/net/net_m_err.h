/**
 * @file net_m_err.h
 * @brief net 网络管理模块错误码（net 层，对外）
 * @details 编码从 220 起，避开 netmgr 既有 200~214 区间，便于两套实现并存比对。
 *          本层自有错误使用 NETM_ERR_*；sys 层返回的 NETMGRERR_* 负值原样上抛，
 *          不做转译，调用方按两个区间分别判读。
 *          模块内部不打印任何日志，异常一律以负值返回，由调用方按码打印。
 */
#ifndef __NET_M_ERR_H__
#define __NET_M_ERR_H__

#ifdef __cplusplus
extern "C" {
#endif

/** 入参非法（空指针、越界值、长度不足） */
#define NETM_ERR_PARAM      220
/** 模块状态不允许该操作（未启动、重复启动、正在停止） */
#define NETM_ERR_STATE      221
/** 线程、互斥量或条件变量操作失败 */
#define NETM_ERR_THREAD     222
/** 未找到目标对象（接口不存在、配置项缺失） */
#define NETM_ERR_NOTFOUND   223
/** 配置内容非法或组合不成立（含优先级冲突） */
#define NETM_ERR_CONF       224
/** 动态内存申请失败 */
#define NETM_ERR_MEM        225
/** 默认路由下发或删除失败 */
#define NETM_ERR_ROUTE      226
/** DNS 配置写入失败 */
#define NETM_ERR_DNS        227
/** 外部进程拉起、探测或结束失败 */
#define NETM_ERR_PROC       228
/** 等待结果超时 */
#define NETM_ERR_TIMEOUT    229
/** 网络接口 ioctl 操作失败 */
#define NETM_ERR_IOCTL      230
/** DHCP 取址会话异常 */
#define NETM_ERR_DHCP       231
/** wpa_supplicant 控制通道操作失败 */
#define NETM_ERR_WPA        232
/** 命令队列已满或已关闭 */
#define NETM_ERR_QUEUE      233
/** 该能力在当前构建中未实现（预留：PPP / QMI 拨号） */
#define NETM_ERR_NOTSUP     234
/** 优先级冲突（同一条链路类型重复登记） */
#define NETM_ERR_PRIO       235
/** 缓冲区长度不足 */
#define NETM_ERR_BUFF       236

#ifdef __cplusplus
}
#endif
#endif /* __NET_M_ERR_H__ */
