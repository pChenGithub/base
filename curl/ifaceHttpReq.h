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

typedef int (*HTTP_REPLY_CONTENT_FUNC)(const char* buffer, int len, void* arg);

typedef enum {
    RET_NONE,   // 无返回值
    RET_FILE,   // 返回文件
    RET_JSON,   // 返回内容json
} HTTP_REPLY_TYPE;

typedef struct {
    HTTP_REPLY_TYPE type;          // 是否有返回值，或者指定返回类型
    HTTP_REPLY_CONTENT_FUNC opt;
    void* p;                        // 文件描述符或者指向一段内存，如果http请求成功
    int len;                        // 如果有返回内容，len指定给分配的缓存大小
    int offset;                     // 标记存储偏移
    int ret;                        // 带回回调的错误
} HTTP_REPLY;


// http请求接口，这里的type是为了用户指定返回的内容
int httpReq(HTTP_REPLY_TYPE type ,const char* url, const char* indata, char* outdata, const int outdatalen);
int httpGetFileReq(HTTP_REPLY_TYPE type, const char* url, const char* indata, char* outdata, const int outdatalen);
// http请求超时接口
int httpReqTimeout(HTTP_REPLY_TYPE type ,const char* url, const char* indata,
                   char* outdata, const int outdatalen, int timeoutMs);
int httpGetReq(HTTP_REPLY_TYPE type ,const char* url, const char* indata, char* outdata, const int outdatalen);
int httpPutReq(HTTP_REPLY_TYPE type ,const char* url, const char* indata, char* outdata, const int outdatalen);
// http上传文件
int httpUploadFile(const char* url, char* data, int len, char *admin, char *pass);
int httpPutUploadBinaryFile(const char* url, char* data);



#ifdef __cplusplus
}
#endif

#endif /*_IFACEHTTPREQ_H_*/



