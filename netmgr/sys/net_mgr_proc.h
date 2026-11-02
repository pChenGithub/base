/**
 * @file net_mgr_proc.h
 * @brief netmgr 系统适配层：外部进程控制（sys 层，可独立使用）
 * @details 用于探测/结束 wpa_supplicant、hostapd、udhcpd 等外部进程，
 *          以及执行 shell 命令，避免引入对 base 内其它模块的依赖。
 */
#ifndef __NET_MGR_PROC_H__
#define __NET_MGR_PROC_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 查询 ===== */

/**
 * @brief 探测指定名称的进程是否存在
 * @param name 进程名（不含路径），不可为空
 * @return 1 存在；0 不存在或入参非法
 */
int netmgr_proc_exist(const char* name);

/* ===== 操作 ===== */

/**
 * @brief 结束指定名称的全部进程
 * @param name 进程名（不含路径），不可为空
 * @return 0 成功（未找到进程也视为成功）；负值为 NETMGRERR_* 错误码
 */
int netmgr_proc_kill(const char* name);

/**
 * @brief 执行 shell 命令并等待结束
 * @param fmt 命令格式串，不可为空
 * @return 0 命令返回成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_proc_exec(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_PROC_H__ */
