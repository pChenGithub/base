/**
 * @file net_mgr_proc.h
 * @brief netmgr 公共基础能力：外部进程控制（common 层，sys/core 皆可依赖）
 * @details 用于探测/结束 wpa_supplicant、hostapd、udhcpd 等外部进程，
 *          以及执行 shell 命令；置于 common 层是为了让 sys 层模块（如 wpa）也能
 *          自主管理其依赖进程，同时保持 sys 层互相之间零横向依赖。
 *
 * 使用约束（独立调用方须知）：
 * 1) 单实例：无文件级状态，可并发调用；
 * 2) 线程：本层不起线程；exec 走 system，会阻塞调用线程直到命令结束；
 *    spawn 只负责拉起后台进程（双 fork 脱离会话），不阻塞、不产生僵尸；
 * 3) 权限：kill 需目标进程属主权限或 root；
 * 4) 外部依赖：/proc 文件系统与 /bin/sh。
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
 * @details 适用于短命命令；启动常驻守护进程请用 netmgr_proc_spawn，
 *          否则调用线程会被守护进程的存活时间拖住
 * @param fmt 命令格式串，不可为空
 * @return 0 命令返回成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_proc_exec(const char* fmt, ...);

/**
 * @brief 启动后台程序（不等待其结束）
 * @details 双 fork 脱离会话：中间子进程立即退出并由本函数回收，执行者由 init 收养，
 *          因此既不阻塞调用线程也不产生僵尸；执行者的 stdin/stdout/stderr 全部
 *          重定向到 /dev/null，避免外部程序输出与业务终端纠缠
 * @param file 可执行文件名（按 PATH 查找），不可为空
 * @param argv 参数数组，argv[0] 为程序名，须以 NULL 结尾，不可为空
 * @return 0 已成功拉起（不代表进程后续存活）；负值为 NETMGRERR_* 错误码
 */
int netmgr_proc_spawn(const char* file, char* const argv[]);

#ifdef __cplusplus
}
#endif
#endif /* __NET_MGR_PROC_H__ */
