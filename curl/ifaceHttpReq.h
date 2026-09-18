/**
 * http请求接口，
 * 功能：
 * 1、http请求，返回json字符串
 * 2、http请求，返回文件（指定返回文件路径）
 * 3、http请求，上传文件
 * 4、返回请求结果，成功/失败
*/
#ifndef _IFACEHTTPREQ_H_
#define _IFACEHTTPREQ_H_

#ifdef __cplusplus
extern "C" {
#endif

#define HTTPERROR_CHECK_PAREM       1   // 参数检查失败
#define HTTPERROR_OPENFILE          2   // 打开文件失败
#define HTTPERROR_CURL_INIT         3   // curl init失败
#define HTTPERROR_CURL_PERFORM      4   // curl preform失败
#define HTTPERROR_BUFF_NOTENOUGH    5   // 输入缓存不足
#define HTTPERROR_OPERATION_TIMEDOUT    6   // 请求超时
#define HTTPERROR_COULDNT_CONNECT   7   // 服务器不能连接
#define HTTPERROR_MALLOC            8   // 分配内存失败

/* ---------------- HTTP 传输层配置（内部使用，两个 .c 共用） ---------------- */
/* 说明：内网自签证书，按产品要求刻意关闭证书校验，勿误判为缺陷；
 *       日后需要开启时只改下面三个取值——VERIFY_STATUS 置 1L 开 OCSP、
 *       VERIFY_PEER 置 1L 开证书链校验、VERIFY_HOST 置 2L 开域名校验 */
#define HTTP_SSL_VERIFY_STATUS  0L  // OCSP 装订校验开关
#define HTTP_SSL_VERIFY_PEER    0L  // 服务器证书链校验开关
#define HTTP_SSL_VERIFY_HOST    0L  // 证书域名校验开关

/**
 * http 返回内容处理回调
 * @param buffer 本次接收到的数据
 * @param len    数据长度(字节)
 * @param arg    用户上下文指针（HTTP_REPLY*）
 * @return 0 成功，负值为错误码
 */
typedef int (*HTTP_REPLY_CONTENT_FUNC)(const char* buffer, int len, void* arg);

/** http 返回内容类型 */
typedef enum {
    RET_NONE,   // 无返回值
    RET_FILE,   // 返回文件
    RET_JSON,   // 返回内容json
} HTTP_REPLY_TYPE;

/** http 返回内容上下文 */
typedef struct {
    HTTP_REPLY_TYPE type;          // 是否有返回值，或者指定返回类型
    HTTP_REPLY_CONTENT_FUNC opt;   // 返回内容处理回调
    void* p;                        // 文件描述符或者指向一段内存，如果http请求成功
    int len;                        // 如果有返回内容，len指定给分配的缓存大小
    int offset;                     // 标记存储偏移
    int ret;                        // 带回回调的错误
} HTTP_REPLY;


/**
 * http 请求（POST，固定超时 10s）
 * @param type       返回内容类型：RET_NONE 无返回 / RET_FILE 写入文件 / RET_JSON 写入内存
 * @param url        请求地址
 * @param indata     请求体(json 字符串)；type=RET_FILE 时该参数为目标文件路径
 * @param outdata    返回内容缓冲（type=RET_JSON 时使用）
 * @param outdatalen outdata 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpReq(HTTP_REPLY_TYPE type ,const char* url, const char* indata, char* outdata, const int outdatalen);

/**
 * http 获取文件请求（当前实现与 httpReq 一致）
 * @param type       返回内容类型，取文件时应传 RET_FILE
 * @param url        请求地址
 * @param indata     接收文件的保存路径
 * @param outdata    返回内容缓冲（type=RET_JSON 时使用）
 * @param outdatalen outdata 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpGetFileReq(HTTP_REPLY_TYPE type, const char* url, const char* indata, char* outdata, const int outdatalen);

/**
 * http 请求（POST，自定义超时）
 * @param type       返回内容类型
 * @param url        请求地址
 * @param indata     请求体(json 字符串)；type=RET_FILE 时为目标文件路径
 * @param outdata    返回内容缓冲（type=RET_JSON 时使用）
 * @param outdatalen outdata 缓冲长度
 * @param timeoutMs  请求超时(毫秒)
 * @return 0 成功，负值为错误码
 */
int httpReqTimeout(HTTP_REPLY_TYPE type ,const char* url, const char* indata,
                   char* outdata, const int outdatalen, int timeoutMs);

/**
 * http GET 请求（固定超时 10s）
 * @param type       返回内容类型
 * @param url        请求地址
 * @param indata     请求体(json 字符串)；type=RET_FILE 时为目标文件路径
 * @param outdata    返回内容缓冲（type=RET_JSON 时使用）
 * @param outdatalen outdata 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpGetReq(HTTP_REPLY_TYPE type ,const char* url, const char* indata, char* outdata, const int outdatalen);

/**
 * http PUT 请求（固定超时 10s）
 * @param type       返回内容类型
 * @param url        请求地址
 * @param indata     请求体(json 字符串)；type=RET_FILE 时为目标文件路径
 * @param outdata    返回内容缓冲（type=RET_JSON 时使用）
 * @param outdatalen outdata 缓冲长度
 * @return 0 成功，负值为错误码
 */
int httpPutReq(HTTP_REPLY_TYPE type ,const char* url, const char* indata, char* outdata, const int outdatalen);

/**
 * http 上传文件（multipart/form-data，固定超时 10s）
 * @param url   请求地址
 * @param data  待上传文件的本地路径；调用后该缓冲将被返回内容覆盖（同一缓冲兼作返回）
 * @param len   data 缓冲长度（即返回内容可写入的最大长度）
 * @param admin 认证用户名
 * @param pass  认证密码
 * @return 0 成功，负值为错误码
 */
int httpUploadFile(const char* url, char* data, int len, char *admin, char *pass);

/**
 * http 二进制文件上传（PUT，固定超时 10s）
 * @param url  请求地址
 * @param data 待上传文件的本地路径
 * @return 0 成功，负值为错误码
 */
int httpPutUploadBinaryFile(const char* url, char* data);



#ifdef __cplusplus
}
#endif

#endif /*_IFACEHTTPREQ_H_*/



