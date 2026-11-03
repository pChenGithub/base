/**
 * @file demo_dhcp.c
 * @brief netmgr sys 层 DHCP 模块（net_mgr_dhcp）独立使用示例
 * @details 演示通过状态回调获取取址结果：
 *          1) 在指定接口上发起取址并注册回调；
 *          2) 回调打印 RUNNING / SUCC / FAIL 与错误码；
 *          3) 等待取址线程结束后停止并回收结果。
 *          注意：本示例会真实拉起 udhcpc 并影响指定接口的地址配置。
 *          编译：make demo_dhcp       运行：./demo_dhcp
 */
#include <stdio.h>
#include <unistd.h>

#include "net_mgr_dhcp.h"

/** 示例取址的接口名 */
#define DEMO_IFNAME     "eth0"
/** 单次请求超时秒数，0 表示由 udhcpc 自行决定 */
#define DEMO_TIMEOUT_S  10
/** 等待取址线程结束的轮询间隔（毫秒） */
#define DEMO_POLL_MS    100
/** 等待取址线程结束的最大轮询次数，共 20 秒 */
#define DEMO_POLL_MAX   200

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
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 取址状态回调：打印状态与错误码
 * @param state  取址状态
 * @param ifname 接口名
 * @param err    状态为 FAIL 时的错误码
 * @param user   未使用
 * @return 恒返回 NETMGR_DHCP_RET_OK，不要求内部重试
 */
static NETMGR_DHCP_RET demo_dhcp_cb(NETMGR_DHCP_STATE state, const char* ifname,
                                    int err, void* user)
{
    (void)user;

    printf("[dhcp] %-7s ifname=%-8s err=%d\r\n", demo_state_name(state),
           (NULL == ifname) ? "" : ifname, err);
    return NETMGR_DHCP_RET_OK;
}

/**
 * @brief 示例入口：发起取址、等待结束、停止并打印结果
 * @return 0 正常结束；1 启动失败
 */
int main(void)
{
    int ret = 0; /* 接口返回值 */
    int i = 0;   /* 等待轮次，从 0 开始 */

    ret = netmgr_dhcp_start(DEMO_IFNAME, DEMO_TIMEOUT_S, demo_dhcp_cb, NULL);
    if (ret < 0)
    {
        /* -NETMGRERR_DHCP 表示该接口已有取址线程或槽位已满 */
        printf("发起取址失败, ret=%d\r\n", ret);
        return 1;
    }

    /* 等待取址线程结束（udhcpc 取到租约或失败退出） */
    for (i = 0; i < DEMO_POLL_MAX; ++i)
    {
        if (!netmgr_dhcp_busy(DEMO_IFNAME))
            break;
        usleep(DEMO_POLL_MS * 1000);
    }

    ret = netmgr_dhcp_stop(DEMO_IFNAME);
    if (ret < 0)
        printf("停止取址返回错误, ret=%d\r\n", ret);
    else
        printf("停止取址完成\r\n");
    return 0;
}
