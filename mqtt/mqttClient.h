/**
 * @file mqttClient.h
 * @author your name (you@domain.com)
 * @brief 
 * @version 0.1
 * @date 2022-10-09
 * 
 * @copyright Copyright (c) 2022
 * 自定义mqtt的操作接口
 */
#ifndef __MQTTCLIENT_H__
#define __MQTTCLIENT_H__
#ifdef __cplusplus
extern "C" {
#endif
// 错误码定义
#define MQERR_CHECK_PARAM 1   // 参数检查失败
#define MQERR_MALLOC 2        // 分配内存失败
#define MQERR_CLIENT_CREATE 3 // 创建客户端失败
#define MQERR_CLIENT_CONNECT 4 // 客户端连接失败
#define MQERR_MQTYPE    5 // mqtt库使用类型不对
#define MQERR_CLIENT_DISCONNECT    6 // 客户端断开连接失败
#define MQERR_CLIENT_DISTROY    7 // 销毁客户端失败
#define MQERR_SENDMSG       8 // 发送mqtt消息失败
#define MQERR_WAITFORSEND   9 // 等待发送mqtt消息完成失败
#define MQERR_LOCKFAIL      10 // 加锁失败
#define MQERR_SETCALLBACK   11 // 设置回调函数失败
#define MQERR_SUBSCRIBE_FAIL    12 // 订阅主题失败
#define MQERR_UNSUBSCRIBE_FAIL  13 // 取消订阅主题失败
#define MQERR_SYNCMODE_RECVTIMEOUT  14 // 同步消息模式,等待recv消息超时
#define MQERR_BUFF_NOTENOUGH        15 // 设置缓存大小不足


typedef enum {
    MQTYPE_PAHOC = 0,
} MQTYPE;

typedef int (*handleMessage)(const char* topic, const char* payload,
                             const int payloadlen, void* arg);
typedef int (*handleLost)(const char* cause, void* arg);
typedef int (*handleComplate)(void* arg);

typedef struct {
    MQTYPE type;
    handleMessage msgarrive;
    handleLost connectlost;
    handleComplate sendcomplate;
    void* context;
} MQClient;

// 创建一个mqtt客户端
int createMqttclient(MQClient **client, const char *addr, const char *clientid, const char *username, const char *passw);
// 销毁mqtt客户端
int destroyMqttclient(MQClient* client);
// 设置回调函数
int setCallBack(MQClient* client, void* context, handleMessage msgarrive,
                handleLost connectlost, handleComplate sendcomplate);
// 提交一条消息
typedef enum {
    RETAINED_OFF = 0,
    RETAINED_ON,
} RETAINED_TYPE;
int pushOneMessage(MQClient *client, const char *topic, const char *message, RETAINED_TYPE type);
// 提交一条消息等待回应
int pushOneMessageWait(MQClient *client, const char *topic, const char *message, int messageLen, const char* waitTopic, int timeoutWaitMs);
// 订阅主题
int subscribeTopic(MQClient* client, const char* topic, int qos);
// 取消订阅
int unSubscribeTopic(MQClient* client, const char* topic);
#ifdef __cplusplus
}
#endif

#endif
