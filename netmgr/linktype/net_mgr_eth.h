/**
 * @file net_mgr_eth.h
 * @brief netmgr 链路实现层：以太网链路（linktype 层，可独立使用）
 * @details 对外只暴露一份操作表，由 core 层按链路类型挂载；
 *          本层依赖 common + sys 层，不依赖 core 层。
 */
#ifndef __NET_MGR_ETH_H__
#define __NET_MGR_ETH_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_ctx.h"

/** 以太网链路操作表：open/close/refresh 由本层实现 */
extern const NETMGR_OPS netmgr_eth_ops;

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_ETH_H__ */
