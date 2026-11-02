/**
 * @file net_mgr_link.h
 * @brief netmgr 链路实现层：链路通用就绪判定（linktype 层，可独立使用）
 * @details 适用于「DHCP 取址」形态的链路（以太网 DHCP、WiFi STA、4G ECM）：
 *          按「物理连通 + 已取得地址 + 有默认网关」判定是否就绪，
 *          并同步刷新上下文中的 IP、掩码与网关快照。
 *          本层只依赖 common 与 sys 层，可脱离 core 层单独使用。
 */
#ifndef __NET_MGR_LINK_H__
#define __NET_MGR_LINK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "net_mgr_ctx.h"

/* ===== 状态判定 ===== */

/**
 * @brief 按「物理连通 + 已取地址 + 有默认网关」判定链路是否就绪并刷新快照
 * @details 网关从路由表回查（由 DHCP 客户端写入），未就绪时状态置为 DOWN
 * @param ctx 链路上下文，不可为空
 * @return 0 成功（未就绪时状态置为 DOWN）；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_ready(NETMGR_CTX* ctx);

/**
 * @brief 按「物理连通 + 已取地址 + 配置网关非空」判定静态地址链路是否就绪
 * @details 静态链路的网关由配置给出，无需回查路由表；未就绪时状态置为 DOWN
 * @param ctx  链路上下文，不可为空
 * @param gate 配置网关，不可为空（空串表示该链路不参与默认路由，判为 DOWN）
 * @return 0 成功（未就绪时状态置为 DOWN）；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_ready_static(NETMGR_CTX* ctx, const char* gate);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_LINK_H__ */
