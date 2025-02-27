#include "string_opt.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>

// 字符串的16进制转数值的16进制，比如 "AABB2345" 转成 char buf[] = {0xAA, 0xBB, 0x23, 0x45}
// 需要自己控制输入与输出的长度，函数内部只判断输出是不是为0
int hexStr2Array(char *out, unsigned int len, const char *in)
{
    if (NULL==out|| 0==len || NULL==in)
        return -1;

    int tmp = 0;
    unsigned int i = 0;
    for (i=0;i<len;i++) {
        tmp = in[2*i];
        if (tmp>='a')
            tmp -= ('a'-10);
        else if (tmp>='A')
            tmp -= ('A'-10);
        else
            tmp -= '0';

        out[i] = tmp*16;

        tmp = in[2*i+1];
        if (tmp>='a')
            tmp -= ('a'-10);
        else if (tmp>'A')
            tmp -= ('A'-10);
        else
            tmp -= '0';

        out[i] += tmp;
    }

    return 0;
}

int isHexItem() {
    return 0;
}

// 数组用16进制字符串表示
char *hexArray2Str(char* arrayin, int inlen, char* strout, int outlen) {
    if (NULL==arrayin || NULL==strout)
        return NULL;

    if (inlen>=(2*outlen))
        return NULL;

    int i = 0;
    for (i=0;i<inlen;i++) {
        snprintf(strout+2*i, 3, "%02x", arrayin[i]);
        //printf("%s\n", strout);
    }

    strout[outlen-1] = 0;
    return strout;
}

// 去除16进制字符串的后面补0,这里不见查是否是合法的 "112233",
// 会修改传入字符串
int hexStrStriptail(char* hexstr) {
    if (NULL==hexstr)
        return -STROPTERR_CHECKPARAM;

    int len = strlen(hexstr);
    if (len%2)
        return -STROPTERR_CHECKPARAM;

    int i = 0;
    for (i=(len-2);i>=0;i-=2)
    {
        if ('0'!=hexstr[i] || '0'!=hexstr[i+1])
            break;
        hexstr[i] = 0;
    }

    return 0;
}

int hexArray2Int(const char* in, int inlen) {
    if (NULL==in || inlen<=0)
        return -STROPTERR_CHECKPARAM;

    int ret = 0;
    int tmpl = 0;
    int tmph = 0;
    int i = 0;
    const char* pin = in;
    for (i=0;i<inlen;i++) {
        tmpl = (*pin)&0xf;
        tmph = (*pin)>>4;
        if (tmpl>9 || tmph>9)
            return -STROPTERR_CHECKPARAM;

        ret = ret*100+tmph*10+tmpl;
        pin++;
    }

    return ret;
}

int hex2Int(const char in) {
    return hexArray2Int(&in, 1);
}

// 拷贝字符串
int strCopyC(char *buff, int len, const char* str)
{
    if (NULL==buff || len<=0 || NULL==str)
        return -STROPTERR_CHECKPARAM;

    int strlenght = strlen(str);
    strlenght = strlenght<len?strlenght:len-1;

    memcpy(buff, str, strlenght);
    buff[strlenght] = 0;
    return 0;
}

char *ll2Str(char *out, int outlen, long long in)
{
    if (NULL==out || outlen<=0)
        return NULL;
    snprintf(out, outlen, "%lld", in);
    return out;
}

char *i2Str(char *out, int outlen, int in)
{
    if (NULL==out || outlen<=0)
        return NULL;
    snprintf(out, outlen, "%d", in);
    return out;
}

char *ipByte2Str(unsigned char *ipBytes, char *out, int outlen)
{
    if (NULL==ipBytes || NULL==out || outlen<=0)
        return NULL;
    snprintf(out, outlen, "%d.%d.%d.%d",
             ipBytes[0], ipBytes[1], ipBytes[2], ipBytes[3]);
    return out;
}

int ipStr2Byte4(unsigned char *ipBytes, const char *ip)
{
    int ip0=0, ip1=0, ip2=0, ip3=0;
    if (NULL==ipBytes || NULL==ip)
        return -1;
    sscanf(ip, "%d.%d.%d.%d", &ip0, &ip1, &ip2, &ip3);
    ipBytes[0] = ip0;
    ipBytes[1] = ip1;
    ipBytes[2] = ip2;
    ipBytes[3] = ip3;
    return 0;
}

char *getHostFromUrl(const char *url, char *host, int len)
{
    if (NULL==url || NULL==host || len<=0)
        return NULL;

    const char* ps = NULL;
    const char* pe = url;

    while (!('/'==pe[0] && '/'!=pe[1]))
    {
        if (':'==pe[0])
        {
            pe += 3;
            // 获取开头
            ps = pe;
            continue;
        }
        pe++;
    }

    int hostlen = pe-ps;
    if (len<=hostlen)
        return NULL;

    memcpy(host, ps, hostlen);
    host[hostlen] = 0;

    return host;
}

// 这个函数未考虑溢出
long long hexStr2ll(const char *str)
{
    if (NULL==str)
        return -1;

    long long ret = 0;
    int len = strlen(str);
    int i=0;
    for (i=0;i<len;i++)
    {
        ret = ret<< 4;
        if ('0'<=str[i] && str[i]<='9')
            ret += (str[i]-'0');
        else if ('a'<=str[i] && str[i]<='f')
            ret += (str[i]-'a'+10);
        else if ('A'<=str[i] && str[i]<='F')
            ret += (str[i]-'A'+10);
        else
            return -1;
    }
    return ret;
}

long long hexStrLittle2ll(const char *str)
{
    long long ret = 0;
    int i = 0;

    if (NULL==str)
        return -1;

    int len = strlen(str);
#if 0
    printf("长度 %d\n", len);
#endif
    for (i=len-2;i>=0;i-=2)
    {
        ret = ret << 4;
        if ('0'<=str[i] && str[i]<='9')
            ret += (str[i]-'0');
        else if ('a'<=str[i] && str[i]<='f')
            ret += (str[i]-'a'+10);
        else if ('A'<=str[i] && str[i]<='F')
            ret += (str[i]-'A'+10);
        else
            return -1;
#if 0
        printf("ret值 %lld\n", ret);
#endif

        ret = ret << 4;
        if ('0'<=str[i+1] && str[i+1]<='9')
            ret += (str[i+1]-'0');
        else if ('a'<=str[i+1] && str[i+1]<='f')
            ret += (str[i+1]-'a'+10);
        else if ('A'<=str[i+1] && str[i+1]<='F')
            ret += (str[i+1]-'A'+10);
        else
            return -1;
#if 0
        printf("ret值 %lld\n", ret);
#endif
    }
    return ret;
}

int hexstrLittle2Decstr(const char *str, char *out, int len)
{
    if (NULL==str || NULL==out || len<=0)
        return -1;
    snprintf(out, len, "%lld", hexStrLittle2ll(str));
    return 0;
}

char *C2Str(char *out, int outlen, char in)
{
    if (NULL==out || outlen<=0)
        return NULL;
    snprintf(out, outlen, "%d", in);
    return out;
}

void printLog(const char *file, int line, const char* func, const char* format, ...)
{
    if (NULL==file||NULL==func||NULL==format)
        return ;
    va_list args;
    // 获取当前时间
    struct timeval s_tv;
    struct tm s_ptm;
    char time_str[44] = {0};
    gettimeofday(&s_tv , NULL);
    if (NULL==localtime_r(&s_tv.tv_sec, &s_ptm))
        return ;
    // 02-27 10:08:00.100
    // mm-dd hh:mm:ss.ms
    snprintf(time_str, sizeof(time_str), "%02d-%02d %02d:%02d:%02d.%03ld",
             s_ptm.tm_mon+1,s_ptm.tm_mday,s_ptm.tm_hour,
             s_ptm.tm_min,s_ptm.tm_sec,s_tv.tv_usec/1000);
    // 去除文件案绝对路径
    const char* pfile = strrchr(file, '/')+1;
    if ((const char*)1==pfile)
        pfile = file;
    // 打印日志信息
    va_start(args, format);
    printf("[%s] %s:%d %s: ", time_str, pfile, line, func);
    vprintf(format, args);
    printf("\n");

    va_end(args);
    fflush(stdout);
}
