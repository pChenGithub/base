#ifndef __WEBSERVER_H__
#define __WEBSERVER_H__
#ifdef __cplusplus
extern "C" {
#endif
#include <pthread.h>
#include "web_config.h"
// 错误码
#define WEBAPP_ERR_CHECKPARAM       1   // 参数异常
#define WEBAPP_ERR_MALLOC           2   // 分配内存失败
#define WEBAPP_ERR_SET_LISTEN       3   // 设置web监听失败
#define WEBAPP_ERR_THREAD_CREATE    4   // 线程创建失败
// 外部实现的路径关系
typedef struct {
    const char* listenAddr; // web监听地址
    const char* rootDir;    // web根目录
    WEB_API_PATH* api_path; // web服务监听的路径集合
    //
    void* webApp;           // web实例，无需用户设置
} WEB_USER_CONF;
//
int webAppStart(WEB_USER_CONF* conf);
int webAppStop(WEB_USER_CONF* conf);
#ifdef __cplusplus
}
#endif
#endif
