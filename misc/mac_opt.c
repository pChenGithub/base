#include "mac_opt.h"
#include "string_opt.h"
#include "file_opt.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

//读取mac并转换成sn
int mac_to_snstr(const char* mac_file, char* buf, int len) {
    int ret = 0;
    char mac[18] = {0};
    if (NULL==mac_file||NULL==buf || len<13)
        return -MACOPTERR_CHECKPARAM;

    FILE* fp = fopen(mac_file, "r");
    if (NULL==fp)
        return -MACOPTERR_FOPEN_FAIL;

    ret = fread(mac, 1, 18, fp);
    if (18!=ret) {
        ret = -MACOPTERR_FREAD_FAIL;
        goto exit;
    }

    int j = 0;
    int i = 0;
    for (i=0;i<12;)
    {
        buf[i] = toupper(mac[j]);
        buf[i+1] = toupper(mac[j+1]);
        j +=3;
        i +=2;
    }

    buf[len-1] = 0;


exit:
    fclose(fp);
    fp = NULL;
    return ret;
}

int mac_to_snarray(const char* mac_file, char* buf, int len) {
    int ret = 0;
    char mac[18] = {0};
    if (NULL==buf || len<6)
        return -MACOPTERR_CHECKPARAM;

    ret = mac_to_snstr(mac_file, mac, 18);
    if (ret<0)
        return ret;

    //
    printf("获取到的mac地址 %s\n", mac);
    if (hexStr2Array(buf, len, mac)<0)
        return -MACOPTERR_HEXSTR_TOARRAY;

    return 0;
}

int get_cpu_temperature(const char* file)
{
    if (NULL==file)
        return -MACOPTERR_CHECKPARAM;
    char buff[8] = {0};
    int ret = file_read(file, buff, sizeof(buff));
    if (ret<0)
    {
        return -SYSOPTERR_READ_FILE;
    }
    //printf("温度 %s\n", buff);
    return (atoi(buff));
}
