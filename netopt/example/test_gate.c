#include <stdio.h>
#include <string.h>
#include "link/net_opt.h"
#include "link/net_netlink.h"

int main(int argc, char const *argv[])
{
    int ret = 0;
#if 1
#if 0
    // 设置网关
    //ret = set_gateway("ens33", "192.168.0.3", "255.255.0.0", "192.168.1.1");
    // 添加默认网关
    //ret = set_gateway("ens33", "192.168.0.1", NULL, NULL);

    // 添加指定目标网关，
    //ret = set_gateway("ens33", "192.168.0.15", "255.255.255.0", "192.168.0.0");
    //if (ret<0)
    //{
    //    printf("设置网关失败，错误码%d\n", ret);
    //}

    // 设置网关，如果存在，就删除
    // name gate mask dist
    ret = replace_gateway("ens33", "192.168.0.1", NULL, NULL);
    if (ret<0)
    {
        printf("设置网关失败，错误码%d\n", ret);
    }
    printf("ret %d\n", ret);
#else
    ROUTE_LIST list[2] = {0};
    int len = 2;
    //ret = nl_getGateways("default", NULL, list, &len);
    ret = nl_getGateways(NULL, NULL, NULL, list, &len);
    //ret = get_gateway(NULL, NULL, 0);
    if (ret<0)
        printf("获取网关失败，错误码%d\n", ret);

    int i = 0;
    printf("获取个数 %d\n", len);
    for (i=0;i<len;i++) {
        printf("%s %s %s\n", list[i].dist, list[i].gate, list[i].devname);
    }
#endif
#else
    if (argc<3) {
        printf("参数不足\n");
        return 0;
    }

    const char* dist = NULL;
    if (strncmp(argv[1], "null", 5))
        dist = argv[1];

    const char* mask = NULL;
    if (strncmp(argv[2], "null", 5))
        mask = argv[2];

    ret = del_gateway("ens33", dist, mask);
    if (ret<0)
        printf("删除网关失败，错误码 %d\n", ret);

#endif        
    return 0;
}
