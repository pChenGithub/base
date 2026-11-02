/**
 * @file net_mgr_link.c
 * @brief 链路通用能力：接口存在性、物理连通、启停、IPv4 地址读写与合法性校验
 * @details 全部基于 ioctl 实现，仅依赖内核，不引用 base 内其它自研模块。
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include "net_mgr_def.h"

/**
 * @brief 打开一次性的接口控制 socket
 * @return 成功返回 socket 描述符；失败返回负的 NETMGRERR_* 错误码
 */
static int link_sock_open(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0); /* 接口控制类 ioctl 只需 UDP socket */

    if (fd < 0)
    {
        NETMGR_ERR("socket 创建失败, errno=%d", errno);
        return -NETMGRERR_SOCKET;
    }
    return fd;
}

/**
 * @brief 判断网络接口是否存在
 * @param ifname 接口名，不可为空
 * @return 1 存在；0 不存在或入参非法
 */
int netmgr_link_exist(const char* ifname)
{
    if (NULL == ifname || '\0' == ifname[0])
    {
        NETMGR_ERR("入参 ifname 非法");
        return 0;
    }
    return (0 != if_nametoindex(ifname)) ? 1 : 0;
}

/**
 * @brief 判断接口物理链路是否连通
 * @param ifname 接口名，不可为空
 * @return 1 连通；0 未连通或查询失败
 */
int netmgr_link_up(const char* ifname)
{
    struct ifreq req = {0};  /* ioctl 参数：接口名与接口标志 */
    int fd = -1;       /* 临时控制 socket */
    int ret = 0;       /* ioctl 返回值 */
    int err = 0;       /* 失败时的 errno 快照 */

    if (NULL == ifname || '\0' == ifname[0])
    {
        NETMGR_ERR("入参 ifname 非法");
        return 0;
    }

    fd = link_sock_open();
    if (fd < 0)
        return 0;

    strncpy(req.ifr_name, ifname, IFNAMSIZ - 1);
    ret = ioctl(fd, SIOCGIFFLAGS, &req);
    err = errno;
    close(fd);
    if (ret < 0)
    {
        NETMGR_ERR("读取 %s 标志失败, errno=%d", ifname, err);
        return 0;
    }
    return (req.ifr_flags & IFF_RUNNING) ? 1 : 0;
}

/**
 * @brief 使能或关闭接口
 * @param ifname 接口名，不可为空
 * @param on     1=启用（置 IFF_UP），0=关闭（清 IFF_UP）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_enable(const char* ifname, int on)
{
    struct ifreq req = {0};  /* ioctl 参数：接口名与接口标志 */
    int fd = -1;       /* 临时控制 socket */
    int ret = 0;       /* ioctl 返回值 */
    int err = 0;       /* 失败时的 errno 快照 */

    if (NULL == ifname || '\0' == ifname[0])
    {
        NETMGR_ERR("入参 ifname 非法");
        return -NETMGRERR_PARAM;
    }

    fd = link_sock_open();
    if (fd < 0)
        return fd;

    strncpy(req.ifr_name, ifname, IFNAMSIZ - 1);
    /* 先读出原有标志再改：直接赋值会抹掉接口上其它标志位 */
    if (ioctl(fd, SIOCGIFFLAGS, &req) < 0)
    {
        err = errno;
        close(fd);
        NETMGR_ERR("读取 %s 标志失败, errno=%d", ifname, err);
        return -NETMGRERR_IOCTL;
    }
    if (on)
        req.ifr_flags |= IFF_UP;
    else
        req.ifr_flags &= ~IFF_UP;

    ret = ioctl(fd, SIOCSIFFLAGS, &req);
    err = errno;
    close(fd);
    if (ret < 0)
    {
        NETMGR_ERR("设置 %s 标志失败, errno=%d", ifname, err);
        return -NETMGRERR_IOCTL;
    }
    return 0;
}

/**
 * @brief 读取接口当前 IPv4 地址
 * @param ifname 接口名，不可为空
 * @param ip     输出缓冲，不可为空
 * @param len    输出缓冲长度
 * @return 0 成功（接口尚无地址时返回 0 且输出空串）；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_ip(const char* ifname, char* ip, int len)
{
    struct ifreq req = {0};                 /* ioctl 参数：接口名与接口地址 */
    struct sockaddr_in* addr = NULL;  /* 接口地址视图 */
    int fd = -1;                      /* 临时控制 socket */
    int ret = 0;                      /* ioctl 返回值 */
    int err = 0;                      /* 失败时的 errno 快照 */

    if (NULL == ifname || NULL == ip || len <= 0)
    {
        NETMGR_ERR("入参非法");
        return -NETMGRERR_PARAM;
    }
    ip[0] = '\0';

    fd = link_sock_open();
    if (fd < 0)
        return fd;

    strncpy(req.ifr_name, ifname, IFNAMSIZ - 1);
    ret = ioctl(fd, SIOCGIFADDR, &req);
    err = errno;
    close(fd);
    if (ret < 0)
    {
        /* 接口尚未取得地址属于正常状态，只有真正的故障才记录日志 */
        if (EADDRNOTAVAIL != err)
            NETMGR_ERR("读取 %s 地址失败, errno=%d", ifname, err);
        return 0;
    }

    addr = (struct sockaddr_in*)&req.ifr_addr;
    if (NULL == inet_ntop(AF_INET, &addr->sin_addr, ip, (socklen_t)len))
    {
        NETMGR_ERR("转换 %s 地址文本失败, errno=%d", ifname, errno);
        ip[0] = '\0';
        return -NETMGRERR_IOCTL;
    }
    return 0;
}

/**
 * @brief 设置接口 IPv4 地址
 * @param ifname 接口名，不可为空
 * @param ip     点分十进制地址，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_setip(const char* ifname, const char* ip)
{
    struct ifreq req = {0};         /* ioctl 参数：接口名与待设地址 */
    struct sockaddr_in* addr = NULL; /* 待设地址视图 */
    int fd = -1;              /* 临时控制 socket */
    int ret = 0;              /* ioctl 返回值 */
    int err = 0;              /* 失败时的 errno 快照 */

    if (NULL == ifname || NULL == ip || !netmgr_ip_valid(ip))
    {
        NETMGR_ERR("入参非法, ifname=%s, ip=%s", (NULL == ifname) ? "" : ifname,
                   (NULL == ip) ? "" : ip);
        return -NETMGRERR_PARAM;
    }

    fd = link_sock_open();
    if (fd < 0)
        return fd;

    strncpy(req.ifr_name, ifname, IFNAMSIZ - 1);
    addr = (struct sockaddr_in*)&req.ifr_addr;
    addr->sin_family = AF_INET;
    if (1 != inet_pton(AF_INET, ip, &addr->sin_addr))
    {
        close(fd);
        NETMGR_ERR("转换 %s 地址失败", ip);
        return -NETMGRERR_PARAM;
    }

    ret = ioctl(fd, SIOCSIFADDR, &req);
    err = errno;
    close(fd);
    if (ret < 0)
    {
        NETMGR_ERR("设置 %s 地址失败, errno=%d", ifname, err);
        return -NETMGRERR_IOCTL;
    }
    return 0;
}

/**
 * @brief 设置接口 IPv4 掩码
 * @param ifname 接口名，不可为空
 * @param mask   点分十进制掩码，不可为空
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_setmask(const char* ifname, const char* mask)
{
    struct ifreq req = {0};         /* ioctl 参数：接口名与待设掩码 */
    struct sockaddr_in* addr = NULL; /* 待设掩码视图 */
    int fd = -1;              /* 临时控制 socket */
    int ret = 0;              /* ioctl 返回值 */
    int err = 0;              /* 失败时的 errno 快照 */

    if (NULL == ifname || NULL == mask || !netmgr_ip_valid(mask))
    {
        NETMGR_ERR("入参非法, ifname=%s", (NULL == ifname) ? "" : ifname);
        return -NETMGRERR_PARAM;
    }

    fd = link_sock_open();
    if (fd < 0)
        return fd;

    strncpy(req.ifr_name, ifname, IFNAMSIZ - 1);
    addr = (struct sockaddr_in*)&req.ifr_addr;
    addr->sin_family = AF_INET;
    if (1 != inet_pton(AF_INET, mask, &addr->sin_addr))
    {
        close(fd);
        NETMGR_ERR("转换 %s 掩码失败", mask);
        return -NETMGRERR_PARAM;
    }

    ret = ioctl(fd, SIOCSIFNETMASK, &req);
    err = errno;
    close(fd);
    if (ret < 0)
    {
        NETMGR_ERR("设置 %s 掩码失败, errno=%d", ifname, err);
        return -NETMGRERR_IOCTL;
    }
    return 0;
}

/**
 * @brief 校验 IPv4 点分十进制字符串合法性
 * @param ip 待校验字符串，不可为空
 * @return 1 合法；0 非法
 */
int netmgr_ip_valid(const char* ip)
{
    struct in_addr addr = {0}; /* 转换结果容器 */

    if (NULL == ip || '\0' == ip[0])
    {
        NETMGR_ERR("入参 ip 非法");
        return 0;
    }
    return (1 == inet_pton(AF_INET, ip, &addr)) ? 1 : 0;
}

/**
 * @brief 在候选接口名中查找第一个存在的接口
 * @param names 候选接口名数组，元素不可为空
 * @param count 候选数量
 * @param out   输出接口名缓冲，不可为空
 * @param len   输出缓冲长度
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
int netmgr_dev_find(const char* const* names, int count, char* out, int len)
{
    int i = 0; /* 候选下标，从 0 开始 */

    if (NULL == names || count <= 0 || NULL == out || len <= 0)
    {
        NETMGR_ERR("入参非法, count=%d", count);
        return -NETMGRERR_PARAM;
    }
    out[0] = '\0';

    for (i = 0; i < count; ++i)
    {
        if (NULL == names[i] || '\0' == names[i][0])
            continue;
        if (!netmgr_link_exist(names[i]))
            continue;
        if ((int)strlen(names[i]) >= len)
        {
            NETMGR_ERR("输出缓冲不足, 需要 %d 字节", (int)strlen(names[i]) + 1);
            return -NETMGRERR_BUFF;
        }
        snprintf(out, (size_t)len, "%s", names[i]);
        return 0;
    }

    NETMGR_ERR("候选接口均不存在, 数量=%d", count);
    return -NETMGRERR_NOTFOUND;
}

/**
 * @brief 按「物理连通 + 已取地址 + 有默认网关」判定链路是否就绪并刷新快照
 * @param ctx 链路上下文，不可为空
 * @return 0 成功（未就绪时状态置为 DOWN）；负值为 NETMGRERR_* 错误码
 */
int netmgr_link_ready(NETMGR_CTX* ctx)
{
    NETMGR_ROUTE route = {0}; /* 该接口上的默认路由 */
    int ret = 0;        /* 子步骤返回值 */

    if (NULL == ctx)
    {
        NETMGR_ERR("入参 ctx 为空");
        return -NETMGRERR_PARAM;
    }

    ctx->ip[0] = '\0';
    ctx->gate[0] = '\0';

    /* 第 1 步：物理连通性，未连通直接判为不可用 */
    if (!netmgr_link_up(ctx->ifname))
    {
        ctx->state = NETMGR_ST_DOWN;
        return 0;
    }

    /* 第 2 步：地址是否已取得 */
    ret = netmgr_link_ip(ctx->ifname, ctx->ip, (int)sizeof(ctx->ip));
    if (ret < 0)
        return ret;
    if ('\0' == ctx->ip[0])
    {
        ctx->state = NETMGR_ST_DOWN;
        return 0;
    }

    /* 第 3 步：默认网关，由 DHCP 客户端写入路由表，此处回查 */
    ret = netmgr_route_dev(ctx->ifname, &route);
    if (ret < 0)
    {
        ctx->state = NETMGR_ST_DOWN;
        return 0;
    }

    snprintf(ctx->gate, (size_t)sizeof(ctx->gate), "%s", route.gate);
    ctx->state = NETMGR_ST_READY;
    return 0;
}
