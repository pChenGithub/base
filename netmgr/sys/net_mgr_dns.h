/**
 * @file net_mgr_dns.h
 * @brief netmgr 系统适配层：DNS 下发（sys 层，可独立使用）
 * @details 将 DNS 服务器写入系统 DNS 配置文件，采用「写临时文件 + 原子改名」，
 *          避免宿主程序读到半截内容。
 *
 * 使用约束（独立调用方须知）：
 * 1) 单实例：无文件级状态；多处并发下发同一文件需由调用方串行化；
 * 2) 线程：本层不起线程，接口同步返回；
 * 3) 权限：需可写系统 DNS 配置文件（通常为 root）；
 * 4) 外部依赖：系统 DNS 配置文件及其所在目录可写。
 */
#ifndef __NET_MGR_DNS_H__
#define __NET_MGR_DNS_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 操作 ===== */

/**
 * @brief 将 DNS 写入系统 DNS 配置文件
 * @param dns IPv4 地址；为空或空串表示清空 DNS 配置
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_dns_apply(const char* dns);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_DNS_H__ */
