/**
 * @file net_m_cell.c
 * @brief net 4G 蜂窝链路实现：ECM 网卡 + DHCP（net 层）
 * @details 当前业务暂无 4G 需求，本文件保留完整兼容接口与 ECM 通路：
 *          mode 为 PPP / QMI 时在分配资源前直接返回 -NETMGRERR_NOTSUP，
 *          待接入方式明确后再补齐拨号实现，不在本层留下半成品状态。
 */
#include "net_m_ctx.h"

#include "net_mgr_dhcp.h"
#include "net_mgr_err.h"
#include "net_mgr_iface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** 4G 链路私有数据 */
typedef struct {
    NET_M_HEAD head;       /**< 公共头（链路类型），须为首成员供 DHCP 回调使用 */
    NET_M_CELL_CONF conf;  /**< 配置副本，生命周期与链路上下文一致 */
} NET_M_CELL_PRIV;

static int cell_open(NET_M_LINK_RT* link, const void* conf);
static int cell_close(NET_M_LINK_RT* link);
static int cell_refresh(NET_M_LINK_RT* link);

/** 4G 链路操作表；DHCP 由 sys 层自行重试，故不提供自愈钩子 */
const NET_M_OPS net_m_cell_ops = {cell_open, cell_close, cell_refresh, NULL};

static int cell_open(NET_M_LINK_RT* link, const void* conf)
{
    const NET_M_CELL_CONF* cfg = (const NET_M_CELL_CONF*)conf; /* 入参配置视图 */
    const char* const devs[] = {"usb0", "wwan0", "eth1"};      /* ECM 常见命名 */
    NET_M_CELL_PRIV* priv = NULL;                              /* 私有数据 */
    int devNum = (int)(sizeof(devs) / sizeof(devs[0]));        /* 候选接口数量 */
    int ret = 0;                                               /* 子步骤返回值 */

    if (NULL == link || NULL == cfg)
    {
        return -NETMGRERR_PARAM;
    }

    /* 第 1 步：接入方式校验，未实现的拨号方式在分配资源前拒绝 */
    if (NET_M_CELL_ECM != cfg->mode)
    {
        return -NETMGRERR_NOTSUP;
    }

    priv = (NET_M_CELL_PRIV*)calloc(1, sizeof(NET_M_CELL_PRIV));
    if (NULL == priv)
    {
        return -NETMGRERR_MEM;
    }
    memcpy(&priv->conf, cfg, sizeof(priv->conf));
    priv->head.type = NET_M_CELL;
    priv->conf.mode = NET_M_CELL_ECM;

    /* 第 2 步：确定接口名，未指定时按候选列表自动探测 */
    if ('\0' == priv->conf.ifname[0])
    {
        ret = netmgr_iface_find(devs, devNum, priv->conf.ifname,
                                (int)sizeof(priv->conf.ifname));
        if (ret < 0)
        {
            free(priv);
            return ret;
        }
    }
    if (!netmgr_iface_exist(priv->conf.ifname))
    {
        free(priv);
        return -NETMGRERR_NOTFOUND;
    }

    /* 第 3 步：启用接口并发起 DHCP，地址异步获取 */
    ret = netmgr_iface_enable(priv->conf.ifname, 1);
    if (ret < 0)
    {
        free(priv);
        return ret;
    }
    ret = netmgr_dhcp_start(priv->conf.ifname, net_m_dhcp_notify, priv);
    if (ret < 0)
    {
        free(priv);
        return ret;
    }

    /* 第 4 步：全部成功后才挂载上下文 */
    (void)snprintf(link->ifname, sizeof(link->ifname), "%s", priv->conf.ifname);
    (void)snprintf(link->dns, sizeof(link->dns), "%s", priv->conf.dns);
    link->priv = priv;
    return 0;
}

static int cell_close(NET_M_LINK_RT* link)
{
    int err = 0; /* 首个错误码 */
    int ret = 0; /* 子步骤返回值 */

    if (NULL == link)
    {
        return -NETMGRERR_PARAM;
    }

    if ('\0' != link->ifname[0])
    {
        /* 停止取址失败不阻断资源释放，错误码在末尾上抛 */
        ret = netmgr_dhcp_stop(link->ifname);
        if (ret < 0)
        {
            err = ret;
        }

        /* 关闭接口管理态：与 ip link set <ifname> down 等效 */
        ret = netmgr_iface_enable(link->ifname, 0);
        if (ret < 0 && 0 == err)
        {
            err = ret;
        }
    }

    free(link->priv);
    link->priv = NULL;

    link->ifname[0] = '\0';
    link->ip[0] = '\0';
    link->mask[0] = '\0';
    link->gate[0] = '\0';
    link->dns[0] = '\0';
    link->state = NET_M_ST_DOWN;
    link->route_on = 0;
    link->err = 0;
    link->dhcpSeen = 0;
    link->dhcpErr = 0;
    return err;
}

static int cell_refresh(NET_M_LINK_RT* link)
{
    if (NULL == link || NULL == link->priv)
    {
        return -NETMGRERR_PARAM;
    }

    /* 4G 恒为 DHCP 取址：接口消失、就绪判定与 DOWN 归因共用公共实现 */
    return net_m_link_ready_dhcp(link);
}
