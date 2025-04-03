#include "lib_log.h"

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
