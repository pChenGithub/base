/**
 * @file demo_dhcp.c
 * @brief netmgr sys 层 DHCP 模块（net_mgr_dhcp）独立使用示例
 * @details 演示通过状态回调获取取址结果：
 *          1) 在指定接口上建立取址会话并注册回调；
 *          2) 回调打印 RUNNING / SUCC / FAIL 与错误码；
 *          3) 等到回调给出 SUCC 或 FAIL 后停止会话（会话常驻续租，不会自行结束）。
 *          注意：本示例会真实拉起 udhcpc 并影响指定接口的地址配置。
 *          编译：make demo_dhcp       运行：./demo_dhcp
 */
#include <stdio.h>
#include <unistd.h>

#include "net_mgr_dhcp.h"

/** 示例取址的接口名 */
#define DEMO_IFNAME     "eth0"
/** 等待取址结果的轮询间隔（毫秒） */
#define DEMO_POLL_MS    100
/** 等待取址结果的最大轮询次数，共 20 秒 */
#define DEMO_POLL_MAX   200

/**
 * 最近一次取址状态（回调线程写入、主线程读取）
 * 用途：主线程据此判断本轮取址是否已给出结果
 * 单位/范围：NETMGR_DHCP_STATE 取值，初值 RUNNING 表示尚未有结果
 * 默认值依据：会话启动后先上报 RUNNING，SUCC / FAIL 才算有结论
 * 写入方/读取方：回调线程写入，main 只读
 * 生命周期：仅内存态，进程退出即失效
 */
static volatile NETMGR_DHCP_STATE g_demoState = NETMGR_DHCP_RUNNING;

/**
 * @brief 取址状态转可读名称
 * @param state 取址状态
 * @return 常量字符串，未知状态返回 UNKNOWN
 */
static const char* demo_state_name(NETMGR_DHCP_STATE state)
{
    switch (state)
    {
    case NETMGR_DHCP_RUNNING:
        return "RUNNING";
    case NETMGR_DHCP_SUCC:
        return "SUCC";
    case NETMGR_DHCP_FAIL:
        return "FAIL";
    case NETMGR_DHCP_LOST:
        return "LOST";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 取址状态回调：打印状态与错误码（只做通知，无返回值）
 * @param state  取址状态
 * @param ifname 接口名
 * @param err    状态为 FAIL 时的错误码
 * @param user   未使用
 */
static void demo_dhcp_cb(NETMGR_DHCP_STATE state, const char* ifname,
                         int err, void* user)
{
    (void)user;

    printf("[dhcp] %-7s ifname=%-8s err=%d\r\n", demo_state_name(state),
           (NULL == ifname) ? "" : ifname, err);
    g_demoState = state;
}

/**
 * @brief 示例入口：建立取址会话、等待取址结果、停止会话
 * @return 0 正常结束；1 会话建立失败
 */
int main(void)
{
    int ret = 0; /* 接口返回值 */
    int i = 0;   /* 等待轮次，从 0 开始 */

    ret = netmgr_dhcp_start(DEMO_IFNAME, demo_dhcp_cb, NULL);
    if (ret < 0)
    {
        /* -NETMGRERR_DHCP 表示该接口已有取址会话或槽位已满 */
        printf("[dhcp] 建立取址会话失败, ifname=%s ret=%d\r\n", DEMO_IFNAME, ret);
        return 1;
    }

    /* 会话常驻、不会自行结束：无服务器时 udhcpc 持续重试（不报 FAIL），
       因此等到 SUCC/FAIL 或 20 秒上限后停止 */
    for (i = 0; i < DEMO_POLL_MAX; ++i)
    {
        if (NETMGR_DHCP_SUCC == g_demoState || NETMGR_DHCP_FAIL == g_demoState)
            break;
        usleep(DEMO_POLL_MS * 1000);
    }

    /* 未等到结论时明确指出超时，避免“没打印就是没问题”的误解 */
    if (NETMGR_DHCP_SUCC != g_demoState && NETMGR_DHCP_FAIL != g_demoState)
        printf("[dhcp] 等待超时, ifname=%s wait=%ds state=%s（将停止会话）\r\n",
               DEMO_IFNAME, (int)(DEMO_POLL_MAX * DEMO_POLL_MS / 1000),
               demo_state_name(g_demoState));

    ret = netmgr_dhcp_stop(DEMO_IFNAME);
    if (ret < 0)
        printf("[dhcp] 停止取址会话失败, ifname=%s ret=%d\r\n", DEMO_IFNAME, ret);
    else
        printf("[dhcp] 停止取址会话成功, ifname=%s\r\n", DEMO_IFNAME);
    return 0;
}
