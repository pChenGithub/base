/**
 * @file net_mgr_util.h
 * @brief netmgr 通用工具：地址与文本校验（common 层，可独立使用）
 * @details 本层只做纯计算与格式校验，不触碰内核与外部进程；
 *          sys / linktype / core / api 各层均可直接调用。
 */
#ifndef __NET_MGR_UTIL_H__
#define __NET_MGR_UTIL_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 校验 ===== */

/**
 * @brief 校验 IPv4 点分十进制字符串合法性
 * @param ip 待校验字符串，不可为空
 * @return 1 合法；0 非法
 */
int netmgr_ip_valid(const char* ip);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_UTIL_H__ */
