#include <stdio.h>
#include <unistd.h>
#include "wifi_opt.h"

int main(int argc, char const *argv[])
{
    // 开启wifi
    int ret = wifi_sta_enable();
    if (ret<0)
        printf("[%d]错误 %d\n", __LINE__, ret);
    sleep(1);
    // 执行扫描wifi命令
    #if 1
    ret = wifi_sta_scan();
    if (ret<0)
        printf("[%d]错误 %d\n", __LINE__, ret);
    #endif

    #if 0
    sleep(3);
    ret = wifi_sta_scan_result();
    if (ret<0)
        printf("[%d]错误 %d\n", __LINE__, ret);
    #endif

    // 连接wifi
    #if 1
    wifi_sta_connect("", "");
    #endif

    while (1)
    {
        /* code */
        sleep(1000);
    }
    
    return 0;
}


