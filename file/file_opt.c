#include "file_opt.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
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
        return -ERR_FILEOPT_NOT_EXIST;
    return 0;
}

long file_size(const char *file)
{
    if (NULL==file)
        return -ERR_FILEOPT_CHECKPARAM;
    struct stat sb;
    if (0!=stat(file, &sb))
        return -ERR_FILEOPT_STAT;
    return (sb.st_size);
}

int file_read(const char *file, char *buf, unsigned int buflen)
{
    if (NULL==file || NULL==buf || 0==buflen)
        return -ERR_FILEOPT_CHECKPARAM;
    FILE* fp = fopen(file, "rb");
    if (NULL==fp)
        return -ERR_FILEOPT_FILE_OPEN;
    int cnt = fread(buf, buflen, 1, fp);
    fclose(fp);
    return cnt;
}
