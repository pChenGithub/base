#ifndef __MQTTPAHOCCLIENT_H__
#define __MQTTPAHOCCLIENT_H__
#ifdef __cplusplus
extern "C" {
#endif
#include "mqttClient.h"
#include "MQTTClient.h"
#include <pthread.h>
#include <semaphore.h>

#define MQSYNC      // 使用同步模式
#ifndef MQSYNC
#define MQASYNC     // 使用异步模式
#endif

#define QOS         1
#define LOCKTIMEOUT_10MS    50   // 拿锁超时时间(10ms)
#define SENDMSG_TIMEOUT_MS  10000L // 等待发送消息完成时间(ms)

/*
 * 墓碑池上限: 逻辑销毁(destroyMqttclient)后保留待复用的客户端对象个数。
 * 上界 = 用过的 (addr, clientId, username) 组合数(本项目 <= 4), 不随断线次数增长。
 */
#define MAX_TOMBSTONE   8

/* 客户端内部状态(仅供实现内部使用, 不影响对外接口) */
typedef enum {
    MQSTATE_INIT = 0,       // 已创建, 未连接
    MQSTATE_CONNECTING,     // 连接中
    MQSTATE_CONNECTED,      // 已连接
    MQSTATE_TOMBSTONE,      // 已逻辑销毁(已断开), 等待 create 侧复用
} MQSTATE_INNER;

#define MQ_USERNAME "BA2FD68F4C61"
#define MQ_PASSWORD "7f8b2239e58c865d8c74d246cc46fa48"

typedef struct {
    MQClient base;
    MQTTClient client;
    MQTTClient_connectOptions conn_opts;
    MQTTClient_message pubmsg;
    MQTTClient_deliveryToken token;
    pthread_mutex_t lockmsg;
    // 改造同步方式使用的信号量
    sem_t waitRspTopic;
    // 同步通信监听的topic
    const char* waitTopic;
    char* waitData;
    int waitDataLen;
    int waitRetCode;
    /* 以下为"逻辑销毁+墓碑复用"机制的内部字段 */
    int state;                                      /* 内部状态, 取值 MQSTATE_INNER */
    char keyAddr[128];                              /* 复用匹配键: 服务地址副本 */
    char keyClientId[64];                           /* 复用匹配键: clientId 副本 */
    char keyUser[32];                               /* 复用匹配键: 用户名副本 */
    pthread_mutex_t lockstate;                      /* 保护 state 与 wait* 字段 */
} MQClientPahoC;

#ifdef __cplusplus
}
#endif
#endif
