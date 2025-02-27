#include "zyEcpHttpReq.h"
#include "curl/curl.h"
#include "global.h"
#include "cJSON.h"
#include "cJSON_tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

// 写内存
static int http_reply_json(const char* buffer, int len, void* arg)
{
    HTTP_REPLY* reply = (HTTP_REPLY*)arg;
    char* p = reply->p;
    int maxLen = reply->len;

    if(NULL == p)
    {
        // 传入带回参数为空
        reply->ret = -HTTPERROR_CHECK_PAREM;
        return -HTTPERROR_CHECK_PAREM;
    }

    if(maxLen <= (reply->offset + len))
    {
        // 缓存不足，报错
        reply->ret = -HTTPERROR_BUFF_NOTENOUGH;
        return -HTTPERROR_BUFF_NOTENOUGH;
    }

    // 偏移
    p += reply->offset;

    // 填入返回内容
    memcpy(p, buffer, len);
    reply->offset += len;
    p[len] = 0;
    return 0;
}

static size_t curl_write_cb(void* buffer, size_t size, size_t nitems, void* arg)
{
    size_t count = size * nitems;
    HTTP_REPLY* reply = (HTTP_REPLY*)arg;

    //LOG_D("http reply data len %d ", count);
    if(NULL == buffer || 0 == count || NULL == reply)
    {
        reply->ret = -HTTPERROR_CHECK_PAREM;
        return -HTTPERROR_CHECK_PAREM;
    }
    reply->opt(buffer, count, reply);
    return count;
}

int httpAddEcpDev(const char *url, const char *indata, char *outdata, const int outdatalen)
{
    int ret = 0;
    // 校验参数
    // 校验token，如果过期，重新获取
    ret = httpGetToken(LOCAL_ECP_APP_HTTP"/api/app/token", LOCAL_APP_ID, LOCAL_APP_SECRET);
    if (ret<0)
    {
        return -HTTPERROR_GET_TOKEN;
    }

    int lenght = strlen(indata);
    HTTP_REPLY reply;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    CURLcode res;
    char head_pro[64] = {0};

    memset(&reply, 0, sizeof(reply));
    reply.type = RET_JSON;
    // 本接口固定返回json
    reply.opt = http_reply_json;
    // 有返回内容，把缓存指向data，并且指定长度,
    // 这两个参数的检查会在接收数据的时候判断
    reply.p = outdata;
    reply.len = outdatalen;

    snprintf(head_pro, 64, "Content-Length:%d", lenght);
    curl = curl_easy_init();
    if(NULL == curl)
    {
        return -HTTPERROR_CURL_INIT;
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    char addr[1024] = {0};
    snprintf(addr, sizeof(addr), "%s?access_token=%s", url, g_dev_meta.workInfo.access_token);
    //LOG_I("url %s", addr);
    curl_easy_setopt(curl, CURLOPT_URL, addr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYSTATUS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    //headers = curl_slist_append(headers, "Content-Type:application/json");
    headers = curl_slist_append(headers, head_pro);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // body有长度，填入body
    if(0 != lenght)
    {
        LOG_I("httpreq %s", indata);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, indata);
        //curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(indata));
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);

    // 执行请求,等待回调结束
    res = curl_easy_perform(curl);
    if(res != CURLE_OK)
    {
        LOG_E("请求错误码 %d", res);
        if(CURLE_COULDNT_CONNECT == res)
        {
            ret = -HTTPERROR_COULDNT_CONNECT;
        }
        else if(CURLE_OPERATION_TIMEDOUT == res)
        {
            ret = -HTTPERROR_OPERATION_TIMEDOUT;
        }
        else
        {
            ret = -HTTPERROR_CURL_PERFORM;
        }
    }
    else
    {
        ret = reply.ret;
    }

//curlInitError:
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

int httpGetToken(const char *url, const char *appid, char *appsecret)
{
    if(NULL == url || NULL == appid || NULL==appsecret)
    {
        return -HTTPERROR_CHECK_PAREM;
    }
    // 判断token是否过期
    time_t curr_t = time(NULL);
    if (curr_t<g_dev_meta.workInfo.expiry_time)
    {
        // 没有过期
        LOG_I("token没有过期，不重新获取");
        return 0;
    }
    LOG_I("token过期，重新获取");

    int ret = 0;
    HTTP_REPLY reply;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    CURLcode res;

    memset(&reply, 0, sizeof(reply));
    reply.type = RET_JSON;
    // 本接口固定返回json
    reply.opt = http_reply_json;
    // 有返回内容，把缓存指向data，并且指定长度,
    // 这两个参数的检查会在接收数据的时候判断
    reply.p = calloc(1, 1024);
    if (NULL==reply.p)
    {
        return -HTTPERROR_MALLOC;
    }
    reply.len = 1024;

    curl = curl_easy_init();
    if(NULL == curl)
    {
        ret = -HTTPERROR_CURL_INIT;
        goto free_buff;
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
    // 借用reply.p
    snprintf(reply.p, 1024, "%s?appid=%s&appsecret=%s", url, appid, appsecret);
    curl_easy_setopt(curl, CURLOPT_URL, reply.p);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYSTATUS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // 没有body
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);

    // 执行请求,等待回调结束
    res = curl_easy_perform(curl);
    if(res != CURLE_OK)
    {
        LOG_E("请求错误码 %d", res);
        if(CURLE_COULDNT_CONNECT == res)
        {
            ret = -HTTPERROR_COULDNT_CONNECT;
        }
        else if(CURLE_OPERATION_TIMEDOUT == res)
        {
            ret = -HTTPERROR_OPERATION_TIMEDOUT;
        }
        else
        {
            ret = -HTTPERROR_CURL_PERFORM;
        }
    }
    else
    {
        ret = reply.ret;
    }

    //printf("<== %s\n", (char*)reply.p);
    // 解析返回json，保存token
    cJSON* root = cJSON_Parse(reply.p);
    if (NULL==root)
    {
        ret = -HTTPERROR_PARSE_JSON;
        goto curlInitError;
    }
    ret = getfield_str(root, "access_token", g_dev_meta.workInfo.access_token,
                       sizeof(g_dev_meta.workInfo.access_token));
    if (ret<0)
    {
        ret = -HTTPERROR_GET_FILED_VAL;
        goto free_json;
    }
    int expires_in = 0;
    ret = getfield_int(root, "expires_in", &expires_in);
    if (ret<0)
    {
        ret = -HTTPERROR_GET_FILED_VAL;
        goto free_json;
    }
    // 修改token到期时间
    g_dev_meta.workInfo.expiry_time = curr_t+expires_in;
    // 修改正确返回
    ret = 0;

free_json:
    cJSON_Delete(root);
    root = NULL;
curlInitError:
    //curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
free_buff:
    free(reply.p);
    reply.p = NULL;
    return ret;
}

int httpDelEcpDev(const char *url, const char *device_sn, char *app_secret, char *outdata, const int outdatalen)
{
    int ret = 0;
    // 校验参数
    // 校验token，如果过期，重新获取
    ret = httpGetToken(LOCAL_ECP_APP_HTTP"/api/app/token", LOCAL_APP_ID, LOCAL_APP_SECRET);
    if (ret<0)
    {
        return -HTTPERROR_GET_TOKEN;
    }

    HTTP_REPLY reply;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    CURLcode res;

    memset(&reply, 0, sizeof(reply));
    reply.type = RET_JSON;
    // 本接口固定返回json
    reply.opt = http_reply_json;
    // 有返回内容，把缓存指向data，并且指定长度,
    // 这两个参数的检查会在接收数据的时候判断
    reply.p = outdata;
    reply.len = outdatalen;

    curl = curl_easy_init();
    if(NULL == curl)
    {
        return -HTTPERROR_CURL_INIT;
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    //
    char addr[1024] = {0};
    snprintf(addr, sizeof(addr), "%s?access_token=%s&device_sn=%s&app_secret=%s", url,
             g_dev_meta.workInfo.access_token, device_sn, app_secret);
    //LOG_I("url %s", addr);
    curl_easy_setopt(curl, CURLOPT_URL, addr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYSTATUS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    //headers = curl_slist_append(headers, "Content-Type:application/json");
    //headers = curl_slist_append(headers, head_pro);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // body无

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);

    // 执行请求,等待回调结束
    res = curl_easy_perform(curl);
    if(res != CURLE_OK)
    {
        LOG_E("请求错误码 %d", res);
        if(CURLE_COULDNT_CONNECT == res)
        {
            ret = -HTTPERROR_COULDNT_CONNECT;
        }
        else if(CURLE_OPERATION_TIMEDOUT == res)
        {
            ret = -HTTPERROR_OPERATION_TIMEDOUT;
        }
        else
        {
            ret = -HTTPERROR_CURL_PERFORM;
        }
    }
    else
    {
        ret = reply.ret;
    }

//curlInitError:
    //curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}
