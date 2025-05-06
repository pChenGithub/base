#ifndef __MMC_OPT_H__
#define __MMC_OPT_H__
#ifdef __cplusplus
extern "C" {
#endif

#define MMCOPTERR_CHECKPARAM    1   // 检查参数失败
#define MMCOPTERR_OPENDEV       2   // 打开mmc设备失败
#define MMCOPTERR_BLKGETSIZE64  3   // 执行ioctr获取mmc大小错误
#define MMCOPTERR_STAVFS        4   // 执行stavfs失败

unsigned long long mmc_getsize(const char* dev);
unsigned long long mmc_getUseSize(const char* dev);
// 获取所指定路径所在分区的信息，参数：文件或者文件夹路径
int mmc_getFreePercent(const char* path);
// 获取存储使用率
// path 要获取磁盘信息的路径，只要是挂载到操作文件系统的路径即可
double mmc_getUsagerate(const char* path);

#ifdef __cplusplus
}
#endif
#endif
