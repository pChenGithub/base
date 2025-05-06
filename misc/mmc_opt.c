#include "mmc_opt.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/statvfs.h>

/**
 * @brief mmc_getsize
 *  获取文件大小
 * @param dev
 * @return
 */
unsigned long long mmc_getsize(const char* dev) {
    unsigned long long size = 0;
    if (NULL==dev)
        return -MMCOPTERR_CHECKPARAM;
    int fd = open(dev, O_RDONLY);
    if (fd<0)
        return -MMCOPTERR_OPENDEV;
    if (ioctl(fd, BLKGETSIZE64, &size)<0) {
        close(fd);
        return -MMCOPTERR_BLKGETSIZE64;
    }
    close(fd);
    return size;
}

/**
 * @brief mmc_getFreePercent
 *  获取剩余空间
 * @param path
 * @return
 */
int mmc_getFreePercent(const char* path) {
    struct statvfs sta = {0};
    int percent = 0;
    if (NULL==path)
        return -MMCOPTERR_CHECKPARAM;
    if (statvfs(path, &sta)<0)
        return -MMCOPTERR_STAVFS;
    //LOG_I("分区信息：%ld, %ld, %ld, %ld\n", sta.f_bsize, sta.f_frsize, sta.f_bfree, sta.f_blocks);
    percent = 100*sta.f_bfree/sta.f_blocks;
    return percent;
}

#if 0
struct statvfs {
    unsigned long  f_bsize;    /* Filesystem block size 文件系统块大小 */
    unsigned long  f_frsize;   /* Fragment size 碎片大小 */
    fsblkcnt_t     f_blocks;   /* Size of fs in f_frsize units  */
    fsblkcnt_t     f_bfree;    /* Number of free blocks 空闲块数量 */
    fsblkcnt_t     f_bavail;   /* Number of free blocks for
                                             unprivileged users 非特权用户的空闲块数量 */
    fsfilcnt_t     f_files;    /* Number of inodes i节点数量 */
    fsfilcnt_t     f_ffree;    /* Number of free inodes 空闲i节点数量 */
    fsfilcnt_t     f_favail;   /* Number of free inodes for
                                             unprivileged users 非特权用户的空闲i节点数量 */
    unsigned long  f_fsid;     /* Filesystem ID 文件系统id */
    unsigned long  f_flag;     /* Mount flags  挂载标识 */
    unsigned long  f_namemax;  /* Maximum filename length 最大文件名长度 */
};
#endif
double mmc_getUsagerate(const char* path)
{
    struct statvfs sta = {0};
    if (NULL==path)
        return -MMCOPTERR_CHECKPARAM;
    if (statvfs(path, &sta)<0)
        return -MMCOPTERR_STAVFS;
    //LOG_I("分区信息：%ld, %ld, %ld, %ld\n", sta.f_bsize, sta.f_frsize, sta.f_bfree, sta.f_blocks);
    double tmp = 10000*(sta.f_blocks-sta.f_bfree)/sta.f_blocks;
    return (tmp/100.0);
}
