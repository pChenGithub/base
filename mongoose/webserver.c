#include "webserver.h"
#include "mongoose.h"
#include "web_config.h"
#include <sys/prctl.h>

// web管理实例
typedef struct
{
    pthread_t pid;  // 轮询线程
    unsigned int flag;  // 状态标记，0为未初始化
    struct mg_mgr mMgr; // web实例
    struct mg_connection *mConnet;
    struct mg_connection *mWsConnet;
    HTTP_SEND_OK http_send_ok;  // 发送成功回调
    WEB_USER_CONF* userData;    // 用户设置参数
} WEB_APP;

//
static int web_code_msg(cJSON* rsp, int code, const char* msg)
{
    if (NULL==rsp)
    {
        printf("参数异常");
        return -1;
    }
    // 拼接回复消息
    cJSON_AddNumberToObject(rsp, "code", code);
    // msg
    if (NULL!=msg)
        cJSON_AddStringToObject(rsp, "msg", msg);
    else
        cJSON_AddStringToObject(rsp, "msg", "未知错误消息");
    return 0;
}
void web_code_rsp(cJSON* rsp, int code, const char* msg, cJSON* data)
{
    if (web_code_msg(rsp, code, msg)<0)
        return ;
    // data
    if (NULL!=data)
        cJSON_AddItemToObject(rsp, "data", data);
    else
        cJSON_AddNullToObject(rsp, "data");
}
void web_code_rsp_str(cJSON *rsp, int code, const char* msg, char* str)
{
    if (web_code_msg(rsp, code, msg)<0)
        return ;
    // data
    if (NULL!=str)
        cJSON_AddStringToObject(rsp, "data", str);
    else
        cJSON_AddNullToObject(rsp, "data");
}
void web_code_rsp_int(cJSON *rsp, int code, const char* msg, int val)
{
    if (web_code_msg(rsp, code, msg)<0)
        return ;
    // data
    cJSON_AddNumberToObject(rsp, "data", val);
}

// 匹配url路径
// 查询路径并执行业务，如果返回-1，表示无效路径，返回0，正常
static int match_url_path(const char* full_path, WEB_API_PATH* apis, WEB_API_PATH** api)
{
    int path_len = 0;
    if (NULL==full_path || NULL==apis || NULL==api)
    {
        printf("参数异常");
        return -1;
    }
    WEB_API_PATH* path = apis;
    for (;WEB_API_PATH_EOF!=path->num;path++)
    {
        path_len = strlen(path->path);
        if (!strncmp(full_path, path->path, path_len))
            break;
    }

    if (WEB_API_PATH_EOF==path->num)
    {
        printf("无效的路径");
        return -1;
    }
    // 判断是否是叶子
    if (0==path->path[path_len] && ' '==full_path[path_len])
    {
        //printf("%s 是叶子\n", path->path);
        // 是叶子，返回回调函数，成功返回
        *api = path;
        return 0;
    }
    //printf("%s 不是叶子\n", path->path);
    // 不是叶子
    full_path += (path_len-1);
    path = path->next_level;
    return match_url_path(full_path, path, api);
}

// 回调，处理http请求
//
static void webEventCb(struct mg_connection *c, int ev, void *ev_data, void *fn_data)
{
    WEB_APP* wApp = (WEB_APP*)fn_data;
    WEB_USER_CONF* userConf = wApp->userData;
    if (ev == MG_EV_HTTP_MSG)
    {
        // http请求
        struct mg_http_message *hm = (struct mg_http_message*)ev_data;
        //LOG_D("\n\n%s\n\n", hm->method.ptr);
        WEB_API_PATH* api = NULL;
        if (0==match_url_path(hm->uri.ptr, userConf->api_path, &api) && NULL!=api)
        {
            // 创建回复消息
            cJSON* root = NULL;
            // 执行回调
            if (api->func_hand)
            {
                wApp->http_send_ok = NULL;
                switch (api->type) {
                case TYPE_REQ_POST:
                    root = cJSON_CreateObject();
                    if (NULL==root)
                    {
                        printf("创建json失败");
                        return ;
                    }
                    api->func_hand(root, hm->body.ptr, &wApp->http_send_ok);
                    break;
                case TYPE_REQ_POST_FILE:
                    root = cJSON_CreateObject();
                    if (NULL==root)
                    {
                        printf("创建json失败");
                        return ;
                    }
                    api->func_hand(root, hm, &wApp->http_send_ok);
                    break;
                case TYPE_REQ_LINK_FILE:
                {
                    // 无需返回 reply
                    LINK_FILE_ARG arg = {c, hm};
                    api->func_hand(NULL, &arg, NULL);
                    return ;
                    break;
                }
                default:
                    printf("无效的请求类型 %d", api->type);
                    return ;
                    break;
                }
            }
            // 转字符串，发送
            char* jsonstr = cJSON_PrintUnformatted(root);
            //LOG_D("repy http %s\n", jsonstr);
            // 回复
            mg_http_reply(c, 200, "Content-Type:application/json;charset=utf-8\r\n", jsonstr);
            free(jsonstr);
            jsonstr = NULL;
            //
            cJSON_Delete(root);
            root = NULL;
        }
        else
        {
            struct mg_http_serve_opts opts = {.root_dir = userConf->rootDir};
            mg_http_serve_dir(c, hm, &opts);
        }
    }
#if 0
    else if (ev == MG_EV_READ)  // 读取数据了
    {
        LOG_I("本次读取数据长度 %d", ((struct mg_str*)ev_data)->len);
    }
#endif
    else if (ev == MG_EV_WRITE) // 发送完成
    {
        // 写完成
        if (wApp->http_send_ok)
        {
            printf("成功回复不为空，执行");
            wApp->http_send_ok();
            wApp->http_send_ok = NULL;
        }
    }
    else if (ev == MG_EV_WS_MSG)
    {
        // ws请求
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        printf("recv ws data %s \n", wm->data.ptr);
        //mg_ws_send(mIntent->mWsConnet, wm->data.ptr, wm->data.len, WEBSOCKET_OP_TEXT);
        mg_iobuf_delete(&c->recv, c->recv.len);
    }
    else if (ev==MG_EV_WS_CTL)
    {
        printf("ws MG_EV_WS_CTL");
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        switch (wm->flags&WEBSOCKET_FLAGS_MASK_OP) {
        case WEBSOCKET_OP_CLOSE:
            printf("ws 关闭");
            wApp->mWsConnet = NULL;
            break;
        default:
            break;
        }
    }
}

// 轮询线程
static void* webAppRun(void* arg)
{
    WEB_APP* wApp = (WEB_APP*)(arg);
    prctl(PR_SET_NAME, "webAppRun");
    while (1)mg_mgr_poll(&wApp->mMgr, 1000);
    return NULL;
}

int webAppStart(WEB_USER_CONF *conf)
{
    int ret = 0;
    if (NULL==conf)
        return -WEBAPP_ERR_CHECKPARAM;
    if (NULL==conf->api_path || NULL==conf->rootDir || NULL==conf->listenAddr)
        return -WEBAPP_ERR_CHECKPARAM;
    if (NULL!=conf->webApp)
        return -WEBAPP_ERR_CHECKPARAM;
    // 分配web实例
    WEB_APP* wApp = calloc(1, sizeof(WEB_APP));
    if (NULL==wApp)
        return -WEBAPP_ERR_MALLOC;
    // 启动服务
    mg_log_set("2");
    mg_mgr_init(&wApp->mMgr);
    if ((wApp->mConnet=mg_http_listen(&wApp->mMgr, conf->listenAddr, webEventCb, wApp))==NULL) {
        ret = -WEBAPP_ERR_SET_LISTEN;
        goto mg_free;
    }
    // 启动线程，轮询事件
    if (0!=pthread_create(&wApp->pid, NULL, webAppRun, wApp)) {
        ret = -WEBAPP_ERR_THREAD_CREATE;
        goto mg_free;
    }
    conf->webApp = wApp;
    wApp->userData = conf;
    return 0;

mg_free:
    mg_mgr_free(&wApp->mMgr);
    free(wApp);
    return ret;
}

int webAppStop(WEB_USER_CONF* conf)
{
    if (NULL==conf)
        return -WEBAPP_ERR_CHECKPARAM;
    if (NULL==conf->webApp)
        return -WEBAPP_ERR_CHECKPARAM;
    WEB_APP* wApp = (WEB_APP*)(conf->webApp);
    // 停止线程，等待线程退出
    pthread_cancel(wApp->pid);
    pthread_join(wApp->pid, NULL);
    // 销毁
    mg_mgr_free(&wApp->mMgr);
    free(wApp);conf->webApp=NULL;
    return 0;
}
