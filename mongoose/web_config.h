#ifndef __WEB_CONFIG_H__
#define __WEB_CONFIG_H__
#ifdef __cplusplus
extern "C" {
#endif
#include "../json/cJSON.h"
#include "mongoose.h"
// 数组结束符
#define WEB_API_PATH_EOF    -1
// 发送成功回调函数
typedef void (*HTTP_SEND_OK)();
// 路径命中回调函数,返回错误码，0正常
typedef void (*HAND_HTTP_REQ)(cJSON* rsp, const void* body, HTTP_SEND_OK* send_ok);
//
typedef struct
{
    struct mg_connection* c;
    struct mg_http_message* hm;
} LINK_FILE_ARG;
// 类型
typedef enum
{
    TYPE_REQ_POST = 0,  // 默认是post请求json，返回json
    TYPE_REQ_POST_FILE, // post方式上传文件
    TYPE_REQ_LINK_FILE, // 链接方式下载文件
} HTTP_REQ_TYPE;
// aip配置
typedef struct _api_path
{
    int num;
    const char* path;
    struct _api_path* next_level;   // 下一级的路径，指向 WEB_API_PATH数组
    HAND_HTTP_REQ func_hand;
    HTTP_REQ_TYPE type;             // 请求类型
} WEB_API_PATH;
// 配置定义
//#define WEB_ROOT_DIR    PROJECT_DIR"/webapp"
//#define WEB_ROOT_DIR    "./webapp"
//#define WEB_URL_PORT    "8001"
//#define WEB_LISTEN_ADDR "http://0.0.0.0:" WEB_URL_PORT
// http服务错误码
#define WEB_CODE_OK     0   // 正确返回
#define WEB_CODE_ERR    -1  // 错误返回，未知错误
// http请求返回消息封装接口
void web_code_rsp(cJSON *rsp, int code, const char* msg, cJSON* data);
void web_code_rsp_str(cJSON *rsp, int code, const char* msg, char* str);
void web_code_rsp_int(cJSON *rsp, int code, const char* msg, int val);

#ifdef __cplusplus
}
#endif
#endif
