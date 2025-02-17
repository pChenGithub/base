#include "timeopt.h"
#include "mixShell.h"
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int getStdTime(char* hour, char* min, char* sec) {
    (void)hour;
    (void)min;
    (void)sec;
    return 0;
}

int getStdDate(char* year, char* month, char* day) {
    (void)year;
    (void)month;
    (void)day;
    return 0;
}

// 从 0-5,表示 年月日时分秒
int getStdDateTime(char dt[6]) {
    if (NULL==dt)
        return -TIMEOPT_ERR_CHECKPARAM;

    struct tm tmptm;
    time_t tnow = time(NULL);
    // 对比localtime,localtime_r是线程安全的
    if (NULL==localtime_r(&tnow, &tmptm))
        return -TIMEOPT_ERR_LOCALTIME;

    dt[0] = tmptm.tm_year % 100;
    dt[1] = tmptm.tm_mon+1;
    dt[2] = tmptm.tm_mday;

    dt[3] = tmptm.tm_hour;
    dt[4] = tmptm.tm_min;
    dt[5] = tmptm.tm_sec;

    return 0;
}

char *getMsTime(char *ms, int mslen)
{
    if (NULL==ms || mslen<=0)
        return NULL;
    struct timeval tv;
    if (0!=gettimeofday(&tv,NULL))
        return NULL;
    //printf("xx %d s, %d us\n", tv.tv_sec, tv.tv_usec);
    long long int tms = 0;
    tms = tv.tv_sec;
    tms *= 1000;
    tms += tv.tv_usec/1000;
    snprintf(ms, mslen, "%lld", tms);
    return ms;
}

// %Y%m%d%H%M%S 20230104161100
// %y%m%d%H%M%S 230104161100
int getStdDateTimeStr(char *dt, int dtlen, const char *format)
{
    if (NULL==dt || dtlen<=0 || NULL==format)
        return -TIMEOPT_ERR_CHECKPARAM;

    struct tm tmptm;
    time_t tnow = time(NULL);
    if (NULL==localtime_r(&tnow, &tmptm))
        return -TIMEOPT_ERR_LOCALTIME;
    if (strftime(dt, dtlen, format, &tmptm)<=0)
        return -TIMEOPT_ERR_STRFTIME;
    return 0;
}

char* getSTime(char *s, int slen)
{
    if (NULL==s || slen<=0)
        return NULL;
    time_t t = time(NULL);
    snprintf(s, slen, "%ld", t);
    return s;
}

int setSysTime(const char *formatTime, char syncHw)
{
    // date -s '2023-06-06 06:06:06'    29
    // date -s '2023-06-06 06:06:06';hwclock -wl
    char cmd[64] = {0};
    if (NULL==formatTime)
        return -TIMEOPT_ERR_CHECKPARAM;

    if (19!=strlen(formatTime) || '-'!=formatTime[4] || '-'!=formatTime[7])
        return -TIMEOPT_ERR_CHECKPARAM;

    snprintf(cmd, 64, "date -s '%s'", formatTime);
    if (syncHw)
        snprintf(cmd+29, 45, ";hwclock -wl");

    // 执行
    system(cmd);

    return 0;
}

int getArrayDate(char dt[3])
{
    if (NULL==dt)
        return -TIMEOPT_ERR_CHECKPARAM;

    struct tm tmptm;
    time_t tnow = time(NULL);
    // 对比localtime,localtime_r是线程安全的
    if (NULL==localtime_r(&tnow, &tmptm))
        return -TIMEOPT_ERR_LOCALTIME;

    dt[0] = tmptm.tm_year % 100;
    dt[1] = tmptm.tm_mon+1;
    dt[2] = tmptm.tm_mday;

    return 0;
}

int getArrayTime(char dt[3])
{
    if (NULL==dt)
        return -TIMEOPT_ERR_CHECKPARAM;

    struct tm tmptm;
    time_t tnow = time(NULL);
    // 对比localtime,localtime_r是线程安全的
    if (NULL==localtime_r(&tnow, &tmptm))
        return -TIMEOPT_ERR_LOCALTIME;

    dt[0] = tmptm.tm_hour;
    dt[1] = tmptm.tm_min;
    dt[2] = tmptm.tm_sec;

    return 0;
}

int getTimeDiff(int compareTime)
{
    int timeOff = 0;
    time_t tnow = time(NULL);

    if(tnow >= compareTime)
    {
        timeOff = tnow - compareTime;
    }
    else
    {
        timeOff = compareTime - tnow;
    }

    return timeOff;
}

int setSysTimeByRTC()
{
    char tmpstr[20] = {0};
    exeShell("hwclock -s", tmpstr, 20);
    printf("<== %s\n", tmpstr);
    return 0;
}
