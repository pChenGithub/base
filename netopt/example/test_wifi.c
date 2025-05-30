#include <stdio.h>
#include "wifi_opt.h"

int main(int argc, char const *argv[])
{
    // 开启wifi
    int ret = wifi_sta_enable();
    if (ret<0)
        printf("[%d]错误 %d\n", __LINE__, ret);
    // 执行扫描wifi命令
    ret = wifi_sta_scan();
    if (ret<0)
        printf("[%d]错误 %d\n", __LINE__, ret);
    return 0;
}


