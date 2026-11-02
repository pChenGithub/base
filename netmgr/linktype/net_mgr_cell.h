/**
 * @file net_mgr_cell.h
 * @brief netmgr 链路实现层：4G 蜂窝链路（linktype 层，可独立使用）
 * @details 当前支持 ECM/RNDIS/NCM 网卡形态（驱动已生成网卡，走 DHCP）；
 *          PPP 与 QMI 属于拨号形态，仅在操作表上预留，未实现时返回明确的配置错误。
 */
#ifndef __NET_MGR_CELL_H__
#define __NET_MGR_CELL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_ctx.h"

/** 4G 链路操作表：open/close/refresh 由本层实现 */
extern const NETMGR_OPS netmgr_cell_ops;

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_CELL_H__ */
