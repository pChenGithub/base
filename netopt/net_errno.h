/** 
 * 定义网络接口的错误码
*/
#ifndef __NET_ERRNO_H__
#define __NET_ERRNO_H__
#define NETERR_CHECK_PARAM  1
#define NETERR_FOPEN_FAIL   2   // 执行fopen出错
#define NETERR_FREAD_FAIL   3   // 执行fread出错
#define NETERR_FREAD_ZERO   4   // 执行fread读出0长度内容
#define NETERR_SOCKET_FAIL  5   // 执行socket失败，创建socket失败
#define NETERR_SOCKET_GIFCONF_FAIL      6   // socket获取ifconf失败
#define NETERR_HASNO_NETIFNAME          7   // 没有找到指定的网络接口名称
#define NETERR_SOCKET_GIFADDR_FAIL      8   // socket获取ifaddr(ip地址)失败
#define NETERR_SOCKET_GIFNETMASK_FAIL   9   // socket获取ifnetmask(掩码地址)失败
#define NETERR_SOCKET_SIFADDR_FAIL      10  // socket设置ifaddr(ip地址)失败
#define NETERR_SOCKET_SIFNETMASK_FAIL   11  // socket设置ifnetmask(掩码地址)失败
#define NETERR_BIND_FAIL                12  // 执行bind失败
#define NETERR_PTHREADCREATE_FAIL       13  // 创建线程失败
#define NETERR_NETLINK_ISRUNNING        14  // netlink服务已经开启（重复开启服务线程）
#define NETERR_INVALID_IP               15  // 无效的ip地址
#define NETERR_INVALID_MASK             16  // 无效的掩码
#define NETERR_INVALID_GATEWAY          17  // 无效的网关地址
#define NETERR_INVALID_GATEDIST         18  // 无效的网关目标
#define NETERR_SOCKET_ADDRT             19  // socket设置网关失败
#define NETERR_SOCKET_RTMSG             20  // socket获取网关失败
#define NETERR_REQMEM                   21  // 申请内存失败
#define NETERR_SOCKET_RECVFAIL          22  // 对socket执行recv失败
#define NETERR_SOCKET_SENDFAIL          23  // 对socket执行send失败
#define NETERR_INVALID_NLMSG            24  // 无效的nlmsg
#define NETERR_INVALID_RTMSG            25  // 无效的rtmsg
#define NETERR_BUFFER_NOTENOUGH         26  // 输入缓存空间不足
#define NETERR_FGETS_FALI               27  // 执行fges失败
#define NETERR_FWRITE_FALI              28  // 执行fwrite失败
#define NETERR_SOCKET_GIFFLAGS_FAIL     29   // socket获取ifflags(flag)失败
#define NETERR_SOCKET_SIFFLAGS_FAIL     30   // socket设置ifflags(flag)失败
#define NETERR_FINDNO_ENTRY             31   // 没有找到合适节点
#define NETERR_REGCOMP_FAIL             32   // 编译正则表达式失败
#define NETERR_REGEXEC_FAIL             33   // 正则表达式匹配失败
#define NETERR_SOCKET_ADDRT_EXSIT       34  // 设置网关失败，具体为网关存在
#define NETERR_DHCP_STOPTHREAD          35  // 取消dhcp线程失败
#define NETERR_DHCP_HASTHREAD           36  // 已经创建指定接口dhcp线程
#define NETERR_SELECT                   37  // 执行select错误
#define NETERR_SELECT_TIMEOUT           38  // 执行select超时
#endif
