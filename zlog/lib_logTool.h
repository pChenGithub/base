#ifndef __LIB_LOGTOOL_H_
#define __LIB_LOGTOOL_H_
#ifdef __cplusplus
extern "C" {
#endif
#include "lib_logErr.h"

// 检查存储空间删除日志
int delBigLogs(const char* logDir, int percent);
// 检查存储空间，删除文件并返回删除最近的文件名
int delBigFile(const char* logDir, int percent, char* filename, int filenameLen);
// 检查过期日志删除
int delOverdueLogs(const char* logDir, int overTime);
#ifdef __cplusplus
}
#endif
#endif

