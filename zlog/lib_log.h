#ifndef __LIB_LOG_H__
#define __LIB_LOG_H__
#ifdef __cplusplus
extern "C" {
#endif
#include "include/zlog.h"
#include "devmeta.h"
#define LIBLOG_ERR_CHECKPARAM   1   // 参数异常

#define LOG_I(fomat, args...) ({dzlog_info(fomat, ## args);})
extern ST_DEV_META g_dev_meta;
#define LOG_D(fomat, args...) ({if(STATUS_GENERAL_ON==g_dev_meta.statusInfo.flagDebug)dzlog_debug(fomat, ## args);})
#define LOG_E(fomat, args...) ({dzlog_error(fomat, ## args);})
#define LOG_TEST() ({dzlog_debug("[D]" "*** a test line ***");})

int lib_log_init(const char *conf);
void lib_log_deinit();
#ifdef __cplusplus
}
#endif
#endif

