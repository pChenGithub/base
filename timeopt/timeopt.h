#ifndef __TIMEOPT_H__
#define __TIMEOPT_H__
#ifdef __cplusplus
extern "C" {
#endif
#define TIMEOPT_ERR_CHECKPARAM      1
#define TIMEOPT_ERR_LOCALTIME       2   // 日历时间格式化本地时间失败
#define TIMEOPT_ERR_STRFTIME        3   // 格式化输出时间字符串失败
#define TIMEOPT_ERR_GETTIMEOFDAY    4   // 获取gettimeofday失败
// https://blog.csdn.net/gerryzhu/article/details/50371255  不同的时间和api讲解
int getStdTime(char* hour, char* min, char* sec);
int getStdDate(char* year, char* month, char* day);
// 获取年月日时分秒,数字返回(6个字节)
int getStdDateTime(char dt[6]);
// 获取年月日,数字返回(3个字节)
int getArrayDate(char dt[3]);
// 获取时分秒,数字返回(3个字节)
int getArrayTime(char dt[3]);
//------------------------------------------
// 获取秒,字符串返回
char *getSTime(char* s, int slen);
// 获取毫秒,字符串返回
char* getMsTime(char* ms, int mslen);
// 获取时间,年月日,时分秒,字符串返回,年保留2个字节
int getStdDateTimeStr(char* dt, int dtlen, const char* format);
//------------------------------------------
// 设置系统时间,,格式化的时间,2023-06-06 06:06:06
int setSysTime(const char* formatTime, char syncHw);
// 设置系统时间,读取rtc时间
int setSysTimeByRTC();
//------------------------------------------
// 获取与当前的时间差
int getTimeDiff(int compareTime);
#ifdef __cplusplus
}
#endif
#endif


