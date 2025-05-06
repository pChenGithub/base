#include "mac_opt.h"
#include "string_opt.h"
#include "file_opt.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <pthread.h>

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

double get_cpu_temperature(const char* file)
{
    int ret = get_cpu_temperature_raw(file);
    if (ret<0)
        return ret;
    // 转成2位小数
    ret /= 10;
    return (ret/100.0);
}

int get_cpu_temperature_raw(const char *file)
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

double get_mem_Usagerate()
{
    struct sysinfo s_info;
    if (0!=sysinfo(&s_info))
        return -SYSOPTERR_GET_SYSINFO;
    // 计算
    double tmp = 10000*(s_info.totalram - s_info.freeram)/s_info.totalram;
    //LOG_I("***** %f", tmp);
    return (tmp/100.0);
}

typedef struct {
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;
    unsigned long long guest;
    unsigned long long guest_nice;
} CPU_TIME_INFO;
typedef struct {
    CPU_TIME_INFO old;
    CPU_TIME_INFO cur;
    int flag;           // 标记是否初始化，非0表示初始化
} TMP_CPU_RAET;
static pthread_mutex_t lock_rate = PTHREAD_MUTEX_INITIALIZER;
static TMP_CPU_RAET info = {
    {0}, {0}, 0,
};

static int read_cpu_time(CPU_TIME_INFO *info) {
    if (NULL==info)
        return -MACOPTERR_CHECKPARAM;
    FILE *fp = fopen("/proc/stat", "r");
    if (fp == NULL)
        return -SYSOPTERR_OPEN_FILE;
    char line[128] = {0};
    if (NULL==fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -SYSOPTERR_READ_FILE;
    }
    //LOG_I("******* %s", line);
    //
    sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
           &info->user, &info->nice, &info->system, &info->idle,
           &info->iowait, &info->irq, &info->softirq, &info->steal,
           &info->guest, &info->guest_nice);
    fclose(fp);
    return 0;
}

static double calculate_cpu_usage(CPU_TIME_INFO *prev, CPU_TIME_INFO *curr) {
    unsigned long long prev_total = prev->user + prev->nice + prev->system + prev->idle +
                                    prev->iowait + prev->irq + prev->softirq + prev->steal;
    unsigned long long curr_total = curr->user + curr->nice + curr->system + curr->idle +
                                    curr->iowait + curr->irq + curr->softirq + curr->steal;
    unsigned long long total_diff = curr_total - prev_total;

    unsigned long long prev_idle = prev->idle + prev->iowait;
    unsigned long long curr_idle = curr->idle + curr->iowait;
    unsigned long long idle_diff = curr_idle - prev_idle;

    double tmp = 10000*(total_diff - idle_diff) / total_diff;
    //LOG_I("**** %f", tmp);
    return (tmp/100.0);
}

double get_cpu_Usagerate()
{
    double ret = 0;
    // 加锁
    if (0!=pthread_mutex_trylock(&lock_rate)) {
        usleep(10000);
        if (0!=pthread_mutex_trylock(&lock_rate))
            return -SYSOPTERR_GET_LOCK;
    }
    if (0==info.flag) {
        // 没有读过，读取信息初始化old
        ret = read_cpu_time(&info.old);
        if (ret<0)
            goto exit_end;
        info.flag = 1;  // 读完第一次，标记读过
        goto exit_end;
    }
    // 读新的信息之前，先备份旧的
    memcpy(&info.old, &info.cur, sizeof(CPU_TIME_INFO));
    // 读过了，读去信息初始化 cur
    ret = read_cpu_time(&info.cur);
    if (ret<0)
        goto exit_end;
    // 计算使用率
    ret = calculate_cpu_usage(&info.old, &info.cur);

exit_end:
    // 解锁
    pthread_mutex_unlock(&lock_rate);
    return ret;
}
