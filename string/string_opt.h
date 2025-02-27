#ifndef __STRING_OPT_H__
#define __STRING_OPT_H__
#ifdef __cplusplus
extern "C" {
#endif

#define STROPTERR_CHECKPARAM        1   // 参数检查错误
//-----------------------------------------------------
// 16进制字符串,转16进制数组, "112233"转0x112233
int hexStr2Array(char *out, unsigned int len, const char *in);
// 16进制数组,转16进制字符串, 比如 0x112233转"112233"
char* hexArray2Str(char* arrayin, int inlen, char* strout, int outlen);
// 16进制字符串,去除后面的补0, 比如 "1234560000"转成0x123456
int hexStrStriptail(char* hexstr);
// 输入一个字节,用16进制表示其整数, 比如 0x1617转成1617
int hexArray2Int(const char* in, int inlen);
// 单个字节转整数, 比如 0x16转16
int hex2Int(const char in);
//-----------------------------------------------------
// 16进制的字符串转成longlong, 比如 "112233"转0x112233,返回longlong值
long long hexStr2ll(const char* str);
// 16进制的字符串转成longlong, 比如 "112233"转成0x332211
long long hexStrLittle2ll(const char* str);
// 16进制的字符串转成10进制字符串,比如 "112233"转成 "3351057"(0x332211)
int hexstrLittle2Decstr(const char* str, char* out, int len);
//-----------------------------------------------------
// 拷贝字符串,如果buff不够长,会截断
int strCopyC(char* buff, int len, const char* str);
#define STR_COPY(buff, bufflen, str) ({ \
    int strlenght = strlen(str); \
    strlenght = strlenght<bufflen?strlenght:bufflen-1; \
    memcpy(buff, str, strlenght); \
    buff[strlenght] = 0; \
})
//-----------------------------------------------------
// 数字转字符串,需要指定buff,并会返回这个buff
char* ll2Str(char* out, int outlen, long long in);
char* i2Str(char* out, int outlen, int in);
char* C2Str(char* out, int outlen, char in);
//-----------------------------------------------------
// 数组转字符串,
//-----------------------------------------------------
// 4字节IP转字符串,ipBytes是char[4]类型
char* ipByte2Str(unsigned char* ipBytes, char* out, int outlen);
// IP转4字节数字
int ipStr2Byte4(unsigned char* ipBytes, const char* ip);
// 从url字符串获取主机域名/IP
char *getHostFromUrl(const char* url, char* host, int len);
//-----------------------------------------------------
// 打印日志
void printLog(const char* file, int line, const char *func, const char *fomat, ...);
#ifdef __cplusplus
}
#endif
#endif

