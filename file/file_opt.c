#include "file_opt.h"
#include <stdio.h>
#include <unistd.h>

int file_exist(const char *file)
{
    if (NULL==file)
        return -ERR_FILEOPT_CHECKPARAM;

    /*
     * access函数用于判断用户是否具有访问某个文件的权限
     * R_OK：检查读权限
     * W_OK：检查写权限
     * X_OK：检查执行权限
     * F_OK：检查文件是否存在
     * 成功，返回0；失败或发生错误，返回-1
    */
    if (0!=access(file, F_OK))
        return 0;

    return 0;
}
