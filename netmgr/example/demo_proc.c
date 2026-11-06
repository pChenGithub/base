/**
 * @file demo_proc.c
 * @brief netmgr common 层进程模块（net_mgr_proc）独立使用示例
 * @details 演示：
 *          1) 探测若干常见进程是否存在；
 *          2) 执行一条无害的 shell 命令并检查返回值。
 *          结束进程会直接影响系统，示例不自动执行（用法见文末注释）。
 *          编译：make demo_proc       运行：./demo_proc
 */
#include <stdio.h>

#include "net_mgr_proc.h"

/** 待探测的进程名列表 */
static const char* const g_procs[] = {"init", "systemd", "udhcpc", "netmgr"};

/**
 * @brief 示例入口：探测进程并执行一条 shell 命令
 * @return 0 正常结束；1 命令执行失败
 */
int main(void)
{
    int procNum = (int)(sizeof(g_procs) / sizeof(g_procs[0])); /* 待探测进程数量 */
    int ret = 0;                                               /* 接口返回值 */
    int i = 0;                                                 /* 探测下标，从 0 开始 */

    printf("[proc] 进程探测：\r\n");
    for (i = 0; i < procNum; ++i)
        printf("[proc] %-10s 存在: %s\r\n", g_procs[i],
               netmgr_proc_exist(g_procs[i]) ? "是" : "否");

    printf("[proc] 命令执行：\r\n");
    /* 子进程直接继承标准输出，先刷新本进程缓冲以保证打印顺序 */
    (void)fflush(stdout);
    ret = netmgr_proc_exec("echo netmgr proc demo");
    if (ret < 0)
    {
        printf("[proc] 执行命令失败, cmd=%s ret=%d\r\n", "echo netmgr proc demo", ret);
        return 1;
    }
    printf("[proc] 命令执行成功, cmd=%s\r\n", "echo netmgr proc demo");

    /* 结束进程会影响系统运行，示例不自动执行，用法如下：
       netmgr_proc_kill("udhcpc"); */
    return 0;
}
