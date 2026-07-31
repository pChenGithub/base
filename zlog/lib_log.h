#ifndef __LIB_LOG_H__
#define __LIB_LOG_H__
#ifdef __cplusplus
extern "C" {
#endif
#include "include/zlog.h"
#include "lib_logErr.h"

#define LOG_I(fomat, args...) ({dzlog_info(fomat, ## args);})
// 打印debug日志标记，设置 1，LOG_D 会打印，否则不打印
extern char lib_log_debug_flag;
#define LOG_D(fomat, args...) ({if(1==lib_log_debug_flag)dzlog_debug(fomat, ## args);})
#define LOG_E(fomat, args...) ({dzlog_error(fomat, ## args);})
#define LOG_TEST() ({dzlog_debug("[D]" "*** a test line ***");})

int lib_log_init(const char *conf);
void lib_log_deinit();
#ifdef __cplusplus
}
#endif
#endif

