/**
 * @file net_mgr_iface.c
 * @brief netmgr 系统适配层：网络接口操作实现（ioctl）
 * @details 仅依赖内核与 common 层的错误码与校验，不依赖 sys 层其它能力。
 */
#include "net_mgr_iface.h"

#include "net_mgr_err.h"
#include "net_mgr_util.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

/**
 * @brief 打开一次性的接口控制 socket
 * @return 成功返回 socket 描述符；失败返回负的 NETMGRERR_* 错误码
 */
static int iface_sock_open(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0); /* 接口控制类 ioctl 只需 UDP socket */

    if (fd < 0)
    {
        return -NETMGRERR_SOCKET;
    }
    return fd;
}

/**
 * @brief 判断网络接口是否存在
 * @param ifname 接口名，不可为空
 * @return 1 存在；0 不存在或入参非法
 */
int netmgr_iface_exist(const char* ifname)
{
    if (NULL == ifname || '\0' == ifname[0])
    {
        return 0;
    }
    return (0 != if_nametoindex(ifname)) ? 1 : 0;
}

/**
 * @brief 判断接口是否可用（就绪）
 * @details 管理态 IFF_UP 与 IFF_RUNNING 同时置位才算就绪；内核自 2.6.36 起
 *          在 IFF_RUNNING 中反映 carrier（operstate），故不再另读 sysfs
 * @param ifname 接口名，不可为空
 * @return 1 可用；0 不可用（管理态关闭 / 无载波 / 查询失败）
 */
int netmgr_iface_ready(const char* ifname)
{
    struct ifreq req = {0};  /* ioctl 参数：接口名与接口标志 */
    int fd = -1;       /* 临时控制 socket */
    int ret = 0;       /* ioctl 返回值 */

    if (NULL == ifname || '\0' == ifname[0])
    {
        return 0;
    }

    fd = iface_sock_open();
    if (fd < 0)
        return 0;

    snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
    ret = ioctl(fd, SIOCGIFFLAGS, &req);
    close(fd);
    if (ret < 0)
    {
        return 0;
    }

    /* 管理态与载波同时就位才算可用：IFF_RUNNING 由内核按 carrier（operstate）推导。
       前提为目标内核 ≥ 2.6.36（此版本起 IFF_RUNNING 才含 carrier）；
       更老内核需改为「读 sysfs carrier 优先、不可读再退回 IFF_RUNNING」 */
    return ((req.ifr_flags & IFF_UP) && (req.ifr_flags & IFF_RUNNING)) ? 1 : 0;
}

/**
 * @brief 使能或关闭接口
 * @param ifname 接口名，不可为空
 * @param on     1=启用（置 IFF_UP），0=关闭（清 IFF_UP）
 * @return 0 成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_iface_enable(const char* ifname, int on)
{
    struct ifreq req = {0};  /* ioctl 参数：接口名与接口标志 */
    int fd = -1;       /* 临时控制 socket */
    int ret = 0;       /* ioctl 返回值 */

    if (NULL == ifname || '\0' == ifname[0])
    {
        return -NETMGRERR_PARAM;
    }

    fd = iface_sock_open();
    if (fd < 0)
        return fd;

    snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
    /* 先读出原有标志再改：直接赋值会抹掉接口上其它标志位 */
    if (ioctl(fd, SIOCGIFFLAGS, &req) < 0)
    {
        close(fd);
        return -NETMGRERR_IOCTL;
    }
    if (on)
        req.ifr_flags |= IFF_UP;
    else
        req.ifr_flags &= ~IFF_UP;

    ret = ioctl(fd, SIOCSIFFLAGS, &req);
    close(fd);
    if (ret < 0)
    {
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
int netmgr_iface_ip(const char* ifname, char* ip, int len)
{
    struct ifreq req = {0};           /* ioctl 参数：接口名与接口地址 */
    struct sockaddr_in* addr = NULL;  /* 接口地址视图 */
    int fd = -1;                      /* 临时控制 socket */
    int ret = 0;                      /* ioctl 返回值 */
    int err = 0;                      /* 失败时的 errno 快照 */

    if (NULL == ifname || NULL == ip || len <= 0)
    {
        return -NETMGRERR_PARAM;
    }
    ip[0] = '\0';

    fd = iface_sock_open();
    if (fd < 0)
        return fd;

    snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
    ret = ioctl(fd, SIOCGIFADDR, &req);
    err = errno;
    close(fd);
    if (ret < 0)
    {
        /* 接口尚未取得地址属于正常状态，其余失败按异常返回错误码 */
        return (EADDRNOTAVAIL == err) ? 0 : -NETMGRERR_IOCTL;
    }

    addr = (struct sockaddr_in*)&req.ifr_addr;
    if (NULL == inet_ntop(AF_INET, &addr->sin_addr, ip, (socklen_t)len))
    {
        ip[0] = '\0';
        return -NETMGRERR_IOCTL;
    }
    return 0;
}

/**
 * @brief 读取接口当前 IPv4 掩码
 * @param ifname 接口名，不可为空
 * @param mask   输出缓冲，不可为空
 * @param len    输出缓冲长度
 * @return 0 成功（接口尚无地址时返回 0 且输出空串）；负值为 NETMGRERR_* 错误码
 */
int netmgr_iface_mask(const char* ifname, char* mask, int len)
{
    struct ifreq req = {0};           /* ioctl 参数：接口名与接口掩码 */
    struct sockaddr_in* addr = NULL;  /* 接口掩码视图 */
    int fd = -1;                      /* 临时控制 socket */
    int ret = 0;                      /* ioctl 返回值 */
    int err = 0;                      /* 失败时的 errno 快照 */

    if (NULL == ifname || NULL == mask || len <= 0)
    {
        return -NETMGRERR_PARAM;
    }
    mask[0] = '\0';

    fd = iface_sock_open();
    if (fd < 0)
        return fd;

    snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
    ret = ioctl(fd, SIOCGIFNETMASK, &req);
    err = errno;
    close(fd);
    if (ret < 0)
    {
        /* 接口尚未取得地址属于正常状态，其余失败按异常返回错误码 */
        return (EADDRNOTAVAIL == err) ? 0 : -NETMGRERR_IOCTL;
    }

    addr = (struct sockaddr_in*)&req.ifr_netmask;
    if (NULL == inet_ntop(AF_INET, &addr->sin_addr, mask, (socklen_t)len))
    {
        mask[0] = '\0';
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
int netmgr_iface_setip(const char* ifname, const char* ip)
{
    struct ifreq req = {0};          /* ioctl 参数：接口名与待设地址 */
    struct sockaddr_in* addr = NULL; /* 待设地址视图 */
    int fd = -1;                     /* 临时控制 socket */
    int ret = 0;                     /* ioctl 返回值 */

    if (NULL == ifname || NULL == ip || !netmgr_ip_valid(ip))
    {
        return -NETMGRERR_PARAM;
    }

    fd = iface_sock_open();
    if (fd < 0)
        return fd;

    snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
    addr = (struct sockaddr_in*)&req.ifr_addr;
    addr->sin_family = AF_INET;
    if (1 != inet_pton(AF_INET, ip, &addr->sin_addr))
    {
        close(fd);
        return -NETMGRERR_PARAM;
    }

    ret = ioctl(fd, SIOCSIFADDR, &req);
    close(fd);
    if (ret < 0)
    {
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
int netmgr_iface_setmask(const char* ifname, const char* mask)
{
    struct ifreq req = {0};          /* ioctl 参数：接口名与待设掩码 */
    struct sockaddr_in* addr = NULL; /* 待设掩码视图 */
    int fd = -1;                     /* 临时控制 socket */
    int ret = 0;                     /* ioctl 返回值 */

    if (NULL == ifname || NULL == mask || !netmgr_ip_valid(mask))
    {
        return -NETMGRERR_PARAM;
    }

    fd = iface_sock_open();
    if (fd < 0)
        return fd;

    snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
    addr = (struct sockaddr_in*)&req.ifr_addr;
    addr->sin_family = AF_INET;
    if (1 != inet_pton(AF_INET, mask, &addr->sin_addr))
    {
        close(fd);
        return -NETMGRERR_PARAM;
    }

    ret = ioctl(fd, SIOCSIFNETMASK, &req);
    close(fd);
    if (ret < 0)
    {
        return -NETMGRERR_IOCTL;
    }
    return 0;
}

/**
 * @brief 在候选接口名中查找第一个存在的接口
 * @param names 候选接口名数组，元素不可为空
 * @param count 候选数量
 * @param out   输出接口名缓冲，不可为空
 * @param len   输出缓冲长度
 * @return 0 找到；负值为 NETMGRERR_* 错误码
 */
int netmgr_iface_find(const char* const* names, int count, char* out, int len)
{
    int i = 0; /* 候选下标，从 0 开始 */

    if (NULL == names || count <= 0 || NULL == out || len <= 0)
    {
        return -NETMGRERR_PARAM;
    }
    out[0] = '\0';

    for (i = 0; i < count; ++i)
    {
        if (NULL == names[i] || '\0' == names[i][0])
            continue;
        if (!netmgr_iface_exist(names[i]))
            continue;
        if ((int)strlen(names[i]) >= len)
        {
            return -NETMGRERR_BUFF;
        }
        snprintf(out, (size_t)len, "%s", names[i]);
        return 0;
    }

    return -NETMGRERR_NOTFOUND;
}
