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

int mac_to_snstr(const char *mac_file, char* buf, int len);
int mac_to_snarray(const char* mac_file, char* buf, int len);
// 获取cpu温度
int get_cpu_temperature(const char *file);
// 获取mem使用率

#ifdef __cplusplus
}
#endif
#endif
