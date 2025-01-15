#ifndef __MQTTPAHOCCLIENT_H__
#define __MQTTPAHOCCLIENT_H__
#ifdef __cplusplus
extern "C" {
#endif
#include "mqttClient.h"
#include "paho.mqtt.c-1.3.9/src/MQTTClient.h"
#include <pthread.h>
#include <semaphore.h>

#define MQSYNC      // 使用同步模式
#ifndef MQSYNC
#define MQASYNC     // 使用异步模式
#endif

#define QOS         1
#define LOCKTIMEOUT_10MS    50   // 拿锁超时时间(10ms)
#define SENDMSG_TIMEOUT_MS  10000L // 等待发送消息完成时间(ms)

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
} MQClientPahoC;

#ifdef __cplusplus
}
#endif
#endif
