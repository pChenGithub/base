#include "mqttPahocClient.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/**
 * @brief 
 * 
 * @param context 
 * @param topicName 
 * @param topicLen 
 * @param message 
 * @return int 
 * 处理订阅的消息
 */
static int msgarrvd(void *context, char *topicName, int topicLen, MQTTClient_message *message)
{
    printf("recv mqtt msg\n");
#if 0
    printf("Message arrived\n");
    printf("     topic: %s\n", topicName);
    printf("   message: %.*s\n", message->payloadlen, (char*)message->payload);
#endif
    // 这里调用回调处理消息
    MQClientPahoC *pclient = (MQClientPahoC *)context;
    // 如果设置了sync topic,判断是否是订阅主题
    if (NULL==pclient->waitTopic || 0!=memcmp(pclient->waitTopic, topicName, strlen(topicName)+1))
        goto end_syncpush;
    if (NULL==pclient->waitData || pclient->waitDataLen<=0)
    {
        pclient->waitRetCode = -MQERR_CHECK_PARAM;
        goto ret_syncpush;
    }
    if (message->payloadlen>=pclient->waitDataLen)
    {
        pclient->waitRetCode = -MQERR_BUFF_NOTENOUGH;
        goto ret_syncpush;
    }
    //printf("mqtt消息长度 %d\n", message->payloadlen);
    memcpy(pclient->waitData, message->payload, message->payloadlen);
    pclient->waitData[message->payloadlen] = 0;
    pclient->waitRetCode = 0;
ret_syncpush:
    //printf("post sem\n");
    sem_post(&(pclient->waitRspTopic));
    goto end_push;
end_syncpush:
    if (NULL!=pclient->base.msgarrive)
        pclient->base.msgarrive(topicName, message->payload, message->payloadlen);
end_push:
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

/**
 * @brief 
 * 
 * @param context 
 * @param cause 
 * 处理连接断开的消息
 */
static void connlost(void *context, char *cause)
{
    printf("conect lost\n");
#if 0
    printf("\nConnection lost\n");
    printf("     cause: %s\n", cause);
#endif
    // 这里开始处理连接丢失消息
    MQClientPahoC *pclient = (MQClientPahoC *)context;
    if (NULL!=pclient->base.connectlost)
        pclient->base.connectlost(cause);
}

/**
 * @brief 
 * 
 * @param context 
 * @param dt 
 */
static void delivered(void *context, MQTTClient_deliveryToken dt)
{
    (void)(context);
    //printf("Message with token value %d delivery confirmed\n", dt);
}

/**
 * @brief
 * 创建一个mqtt客户端,完成建立连接,设置回调
 * client:  返回创建的 MQClient
 */
int createMqttclient(MQClient **client, const char *addr, const char *clientid, const char* username, const char* passw) {
    int ret = 0;
    if (NULL == client || NULL==addr || NULL==clientid)
        return -MQERR_CHECK_PARAM;
    MQClientPahoC *pclient = (MQClientPahoC *)malloc(sizeof(MQClientPahoC));
    if (NULL == pclient)
        return -MQERR_MALLOC;

    pclient->base.type = MQTYPE_PAHOC;
    printf("客户端id %s\n", clientid);
    ret = MQTTClient_create(&(pclient->client), addr, clientid, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (MQTTCLIENT_SUCCESS !=ret)
    {
        printf("create ret errno %d\n", ret);
        ret = -MQERR_CLIENT_CREATE;
        goto create_fail;
    }

    // 同步模式下发送完成无需设置回调
    ret = MQTTClient_setCallbacks(pclient->client, pclient, connlost, msgarrvd, delivered);
    if (MQTTCLIENT_SUCCESS!=ret)
    {
        printf("setcallback ret errno %d\n", ret);
        ret = -MQERR_SETCALLBACK;
        goto setback_fail;
    }

    pclient->base.connectlost = NULL;
    pclient->base.msgarrive = NULL;
    pclient->base.sendcomplate = NULL;

    MQTTClient_message* pubmsg = &(pclient->pubmsg);
    // 初始化pubmsg
    // {{'M', 'Q', 'T', 'M'}, 1, 0, NULL, 0, 0, 0, 0, MQTTProperties_initializer};
    // MQTTProperties_initializer {0,0,0,NULL}
    pubmsg->struct_id[0] = 'M';
    pubmsg->struct_id[1] = 'Q';
    pubmsg->struct_id[2] = 'T';
    pubmsg->struct_id[3] = 'M';

    pubmsg->struct_version = 1;
    pubmsg->payloadlen = 0;
    pubmsg->payload = NULL;
    pubmsg->qos = 0;
    pubmsg->retained = 0;
    pubmsg->dup = 0;
    pubmsg->msgid = 0;

    pubmsg->properties.count = 0;
    pubmsg->properties.max_count = 0;
    pubmsg->properties.length = 0;
    pubmsg->properties.array = NULL;

    /** 
     * { {'M', 'Q', 'T', 'C'}, 8, 60, 1, 1, NULL, NULL, NULL, 30, 0, NULL,\
        0, NULL, MQTTVERSION_DEFAULT, {NULL, 0, 0}, {0, NULL}, -1, 0, NULL, NULL, NULL}
    */
    // 初始化conn_opts
    MQTTClient_connectOptions *conn_opts = &(pclient->conn_opts);
    conn_opts->struct_id[0] = 'M';
    conn_opts->struct_id[1] = 'Q';
    conn_opts->struct_id[2] = 'T';
    conn_opts->struct_id[3] = 'C';

    conn_opts->struct_version = 8;
    conn_opts->keepAliveInterval = 60;
    conn_opts->cleansession = 1;
    conn_opts->reliable = 1;
    conn_opts->will = NULL;
#if 0
    conn_opts->username = "ZYZH-YT325-MJJCR-1";
    conn_opts->password = "9ff33667562cc35790b8db58227b222d";
#else
    conn_opts->username = username;
    conn_opts->password = passw;
#endif
#if 0
    printf("username: %s, pass: %s\n", username, passw);
#endif
    conn_opts->connectTimeout = 30;
    conn_opts->retryInterval = 0;

    conn_opts->ssl = NULL;
    conn_opts->serverURIcount = 0;
    conn_opts->serverURIs = NULL;
    conn_opts->MQTTVersion = MQTTVERSION_DEFAULT;

    conn_opts->returned.serverURI = NULL;
    conn_opts->returned.MQTTVersion = 0;
    conn_opts->returned.sessionPresent = 0;

    conn_opts->binarypwd.len = 0;
    conn_opts->binarypwd.data = NULL;

    conn_opts->maxInflightMessages = -1;
    conn_opts->cleanstart = 0;

    conn_opts->httpHeaders = NULL;
    conn_opts->httpProxy = NULL;
    conn_opts->httpsProxy = NULL;

    // 从新设置连接属性
    pclient->conn_opts.keepAliveInterval = 20;
    pclient->conn_opts.cleansession = 1;
    ret = MQTTClient_connect(pclient->client, conn_opts);
    if (MQTTCLIENT_SUCCESS !=ret)
    {
        printf("connect ret errno %d\n", ret);
        ret = -MQERR_CLIENT_CONNECT;
        goto connect_fail;
    }

    *client = (MQClient*)pclient;
    pthread_mutex_init(&(pclient->lockmsg), NULL);
    sem_init(&(pclient->waitRspTopic), 0, 0);
    return 0;
connect_fail:
setback_fail:
    MQTTClient_destroy(&(pclient->client));
create_fail:
    free(pclient);
    pclient = NULL;
    return ret;
}

int destroyMqttclient(MQClient* client) {
    int ret = 0;
    if (NULL == client)
        return -MQERR_CHECK_PARAM;
    if (MQTYPE_PAHOC!=client->type)
        return -MQERR_MQTYPE;

    MQClientPahoC *pclient = (MQClientPahoC *)client;
    ret = MQTTClient_disconnect(pclient->client, 10000);
    if (MQTTCLIENT_SUCCESS !=ret)
    {
        printf("disconnect ret errno %d\n", ret);
        return -MQERR_CLIENT_DISCONNECT;
    }

    MQTTClient_destroy(&(pclient->client));
    pthread_mutex_destroy(&(pclient->lockmsg));
    sem_destroy(&(pclient->waitRspTopic));
    pclient->waitTopic = NULL;
    free(pclient);
    return 0;
}

/**
 * @brief 
 * 
 * @param topic 
 * @param message 
 * @return int 
 * 发送一条mqtt消息
 */
int pushOneMessage(MQClient* client, const char *topic, const char *message, RETAINED_TYPE type) {
    int ret = 0;
    if (NULL == client || NULL == topic || NULL == message)
        return -MQERR_CHECK_PARAM;
    if (MQTYPE_PAHOC!=client->type)
        return -MQERR_MQTYPE;

    MQClientPahoC *pclient = (MQClientPahoC *)client;
    MQTTClient_message *pubmsg = &(pclient->pubmsg);
    // 加锁, 设置超时,
    int timeout = LOCKTIMEOUT_10MS;
    while (timeout && 0!=pthread_mutex_trylock(&(pclient->lockmsg))) {
        usleep(100000);
        timeout--;
    }
    // 判断是否超时
    if (timeout<=0)
        return -MQERR_LOCKFAIL;

    pubmsg->payload = (void*)message;
    pubmsg->payloadlen = strlen(message);
    pubmsg->qos = QOS;
    pubmsg->retained = type;
    printf("topic %s\n", topic);
    ret = MQTTClient_publishMessage(pclient->client, topic, pubmsg, &(pclient->token));
    if (MQTTCLIENT_SUCCESS != ret)
    {
        printf("pushmsg ret errno %d\n", ret);
        ret = -MQERR_SENDMSG;
        goto end_exit;
    }

    ret = MQTTClient_waitForCompletion(pclient->client, pclient->token, SENDMSG_TIMEOUT_MS);
    if (MQTTCLIENT_SUCCESS != ret)
    {
        printf("waitforcompletion ret errno %d, token %d\n", ret, pclient->token);
        ret = -MQERR_WAITFORSEND;
        goto end_exit;
    }

    ret = 0;
end_exit:
    // 解锁
    pthread_mutex_unlock(&(pclient->lockmsg));
    return ret;
}

// 发送一个消息,并等待指定topic回复,超时错误退出
int pushOneMessageWait(MQClient *client, const char *topic, const char *message, int messageLen, const char* waitTopic, int timeoutWaitMs) {
    int ret = 0;
    if (NULL == client || NULL == topic || NULL == message)
        return -MQERR_CHECK_PARAM;
    if (MQTYPE_PAHOC!=client->type)
        return -MQERR_MQTYPE;

    MQClientPahoC *pclient = (MQClientPahoC *)client;
    MQTTClient_message *pubmsg = &(pclient->pubmsg);
    // 加锁, 设置超时,
    int timeout = LOCKTIMEOUT_10MS;
    while (timeout && 0!=pthread_mutex_trylock(&(pclient->lockmsg))) {
        usleep(10000);
        timeout--;
    }
    // 判断是否超时
    if (timeout<=0)
        return -MQERR_LOCKFAIL;

    // 因为需要等待回复,这里订阅主题
    //printf("subtopic %s\n", waitTopic);
    ret = MQTTClient_subscribe(pclient->client, waitTopic, QOS);
    if (ret<0)
    {
        ret = -MQERR_SUBSCRIBE_FAIL;
        goto end_exit;
    }
    // 赋值wait的主体,以便在回调函数中判断
    pclient->waitTopic = waitTopic;
    pclient->waitData = (char*)message;
    pclient->waitDataLen = messageLen;

    pubmsg->payload = (void*)message;
    pubmsg->payloadlen = strlen(message);
    pubmsg->qos = QOS;
    pubmsg->retained = 0;
#if 0
    printf("\n\npush topic %s\n%s\n\n", topic, message);
#endif
    ret = MQTTClient_publishMessage(pclient->client, topic, pubmsg, &(pclient->token));
    if (MQTTCLIENT_SUCCESS != ret)
    {
        printf("pushmsg ret errno %d\n", ret);
        ret = -MQERR_SENDMSG;
        goto end_unsub;
    }
    printf("publish message OK\n");

    ret = MQTTClient_waitForCompletion(pclient->client, pclient->token, SENDMSG_TIMEOUT_MS);
    if (MQTTCLIENT_SUCCESS != ret)
    {
        printf("waitforcompletion ret errno %d, token %d\n", ret, pclient->token);
        ret = -MQERR_WAITFORSEND;
        goto end_unsub;
    }

    // 等待回复topic消息,并判断消息类型
    // 装载超时时间
    struct timespec waittime;
    struct  timeval tv;
    gettimeofday(&tv, NULL);
    //clock_gettime(CLOCK_REALTIME, &waittime);
    waittime.tv_sec = tv.tv_sec+(timeoutWaitMs/1000);
    waittime.tv_nsec = tv.tv_usec*1000 + (timeoutWaitMs%1000)*1000;
    //printf("wait recv msg %ld\n", time(NULL));
    // 等待
    if (0!=sem_timedwait(&(pclient->waitRspTopic), &waittime))
    {
        // 超时
        //printf("waitrecv timeout %ld %d\n", time(NULL), errno);
        printf("waitrecv timeout\n");
        ret = -MQERR_SYNCMODE_RECVTIMEOUT;
        goto end_unsub;
    }

    printf("wait message OK\n");

    if (0!=pclient->waitRetCode)
    {
        ret = pclient->waitRetCode;
        goto end_unsub;
    }
#if 0
    printf("sub topic %s\n      %s\n", waitTopic, message);
#endif
    ret = 0;
end_unsub:
    // 等待主体重新赋值为0
    pclient->waitTopic = NULL;
    pclient->waitData = NULL;
    pclient->waitDataLen = 0;
    MQTTClient_unsubscribe(pclient->client, waitTopic);
end_exit:
    printf("unlock exit\n");
    // 解锁
    pthread_mutex_unlock(&(pclient->lockmsg));
    printf("push exit\n");
    return ret;
}

int setCallBack(MQClient* client, handleMessage msgarrive, handleLost connectlost, handleComplate sendcomplate) {
    (void)sendcomplate;
    if (NULL==client || NULL==msgarrive || NULL==connectlost)
        return -MQERR_CHECK_PARAM;
    if (MQTYPE_PAHOC!=client->type)
        return -MQERR_MQTYPE;

    client->msgarrive = msgarrive;
    client->connectlost = connectlost;
#ifdef MQSYNC
    client->sendcomplate = NULL;
#else
    client->sendcomplate = sendcomplate;
#endif
    return 0;
}

int subscribeTopic(MQClient* client, const char* topic, int qos) {
    int ret = 0;
    if (NULL == client || NULL == topic)
        return -MQERR_CHECK_PARAM;
    if (MQTYPE_PAHOC!=client->type)
        return -MQERR_MQTYPE;

    MQClientPahoC *pclient = (MQClientPahoC *)client;
    ret = MQTTClient_subscribe(pclient->client, topic, qos);
    if (MQTTCLIENT_SUCCESS!=ret)
        return -MQERR_SUBSCRIBE_FAIL;
    return 0;
}

int unSubscribeTopic(MQClient* client, const char* topic) {
    int ret = 0;
    if (NULL == client || NULL == topic)
        return -MQERR_CHECK_PARAM;
    if (MQTYPE_PAHOC!=client->type)
        return -MQERR_MQTYPE;
    MQClientPahoC *pclient = (MQClientPahoC *)client;
    ret = MQTTClient_unsubscribe(pclient->client, topic);
    if (MQTTCLIENT_SUCCESS!=ret)
        return -MQERR_UNSUBSCRIBE_FAIL;
    return 0;
}


