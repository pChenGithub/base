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
/* 错误码定义: 宏值为正数, 接口返回时取负 */
#define MQERR_CHECK_PARAM 1   /* 参数检查失败 */
#define MQERR_MALLOC 2        /* 分配内存失败 */
#define MQERR_CLIENT_CREATE 3 /* 创建客户端失败 */
#define MQERR_CLIENT_CONNECT 4 /* 客户端连接失败 */
#define MQERR_MQTYPE    5 /* mqtt库使用类型不对 */
#define MQERR_CLIENT_DISCONNECT    6 /* 客户端断开连接失败 */
#define MQERR_CLIENT_DISTROY    7 /* 销毁客户端失败 */
#define MQERR_SENDMSG       8 /* 发送mqtt消息失败 */
#define MQERR_WAITFORSEND   9 /* 等待发送mqtt消息完成失败 */
#define MQERR_LOCKFAIL      10 /* 加锁失败 */
#define MQERR_SETCALLBACK   11 /* 设置回调函数失败 */
#define MQERR_SUBSCRIBE_FAIL    12 /* 订阅主题失败 */
#define MQERR_UNSUBSCRIBE_FAIL  13 /* 取消订阅主题失败 */
#define MQERR_SYNCMODE_RECVTIMEOUT  14 /* 同步消息模式, 等待recv消息超时 */
#define MQERR_BUFF_NOTENOUGH        15 /* 设置缓存大小不足 */


/* mqtt 库使用类型 */
typedef enum {
    MQTYPE_PAHOC = 0,       /* paho mqtt C 库 */
} MQTYPE;

/**
 * @brief 收到订阅消息的回调
 *
 * @param topic 消息主题
 * @param payload 消息内容(非以 '\0' 结尾, 长度见 payloadlen)
 * @param payloadlen 消息内容长度(字节)
 * @param arg 注册回调时传入的上下文
 * @return int 0=成功; <0=错误码(见 MQERR_*)
 */
typedef int (*handleMessage)(const char* topic, const char* payload,
                             const int payloadlen, void* arg);

/**
 * @brief 连接丢失的回调
 *
 * @param cause 断开原因描述, 允许为 NULL
 * @param arg 注册回调时传入的上下文
 * @return int 0=成功; <0=错误码(见 MQERR_*)
 */
typedef int (*handleLost)(const char* cause, void* arg);

/**
 * @brief 发送完成的回调
 *
 * @param arg 注册回调时传入的上下文
 * @return int 0=成功; <0=错误码(见 MQERR_*)
 */
typedef int (*handleComplate)(void* arg);

/* mqtt 客户端对象 */
typedef struct {
    MQTYPE type;                    /* 使用的库类型 */
    handleMessage msgarrive;        /* 收到消息回调 */
    handleLost connectlost;         /* 连接丢失回调 */
    handleComplate sendcomplate;    /* 发送完成回调 */
    void* context;                  /* 回调上下文, 透传给各回调的 arg */
} MQClient;

/**
 * @brief 创建一个 mqtt 客户端并建立连接
 *
 * 说明: 内部按 (addr, clientid, username) 三要素复用已逻辑销毁的客户端对象,
 *       未命中时才真正创建库客户端; 各客户端之间状态互相独立。
 *
 * @param client 出参, 返回创建的客户端对象
 * @param addr 服务地址
 * @param clientid 客户端 ID
 * @param username 用户名
 * @param passw 密码
 * @return int 0=成功; <0=错误码(见 MQERR_*)
 */
int createMqttclient(MQClient **client, const char *addr, const char *clientid, const char *username, const char *passw);

/**
 * @brief 销毁(逻辑销毁)mqtt 客户端
 *
 * 说明: 仅断开连接并标记失效, 不影响其它客户端; 对象保留待同三要素的 create
 *       复用, 且可重复调用(幂等)。
 *
 * @param client 待销毁的客户端对象
 * @return int 0=成功; <0=错误码(见 MQERR_*)
 */
int destroyMqttclient(MQClient* client);

/**
 * @brief 设置该客户端的回调函数与上下文
 *
 * @param client 目标客户端对象
 * @param context 回调上下文, 透传给各回调的 arg
 * @param msgarrive 收到消息回调
 * @param connectlost 连接丢失回调
 * @param sendcomplate 发送完成回调
 * @return int 0=成功; <0=错误码(见 MQERR_*)
 */
int setCallBack(MQClient* client, void* context, handleMessage msgarrive,
                handleLost connectlost, handleComplate sendcomplate);
/* 消息保留标志 */
typedef enum {
    RETAINED_OFF = 0,   /* 不保留 */
    RETAINED_ON,        /* 保留 */
} RETAINED_TYPE;

/**
 * @brief 提交一条消息(不等待响应)
 *
 * @param client 目标客户端对象
 * @param topic 发送主题
 * @param message 发送内容(以 '\0' 结尾的字符串)
 * @param type 消息保留标志
 * @return int 0=成功; <0=错误码(见 MQERR_*)
 */
int pushOneMessage(MQClient *client, const char *topic, const char *message, RETAINED_TYPE type);

/**
 * @brief 提交一条消息并同步等待指定主题的响应
 *
 * 说明: 内部先订阅 waitTopic 再发送, 收到该主题的响应或超时后取消订阅;
 *       message 既为发送内容也为响应接收缓冲区, 故需保证其在等待期间有效。
 *
 * @param client 目标客户端对象
 * @param topic 发送主题
 * @param message 发送内容(同时作为响应接收缓冲区)
 * @param messageLen 响应接收缓冲区长度(字节)
 * @param waitTopic 等待响应的主题
 * @param timeoutWaitMs 等待响应超时(毫秒)
 * @return int 0=成功; <0=错误码(见 MQERR_*)
 */
int pushOneMessageWait(MQClient *client, const char *topic, const char *message, int messageLen, const char* waitTopic, int timeoutWaitMs);

/**
 * @brief 订阅主题
 *
 * @param client 目标客户端对象
 * @param topic 订阅主题
 * @param qos 服务质量等级
 * @return int 0=成功; <0=错误码(见 MQERR_*)
 */
int subscribeTopic(MQClient* client, const char* topic, int qos);

/**
 * @brief 取消订阅主题
 *
 * @param client 目标客户端对象
 * @param topic 待取消的主题
 * @return int 0=成功; <0=错误码(见 MQERR_*)
 */
int unSubscribeTopic(MQClient* client, const char* topic);
#ifdef __cplusplus
}
#endif

#endif
