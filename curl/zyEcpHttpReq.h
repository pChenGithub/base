/**
 * http请求接口，
 * 功能：
 * 1、http请求，返回json字符串
 * 2、http请求，返回文件（指定返回文件路径）
 * 3、http请求，上传文件
 * 4、返回请求结果，成功/失败
*/
#ifndef _ZYECPHTTPREQ_H_
#define _ZYECPHTTPREQ_H_
#ifdef __cplusplus
extern "C" {
#endif
#include "ifaceHttpReq.h"

#define HTTPERROR_PARSE_JSON        100 // 解析json失败
#define HTTPERROR_GET_FILED_VAL     101 // 获取json字段值失败
#define HTTPERROR_GET_TOKEN         102 // 获取token失败

int httpGetToken(const char* url, const char* appid, const char *appsecret);
int httpAddEcpDev(const char* url, const char* indata, char* outdata, const int outdatalen);
int httpDelEcpDev(const char* url, const char* device_sn, const char *app_secret, char *outdata, const int outdatalen);
// 获取一类设备的属性
int httpGetproperty(const char* url, const char* product_key, char *outdata, const int outdatalen);
int httpGetpropertyBysn(const char* url, const char* sn, char *outdata, const int outdatalen);
// 下发设备属性
int httpSetproperty(const char* url, const char *indata,
                    char *outdata, const int outdatalen);
// 下发设备命令
int httpSetCmd(const char* url, const char *indata,
                    char *outdata, const int outdatalen);
// 广播设备命令
int httpBroadcastCmd(const char* url, const char *indata,
                     char *outdata, const int outdatalen);

#ifdef __cplusplus
}
#endif
#endif /*_ZYECPHTTPREQ_H_*/



