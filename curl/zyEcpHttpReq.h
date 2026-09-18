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

/**
 * 获取 ECP 平台 access_token（未过期则直接返回，不重复请求）
 * @param url       平台地址（token 接口为 <url>/api/app/token）
 * @param appid     应用 ID
 * @param appsecret 应用密钥
 * @return 0 成功（含"未过期直接返回"），负值为错误码
 */
int httpGetToken(const char* url, const char* appid, const char *appsecret);

/**
 * 添加 ECP 设备
 * @param url        平台地址
 * @param indata     请求体(json 字符串)
 * @param outdata    返回内容缓冲
 * @param outdatalen 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpAddEcpDev(const char* url, const char* indata, char* outdata, const int outdatalen);

/**
 * 删除 ECP 设备
 * @param url        平台地址
 * @param device_sn  设备序列号
 * @param app_secret 应用密钥
 * @param outdata    返回内容缓冲
 * @param outdatalen 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpDelEcpDev(const char* url, const char* device_sn, const char *app_secret, char *outdata, const int outdatalen);

/**
 * 查询一类设备的属性列表（分页 page=1&pagesize=100）
 * @param url         平台地址
 * @param product_key 产品 key
 * @param outdata     返回内容缓冲
 * @param outdatalen  缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpGetproperty(const char* url, const char* product_key, char *outdata, const int outdatalen);

/**
 * 按设备序列号查询设备属性
 * @param url        平台地址
 * @param sn         设备序列号
 * @param outdata    返回内容缓冲
 * @param outdatalen 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpGetpropertyBysn(const char* url, const char* sn, char *outdata, const int outdatalen);

/**
 * 下发设备属性
 * @param url        平台地址
 * @param indata     请求体(json 字符串)
 * @param outdata    返回内容缓冲
 * @param outdatalen 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpSetproperty(const char* url, const char *indata,
                    char *outdata, const int outdatalen);

/**
 * 下发设备命令（path=/command/device_sn）
 * @param url        平台地址
 * @param indata     请求体(json 字符串)
 * @param outdata    返回内容缓冲
 * @param outdatalen 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpSetCmd(const char* url, const char *indata,
                    char *outdata, const int outdatalen);

/**
 * 广播设备命令（path=/command/broadcast）
 * @param url        平台地址
 * @param indata     请求体(json 字符串)
 * @param outdata    返回内容缓冲
 * @param outdatalen 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpBroadcastCmd(const char* url, const char *indata,
                     char *outdata, const int outdatalen);

/**
 * 获取场景实例列表（按设备序列号）
 * @param url        平台地址
 * @param sn         设备序列号
 * @param outdata    返回内容缓冲
 * @param outdatalen 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpGetScenemodeBysn(const char* url, const char* sn, char *outdata, const int outdatalen);

#ifdef __cplusplus
}
#endif
#endif /*_ZYECPHTTPREQ_H_*/



