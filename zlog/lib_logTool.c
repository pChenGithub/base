#include "lib_logTool.h"
#include "mmc_opt.h"
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "global.h"
#include "file_opt.h"
#include <string.h>

// 自定义过滤器：排除目录
static int filter_dir(const struct dirent *entry) {
    if (NULL==entry)
        return 0;
    //LOG_I("文件 %s 类型 %d", entry->d_name, entry->d_type);
    // 返回非目录项
    if (DT_DIR==entry->d_type)
        return 0;
    return 1;
}
// 删除指定目录早期的5个日志文件
// file 返回删除的最后一个文件名，可以为 NULL
#define LIB_LOG_DEL_MAX 5   // 最大删除文件个数
#define LIB_LOG_RETAIN  2   // 要求保留文件个数
static int delPre5Logs(const char* logDir, char* filename, int len) {
    // /zyckdata/zyck_scb_adapterEAi/log/2025-11-21.txt
    char fullPath[64] = {0};
    int ret = 0;
    if (NULL==logDir)
        return -LIBLOG_ERR_CHECKPARAM;
    char* retname = NULL;
    int delcount = LIB_LOG_DEL_MAX;     // 默认删除5个文件
    // 按时间，名称获取文件列表，并且去除目录
    struct dirent** namelist = NULL;
    int count = scandir(logDir, &namelist, filter_dir, alphasort);
    //LOG_I("文件个数 %d", count);
    if (count<0)
        return -LIBLOG_ERR_SCANDIR;
    if (0==count) {
        ret = -LIBLOG_ERR_HASNO_FILE;
        goto free_namelist;
    }
    // 文件不足2个，无需删除，保留
    if (count<=LIB_LOG_RETAIN)
        goto free_namelist;
    if (count<(LIB_LOG_DEL_MAX+LIB_LOG_RETAIN))
        delcount = count-LIB_LOG_RETAIN;
#if 1
    // 升序
    for (int i=0;i<delcount;i++) {
        LOG_I("文件名称：%s", namelist[i]->d_name);
        snprintf(fullPath, sizeof(fullPath), "%s/%s", logDir, namelist[i]->d_name);
        LOG_I("超大小删除文件 %s", fullPath);
        ret = file_remove(fullPath);
        if (ret<0) {
            // 删除文件失败，终止
            ret = -LIBLOG_ERR_REMOVE_FILE;
            break;
        }
        retname = namelist[i]->d_name;  // 记录最新删除成功的文件名称
    }
#else
    // 降序
    while (n) {
        n--;
        LOG_I("文件名称：%s", namelist[n]->d_name);
    }
#endif
    // 返回最后删除的文件名称
    if (NULL!=filename && len>0 && NULL!=retname)
        strCopyC(filename, len, retname);

free_namelist:
    for (int i=0;i<count;i++)
        free(namelist[i]);  // 清理内存
    free(namelist);         // 清理内存
    return ret;
}
// percent 为20% 的 20
int delBigLogs(const char* logDir, int percent) {
    return delBigFile(logDir, percent, NULL, 0);
}

// overTime 为过期时间，单位是 秒
int delOverdueLogs(const char* logDir, int overTime) {
    int ret = 0;
    // /zyckdata/zyck_scb_adapterEAi/log/2025-11-21.txt
    char fullPath[64] = {0};
    struct stat sb;
    // 检查过期日志
    if (NULL==logDir || overTime<=0)
        return -LIBLOG_ERR_CHECKPARAM;
    DIR* pdir = opendir(logDir);
    if (NULL==pdir)
        return -LIBLOG_ERR_OPENDIR;
    struct dirent* pfile = NULL;
    // 获取当前时间戳
    time_t ct = time(NULL);
    LOG_I("当前时间 %ld", ct);
    ct -= overTime;
    LOG_I("过期时间 %ld", ct);
    while (NULL!=(pfile=readdir(pdir))) {
        // 目录文件无需处理
        if (DT_DIR==pfile->d_type)
            continue ;
        snprintf(fullPath, sizeof(fullPath), "%s/%s", logDir, pfile->d_name);
        /**
            struct stat {
            mode_t st_mode; // 文件类型和权限
            ino_t st_ino; // inode 节点号
            dev_t st_dev; // 设备号
            dev_t st_rdev; // 特殊设备号
            nlink_t st_nlink; // 文件的硬链接数
            uid_t st_uid; // 文件所有者的用户ID
            gid_t st_gid; // 文件所有者的组ID
            off_t st_size; // 文件大小（字节数）
            time_t st_atime; // 文件最后访问时间
            time_t st_mtime; // 文件内容最后修改时间
            time_t st_ctime; // 文件状态改变时间
            blksize_t st_blksize; // 文件系统的块大小
            blkcnt_t st_blocks; // 文件所占的块数
            }; 
        */
        if (0!=stat(fullPath, &sb)) {
            ret = -LIBLOG_ERR_STAT_FILE;
            goto close_exit;
        }
        //LOG_I("文件名称：%s", pfile->d_name);
        //LOG_I("文件修改时间 %ld", sb.st_mtime);
        if (sb.st_mtime<ct) {
            LOG_I("超时间删除文件 %s", fullPath);
            if (file_remove(fullPath)<0)  {
                ret = -LIBLOG_ERR_REMOVE_FILE;
                goto close_exit;
            }
        }
    }
    
close_exit:
    closedir(pdir);
    return ret;
}


int delBigFile(const char *logDir, int percent, char *filename, int filenameLen) {
    int ret = 0;
    if (NULL==logDir || percent<=0 || percent>100)
        return -LIBLOG_ERR_CHECKPARAM;
    // 检查存储空间
    int percent_free = mmc_getFreePercent(logDir);
    if(-1 == percent_free)
        return -LIBLOG_ERR_GET_FREEPERCENT;
    if (percent_free > percent)
        return 0;
    // 循环检查最多检查10次
    char count = 10;
    LOG_I("空闲空间 %d", percent_free);
    //while((count--) && percent_free < percent) {
    while((count--) && 1) {
        // 控件超过设定值，考虑删除日志文件，直至回到阈值以下
        ret = delPre5Logs(logDir, filename, filenameLen);
        if (ret<0)
            return ret;
        //percent_free = mmc_getFreePercent(logDir);
    }
    return 0;
}
