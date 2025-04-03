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


