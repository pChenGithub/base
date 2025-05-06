#ifndef __MAC_OPT_H__
#define __MAC_OPT_H__
#ifdef __cplusplus
extern "C" {
#endif

#define MACOPTERR_CHECKPARAM        1
#define MACOPTERR_FOPEN_FAIL        2
#define MACOPTERR_FREAD_FAIL        3
#define MACOPTERR_HEXSTR_TOARRAY    4   // 16进制的字符串转数组失败
#define SYSOPTERR_READ_FILE         5   // 读取文件失败
#define SYSOPTERR_GET_SYSINFO       6   // 获取系统信息失败
#define SYSOPTERR_OPEN_FILE         7   // 打开文件失败
#define SYSOPTERR_READ_FILE         8   // 读取文件失败
#define SYSOPTERR_GET_LOCK          9   // 获取锁失败

int mac_to_snstr(const char *mac_file, char* buf, int len);
int mac_to_snarray(const char* mac_file, char* buf, int len);
// 获取cpu温度原始数据
int get_cpu_temperature_raw(const char *file);
// 获取cpu温度，返回2为小数
double get_cpu_temperature(const char *file);
// 获取cpu使用率
double get_cpu_Usagerate();
// 获取内存使用率，保留2位小数
double get_mem_Usagerate();
// 获取存储使用率

#ifdef __cplusplus
}
#endif
#endif
