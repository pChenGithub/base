#include "lib_log.h"

char lib_log_debug_flag = 0;    // 打印debug日志标记
int lib_log_init(const char* conf)
{
    if (NULL==conf)
        return -LIBLOG_ERR_CHECKPARAM;
    if (0!=dzlog_init(conf, "zyck"))
        return -1;
    return 0;
}

void lib_log_deinit()
{
    zlog_fini();
}
