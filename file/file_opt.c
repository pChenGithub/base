#include "file_opt.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

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

int dir_exist(const char *dir)
{
    return file_exist(dir);
}

int dir_create(const char *dir)
{
    int ret = 0;
    if (NULL==dir)
        return -ERR_FILEOPT_CHECKPARAM;
    int len = strlen(dir);
    char* path = (char*)calloc(1, len+1);
    strcpy(path, dir);
    for (int i=1;i<len;i++) {
        if (path[i]!='/')
            continue ;
        path[i] = 0;
        if (0==access(path, F_OK)) {
            // 修改回来
            path[i] = '/';
            continue ;
        }
        // 一个新的目录，如果不存在，创建
        if (-1==mkdir(path, 0755)) {
            // 创建文件夹失败
            ret = -ERR_FILEOPT_DIR_CREATE;
            goto end_exit;
        }
        // 修改回来
        path[i] = '/';
    }
    // 处理最后一个，最后一个字符有肯能是/结尾，也可能是普通字符
    if ('/'!=path[len-1]) {
        // 创建最后一个目录
        if (-1==mkdir(path, 0755)) {
            // 创建文件夹失败
            ret = -ERR_FILEOPT_DIR_CREATE;
        }
    }

end_exit:
    free(path);
    path = NULL;
    return ret;
}

int file_write(const char *file, char *buf, unsigned int size)
{
    if (NULL==file||NULL==buf||0==size)
        return -ERR_FILEOPT_CHECKPARAM;
    FILE* fp = fopen(file, "wb");
    if (NULL==fp)
        return -ERR_FILEOPT_FILE_OPEN;
    int cnt = fwrite(buf, size, 1, fp);
    if (1!=cnt) {
        fclose(fp);
        return -ERR_FILEOPT_FILE_WRITE;
    }
    // 关闭
    fclose(fp);
    return cnt;
}

int file_append(const char *file, char *buf, unsigned int size)
{
    if (NULL==file||NULL==buf||0==size)
        return -ERR_FILEOPT_CHECKPARAM;
    FILE* fp = fopen(file, "ab");
    if (NULL==fp)
        return -ERR_FILEOPT_FILE_OPEN;
    int cnt = fwrite(buf, size, 1, fp);
    if (1!=cnt) {
        fclose(fp);
        return -ERR_FILEOPT_FILE_WRITE;
    }
    // 关闭
    fclose(fp);
    return cnt;
}

int file_write_fsync(const char *file, char *buf, unsigned int size)
{
    if (NULL==file||NULL==buf||0==size)
        return -ERR_FILEOPT_CHECKPARAM;
    FILE* fp = fopen(file, "wb");
    if (NULL==fp)
        return -ERR_FILEOPT_FILE_OPEN;
    int cnt = fwrite(buf, size, 1, fp);
    if (1!=cnt) {
        fclose(fp);
        return -ERR_FILEOPT_FILE_WRITE;
    }
    // 刷新到硬盘
    fflush(fp);
    int fd = fileno(fp);
    if (-1!=fd)
        fsync(fd);
    // 关闭
    fclose(fp);
    return cnt;

}
