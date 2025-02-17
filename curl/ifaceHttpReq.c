#include "ifaceHttpReq.h"
#include "curl/curl.h"
#include "global.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>

#define HTTP_DEBUG 1

// 写文件
static int http_reply_file(const char* buffer, int len, void* arg)
{
    HTTP_REPLY* reply = (HTTP_REPLY*)arg;
    FILE* fp = reply->p;
//    int maxLen = reply->len;

//    if(maxLen <= (reply->offset + len))
//    {
//        // 缓存不足，报错
//        reply->ret = -HTTPERROR_BUFF_NOTENOUGH;
//        return -HTTPERROR_BUFF_NOTENOUGH;
//    }

    // 请求返回写入文件
    //LOG_D("http_reply_file len %d ", len);
    fwrite(buffer, len, 1, fp);

    reply->offset += len;

    return 0;
}

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

/**
 * http请求回应
 * 做成可重入的，后面需要采用异步方式
*/
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

/**
 * http请求binary写入
 * 做成可重入的
*/
static size_t curl_read_cb(void* buffer, size_t size, size_t nitems, void* arg)
{
    size_t count = 0;
    HTTP_REPLY* reply = (HTTP_REPLY*)arg;
    FILE* putfile = (FILE*)(reply->p);
    if(NULL == putfile)
    {
        reply->ret = -HTTPERROR_CHECK_PAREM;
        return -HTTPERROR_CHECK_PAREM;
    }

    LOG_I("读取文件");
    count = fread(buffer, size, nitems, putfile);
    return count;
}

/**
 * 一个http请求,可重入的
 * type http请求返回类型
 * url  请求地址
 * indata 请求的json字符串（如果请求json），指定文件名（如果获取的文件）
 * outdata 返回的json字符串（如果返回json）,没有返回指定NULL
 * outdatalen  outdata长度,如果没有返回,指定任意值
 *
*/
int httpReq(HTTP_REPLY_TYPE type, const char* url, const char* indata, char* outdata, const int outdatalen)
{
    return httpReqTimeout(type, url, indata, outdata, outdatalen, 5000L);
}
// 获取http文件数据
int httpGetFileReq(HTTP_REPLY_TYPE type, const char* url, const char* indata, char* outdata, const int outdatalen)
{
    return httpReqTimeout(type, url, indata, outdata, outdatalen, 10000L);
}


// 这里上传日志肯定会有json字符串返回，认为返回的类型是json
int httpUploadFile(const char* url, char* data, int len, char* admin, char* pass)
{
    if(NULL == url || NULL == data || len <= 0)
    {
        return -HTTPERROR_CHECK_PAREM;
    }

    curl_mime* mime = NULL;
    curl_mimepart* part = NULL;
    int ret = 0;
    HTTP_REPLY reply;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    CURLcode res;

    memset(&reply, 0, sizeof(reply));
    reply.type = RET_JSON;
    reply.opt = http_reply_json;
    // 指定http返回内容填入到data
    reply.p = data;
    reply.len = len;

    // 准备发送http请求
    curl = curl_easy_init();
    if(NULL == curl)
    {
        ret = -HTTPERROR_CURL_INIT;
        goto curlInitError;
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "http");

    //headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    mime = curl_mime_init(curl);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "username");
    curl_mime_data(part, admin, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "password");
    curl_mime_data(part, pass, CURL_ZERO_TERMINATED);

    LOG_I("上传文件： %s", data);
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, data);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "path");
    curl_mime_data(part, "log", CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "output");
    curl_mime_data(part, "json", CURL_ZERO_TERMINATED);

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);

    // 执行请求,等待回调结束
    LOG_I("req s");
    res = curl_easy_perform(curl);
    LOG_I("req e");
    if(res != CURLE_OK)
    {
        LOG_E("perform error %d", res);
        ret = -HTTPERROR_CURL_PERFORM;
    }

curlInitError:
    if(NULL != mime)
    {
        curl_mime_free(mime);
    }

    if(NULL != headers)
    {
        curl_slist_free_all(headers);
    }\

    if(NULL != curl)
    {
        curl_easy_cleanup(curl);
    }

    return ret;
}

int httpGetReq(HTTP_REPLY_TYPE type, const char* url, const char* indata, char* outdata, const int outdatalen)
{
    if(NULL == url || NULL == indata)
    {
        return -HTTPERROR_CHECK_PAREM;
    }

    int ret = 0;
    int lenght = strlen(indata);
    HTTP_REPLY reply;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    CURLcode res;
    char head_pro[64] = {0};

    memset(&reply, 0, sizeof(reply));
    reply.type = type;
    switch(type)
    {
        case RET_FILE:
            reply.opt = http_reply_file;
            // 获取文件句柄
            reply.p = fopen(indata, "w");
            if(NULL == reply.p)
            {
                return -HTTPERROR_OPENFILE;
            }
            lenght = 0;
            break;
        case RET_JSON:
            reply.opt = http_reply_json;
            // 有返回内容，把缓存指向data，并且指定长度,
            // 这两个参数的检查会在接收数据的时候判断
            reply.p = outdata;
            reply.len = outdatalen;
            break;
        default:
            return -HTTPERROR_CHECK_PAREM;
            break;
    }

    snprintf(head_pro, 64, "Content-Length: %d", lenght);
    curl = curl_easy_init();
    if(NULL == curl)
    {
        ret = -HTTPERROR_CURL_INIT;
        goto curlInitError;
    }

    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
    //curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5);
    //curl_easy_setopt(curl, CURLOPT_HEADER, 1);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYSTATUS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    //curl_easy_setopt(curl, CURLOPT_CAINFO, "cacert.pem");//cacert.pem为curl官网下载的根证书文件
    //curl_easy_setopt(curl, CURLOPT_SSLVERSION , 3);
    //curl_easy_setopt(curl,CURLOPT_SSLVERSION,1);

    //headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, head_pro);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // body有长度，填入body
    if(RET_JSON == type && 0 != lenght)
    {
        //LOG_I("httpreq %s", indata);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, indata);
        //curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(indata));
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);

    // 执行请求,等待回调结束
    LOG_I("req s");
    res = curl_easy_perform(curl);
    LOG_I("req e");
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

curlInitError:
    // 如果是文件，需要关闭
    if(RET_FILE == reply.type && NULL != reply.p)
    {
        fclose(reply.p);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

int httpPutReq(HTTP_REPLY_TYPE type, const char* url, const char* indata, char* outdata, const int outdatalen)
{
    if(NULL == url || NULL == indata)
    {
        return -HTTPERROR_CHECK_PAREM;
    }

    int ret = 0;
    int lenght = strlen(indata);
    HTTP_REPLY reply;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    CURLcode res;
    char head_pro[64] = {0};

    memset(&reply, 0, sizeof(reply));
    reply.type = type;
    switch(type)
    {
        case RET_FILE:
            reply.opt = http_reply_file;
            // 获取文件句柄
            reply.p = fopen(indata, "w");
            if(NULL == reply.p)
            {
                return -HTTPERROR_OPENFILE;
            }
            lenght = 0;
            break;

        case RET_JSON:
            reply.opt = http_reply_json;
            // 有返回内容，把缓存指向data，并且指定长度,
            // 这两个参数的检查会在接收数据的时候判断
            reply.p = outdata;
            reply.len = outdatalen;
            break;

        default:
            return -HTTPERROR_CHECK_PAREM;
            break;
    }

    snprintf(head_pro, 64, "Content-Length: %d", lenght);
    curl = curl_easy_init();
    if(NULL == curl)
    {
        ret = -HTTPERROR_CURL_INIT;
        goto curlInitError;
    }

    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    //curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5);
    //curl_easy_setopt(curl, CURLOPT_HEADER, 1);
    //curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYSTATUS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    //curl_easy_setopt(curl, CURLOPT_CAINFO, "cacert.pem");//cacert.pem为curl官网下载的根证书文件
    //curl_easy_setopt(curl, CURLOPT_SSLVERSION , 3);
    //curl_easy_setopt(curl,CURLOPT_SSLVERSION,1);

    //headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    headers = curl_slist_append(headers, head_pro);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // body有长度，填入body
    if(RET_JSON == type && 0 != lenght)
    {
        //LOG_I("httpreq %s", indata);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, indata);
        //curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(indata));
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);

    // 执行请求,等待回调结束
    LOG_I("req s");
    res = curl_easy_perform(curl);
    LOG_I("req e");
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

curlInitError:
    // 如果是文件，需要关闭
    if(RET_FILE == reply.type && NULL != reply.p)
    {
        fclose(reply.p);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

int httpPutUploadBinaryFile(const char* url, char* data)
{
    if(NULL == url || NULL == data)
    {
        return -HTTPERROR_CHECK_PAREM;
    }

    int ret = 0;
    HTTP_REPLY reply;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    CURLcode res;
    struct stat file_info;

    // 打开文件
    FILE* fp = fopen(data, "rb");
    if(NULL == fp)
    {
        return -HTTPERROR_OPENFILE;
    }
    stat(data, &file_info);

    memset(&reply, 0, sizeof(reply));
    reply.type = RET_NONE;
    reply.opt = NULL;
    // 指定http返回内容填入到data
    reply.p = fp;
    reply.len = 0;

    // 准备发送http请求
    curl = curl_easy_init();
    if(NULL == curl)
    {
        ret = -HTTPERROR_CURL_INIT;
        goto curlInitError;
    }

    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    LOG_I("请求地址 %s", url);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    if(NULL != headers)
    {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    //curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "<file contents here>");
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, curl_read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA, &reply);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_info.st_size);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYSTATUS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    // 执行请求,等待回调结束
    LOG_I("req s");
    res = curl_easy_perform(curl);
    LOG_I("req e");
    if(res != CURLE_OK)
    {
        LOG_E("perform error %d", res);
        ret = -HTTPERROR_CURL_PERFORM;
    }
    else
    {
        ret = reply.ret;
    }

curlInitError:
    if(NULL != headers)
    {
        curl_slist_free_all(headers);
    }

    if(NULL != curl)
    {
        curl_easy_cleanup(curl);
    }

    if(NULL != fp)
    {
        fclose(fp);
    }
    return ret;
}

int httpReqTimeout(HTTP_REPLY_TYPE type, const char* url, const char* indata,
                   char* outdata, const int outdatalen, int timeoutMs)
{
    if(NULL == url || NULL == indata || timeoutMs <= 0)
    {
        return -HTTPERROR_CHECK_PAREM;
    }

    int ret = 0;
    int lenght = strlen(indata);
    HTTP_REPLY reply;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    CURLcode res;
    char head_pro[64] = {0};

    memset(&reply, 0, sizeof(reply));
    reply.type = type;
    switch(type)
    {
        case RET_FILE:
            reply.opt = http_reply_file;
            // 获取文件句柄
            reply.p = fopen(indata, "w");
            if(NULL == reply.p)
            {
                return -HTTPERROR_OPENFILE;
            }
            lenght = 0;
            break;

        case RET_JSON:
            reply.opt = http_reply_json;
            // 有返回内容，把缓存指向data，并且指定长度,
            // 这两个参数的检查会在接收数据的时候判断
            reply.p = outdata;
            reply.len = outdatalen;
            break;

        default:
            return -HTTPERROR_CHECK_PAREM;
            break;
    }

    snprintf(head_pro, 64, "Content-Length: %d", lenght);
    curl = curl_easy_init();
    if(NULL == curl)
    {
        ret = -HTTPERROR_CURL_INIT;
        goto curlInitError;
    }

    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    //curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    //curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");
    //curl_easy_setopt(curl, CURLOPT_HEADER, 1);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYSTATUS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    //curl_easy_setopt(curl, CURLOPT_CAINFO, "cacert.pem");//cacert.pem为curl官网下载的根证书文件
    //curl_easy_setopt(curl, CURLOPT_SSLVERSION , 3);
    //curl_easy_setopt(curl,CURLOPT_SSLVERSION,1);

    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, head_pro);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // body有长度，填入body
    if(RET_JSON == type && 0 != lenght)
    {
        #if HTTP_DEBUG
        LOG_I("发送数据 【%s】\n url 【%s】", indata, url);
        #endif
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, indata);
        //curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(indata));
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);

    // 执行请求,等待回调结束
    LOG_D("=========req send=========");
    res = curl_easy_perform(curl);
    LOG_D("=========req recv=========");
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

    LOG_D("len:%d offset:%d ret:%d", reply.len,reply.offset,reply.ret);

    #if 0
    if((reply.offset < 1024*4)&&(reply.offset > 0))
    {
        LOG_I("接收数据 【%s】", outdata);
    }
    #endif

curlInitError:
    // 如果是文件，需要关闭
    if(RET_FILE == reply.type && NULL != reply.p)
    {
        fclose(reply.p);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}
