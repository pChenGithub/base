#include "debug_opt.h"
#include "timeopt.h"
#include <stdio.h>
#include <stdio.h>
#include <execinfo.h>
#include <string.h>
#include <stdlib.h>
#include "global.h"

#define MAX_ARRAY_SP    64
int debug_get_crash_2_file(const char* file, char flag_append) {
    if (NULL==file)
        return -DEBUGOPTERR_CHECKPARAM;
    char* option = "wb";
    if (flag_append)
        option = "ab";
    // 打开文件
    FILE* fp = fopen(file, option);
    if (NULL==fp)
        return -DEBUGOPTERR_FOPEN_FAIL;

    // 写入标题
    fprintf(fp, "###### crash info start ######\n");
    // 时间
    fprintf(fp, "datetime: ");
    // %Y%m%d%H%M%S 2023-01-04 16:11:00
    char format_time[20] = {0};
    getStdDateTimeStr(format_time, sizeof(format_time), "%Y-%m-%d %H:%M:%S");
    fprintf(fp, "%s\n", format_time);

    // 获取信息
    void* array_sp[MAX_ARRAY_SP] = {0};
    // 获取完整栈大小
    size_t size = backtrace(array_sp, MAX_ARRAY_SP);
    LOG_I("获取栈大小 %zu/%d\n", size, MAX_ARRAY_SP);
    // 写入文件
    char** info = backtrace_symbols(array_sp, size);
    if (NULL!=info) {
        fprintf(fp, "stack trace:\n");
        for (size_t i=0;i<size;i++) {
            fprintf(fp, "%zu %s\n", i, info[i]);
        }
        //
        free(info);info = NULL;
    }

    // 写入结尾
    fprintf(fp, "###### crash info end ######\n\n");
    // 关闭
    fclose(fp);
    return 0;
}
