#include "mqttPahocClient.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

/* ============================================================================
 * 墓碑池(逻辑销毁后保留的客户端对象)
 *
 * 背景: paho 1.3.9 同步 API 的物理销毁(MQTTClient_destroy)会释放库内部的
 *       handles / bstate->clients 全局链表, 并可能在销毁最后一个客户端时触发
 *       库级 MQTTClient_terminate(ListFree(handles) / handles = NULL)。
 *       而 paho 后台线程(MQTTClient_run)会在不持 mqttclient_mutex 的窗口内遍历
 *       这些全局链表, 两者并发即造成段错误(现场栈: libpaho ListFindItem <-
 *       MQTTClient_run)。MQTTClient_stop() 的"停线程并等待"只在全局都没有客户端
 *       处于连接态时才生效, 因此"另一个客户端在线"时物理销毁必然不安全。
 *
 * 对策: 运行期禁止物理销毁。
 *       - destroyMqttclient(): 仅 MQTTClient_disconnect + 标记失效 + 入池;
 *       - createMqttclient(): 先按 (addr, clientId, username) 三要素全等匹配
 *         复用池中对象, 未命中才真正执行 MQTTClient_create;
 *       - 池中对象只入不销, 上界 MAX_TOMBSTONE; 进程退出不做物理回收。
 *
 * 唯一允许物理销毁的例外: 新建对象时 create 失败 / setCallbacks 失败 两个分支,
 * 此时 paho 后台线程尚未启动, 销毁安全。
 * ========================================================================== */
static MQClientPahoC* g_tombstone[MAX_TOMBSTONE] = {0};
static int g_tombstoneCount = 0;
static pthread_mutex_t g_tombstoneLock = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief 保存本客户端的复用匹配键(addr/clientId/username 三要素副本)
 *
 * @param pclient 目标客户端对象, 不可为空
 * @param addr 服务地址, 不可为空
 * @param clientid 客户端 ID, 不可为空
 * @param username 用户名, 不可为空
 * @return int 0=成功; -MQERR_CHECK_PARAM=入参非法; -MQERR_BUFF_NOTENOUGH=源串超出目标数组容量
 */
static int saveClientKey(MQClientPahoC* pclient, const char* addr,
                         const char* clientid, const char* username)
{
    int addrLen = 0;
    int clientIdLen = 0;
    int userLen = 0;

    if (NULL == pclient || NULL == addr || NULL == clientid || NULL == username)
    {
        return -MQERR_CHECK_PARAM;
    }

    /* 目标容量用 sizeof 计算; 源串(含结尾'\0')超出容量即报错, 不截断、不静默 */
    addrLen = (int)strlen(addr) + 1;
    clientIdLen = (int)strlen(clientid) + 1;
    userLen = (int)strlen(username) + 1;
    if (addrLen > (int)sizeof(pclient->keyAddr)
        || clientIdLen > (int)sizeof(pclient->keyClientId)
        || userLen > (int)sizeof(pclient->keyUser))
    {
        return -MQERR_BUFF_NOTENOUGH;
    }

    memcpy(pclient->keyAddr, addr, (size_t)addrLen);
    memcpy(pclient->keyClientId, clientid, (size_t)clientIdLen);
    memcpy(pclient->keyUser, username, (size_t)userLen);
    return 0;
}

/**
 * @brief 判断客户端的复用匹配键与给定三要素是否全等匹配
 *
 * @param p 已保存匹配键的客户端对象, 不可为空
 * @param addr 服务地址, 不可为空
 * @param clientid 客户端 ID, 不可为空
 * @param username 用户名, 不可为空
 * @return int 1=匹配; 0=不匹配(含入参非法或键未设置)
 */
static int isSameClientKey(MQClientPahoC* p, const char* addr,
                           const char* clientid, const char* username)
{
    if (NULL == p || NULL == addr || NULL == clientid || NULL == username)
    {
        return 0;
    }
    /* 空键表示未设置(键仅用于复用匹配, 未设置则不复用) */
    if ('\0' == p->keyAddr[0] || '\0' == p->keyClientId[0])
    {
        return 0;
    }
    if (0 != strcmp(p->keyAddr, addr) || 0 != strcmp(p->keyClientId, clientid))
    {
        return 0;
    }
    return (0 == strcmp(p->keyUser, username)) ? 1 : 0;
}

/**
 * @brief 从墓碑池中摘取三要素全等匹配的客户端对象
 *
 * @param addr 服务地址
 * @param clientid 客户端 ID
 * @param username 用户名, 允许为 NULL
 * @return MQClientPahoC* 命中的对象(已从池中移除); 未命中返回 NULL
 */
static MQClientPahoC* pool_take(const char* addr, const char* clientid,
                                const char* username)
{
    MQClientPahoC* hit = NULL;
    int i = 0;
    pthread_mutex_lock(&g_tombstoneLock);
    for (i = 0; i < g_tombstoneCount; i++)
    {
        if (0 == isSameClientKey(g_tombstone[i], addr, clientid, username))
            continue;
        hit = g_tombstone[i];
        /* 用尾部元素填补空洞, 保持数组紧凑 */
        g_tombstone[i] = g_tombstone[g_tombstoneCount - 1];
        g_tombstone[g_tombstoneCount - 1] = NULL;
        g_tombstoneCount--;
        break;
    }
    pthread_mutex_unlock(&g_tombstoneLock);
    return hit;
}

/**
 * @brief 将逻辑销毁的客户端对象放入墓碑池(任何情况下都不做物理销毁)
 *
 * @param pclient 待入池的对象, 为 NULL 时直接返回
 * @return void 无
 */
static void pool_put(MQClientPahoC* pclient)
{
    int i = 0;

    if (NULL == pclient)
    {
        return;
    }

    /* 持锁期间各分支统一跳转 exit_unlock 出口, 保证加解锁严格配对 */
    pthread_mutex_lock(&g_tombstoneLock);
    /* 幂等: 同一对象已在池中则忽略 */
    for (i = 0; i < g_tombstoneCount; i++)
    {
        if (g_tombstone[i] == pclient)
        {
            goto exit_unlock;
        }
    }
    if (g_tombstoneCount >= MAX_TOMBSTONE)
    {
        /* 池满: 静默保留对象(绝不物理销毁, 宁可多占内存也不引入 UAF);
         * 池上界按 "客户端种类数" 设定, 正常业务不会到达 */
        goto exit_unlock;
    }
    g_tombstone[g_tombstoneCount] = pclient;
    g_tombstoneCount++;

exit_unlock:
    pthread_mutex_unlock(&g_tombstoneLock);
    return;
}

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
    (void)topicLen;
    if (NULL==context || NULL==topicName || NULL==message)
        goto end_push;
    //printf("recv mqtt msg\n");
#if 0
    printf("Message arrived\n");
    printf("     topic: %s\n", topicName);
    printf("   message: %.*s\n", message->payloadlen, (char*)message->payload);
#endif
    // 这里调用回调处理消息
    MQClientPahoC *pclient = (MQClientPahoC *)context;
    size_t len = strlen(topicName);
    // 如果设置了sync topic,判断是否是订阅主题
    if (NULL==pclient->waitTopic)
        goto end_syncpush;
    if (strlen(pclient->waitTopic)!=len)
        goto end_syncpush;
    if (0!=memcmp(pclient->waitTopic, topicName, len+1))
        goto end_syncpush;
    //
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
        pclient->base.msgarrive(topicName, message->payload,
                                message->payloadlen, pclient->base.context);
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
        pclient->base.connectlost(cause, pclient->base.context);
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
    (void)dt;
    //printf("Message with token value %d delivery confirmed\n", dt);
}

/**
 * @brief
 * 创建一个mqtt客户端,完成建立连接,设置回调
 * client:  返回创建的 MQClient
 */
int createMqttclient(MQClient **client, const char *addr, const char *clientid,
                     const char* username, const char* passw) {
    int ret = 0;
    MQClientPahoC *pclient = NULL;

    if (NULL == client || NULL==addr || NULL==clientid
        || NULL==username || NULL==passw)
        return -MQERR_CHECK_PARAM;

    /* ① 取对象: 优先复用墓碑池中三要素全等匹配的对象, 未命中则新建对象
     * 说明: 不论复用还是新建, 连接都由下面的公共连接流程统一完成 */
    pclient = pool_take(addr, clientid, username);
    if (NULL == pclient)
    {
        /* 新建对象: 分配内存 + 初始化 + 创建库客户端 + 注册回调(不含连接) */
        pclient = (MQClientPahoC *)malloc(sizeof(MQClientPahoC));
        if (NULL == pclient)
        {
            return -MQERR_MALLOC;
        }

        pclient->base.type = MQTYPE_PAHOC;
        pclient->client = NULL;         /* 必须先置空: 失败路径据此判断句柄是否有效 */
        pclient->state = MQSTATE_INIT;
        pclient->keyAddr[0] = '\0';
        pclient->keyClientId[0] = '\0';
        pclient->keyUser[0] = '\0';
        /* 注: wait* 等待字段与 base 回调节点属"每轮易变状态", 统一由下面的公共连接流程复位, 此处不重复 */
        pthread_mutex_init(&(pclient->lockstate), NULL);
        pthread_mutex_init(&(pclient->lockmsg), NULL);
        sem_init(&(pclient->waitRspTopic), 0, 0);

        printf("客户端id %s\n", clientid);
        ret = MQTTClient_create(&(pclient->client), addr, clientid, MQTTCLIENT_PERSISTENCE_NONE, NULL);
        if (MQTTCLIENT_SUCCESS !=ret)
        {
            printf("create ret errno %d\n", ret);
            ret = -MQERR_CLIENT_CREATE;
            goto create_fail;
        }

        /* 同步模式下发送完成无需设置回调 */
        ret = MQTTClient_setCallbacks(pclient->client, pclient, connlost, msgarrvd, delivered);
        if (MQTTCLIENT_SUCCESS!=ret)
        {
            printf("setcallback ret errno %d\n", ret);
            ret = -MQERR_SETCALLBACK;
            goto setback_fail;
        }

        /* 初始化 pubmsg: {{'M', 'Q', 'T', 'M'}, 1, 0, NULL, 0, 0, 0, 0, MQTTProperties_initializer} */
        MQTTClient_message* pubmsg = &(pclient->pubmsg);
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

        /* 初始化 conn_opts: {{'M', 'Q', 'T', 'C'}, 8, 60, 1, 1, NULL, NULL, NULL, 30, 0, NULL,
           0, NULL, MQTTVERSION_DEFAULT, {NULL, 0, 0}, {0, NULL}, -1, 0, NULL, NULL, NULL} */
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
        /* 凭据 username/password 由下面的公共连接流程按本次调用设置, 此处不重复 */
        conn_opts->connectTimeout = 3;
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

        /* 重新设置连接属性: 心跳 20s, 清会话 */
        pclient->conn_opts.keepAliveInterval = 20;
        pclient->conn_opts.cleansession = 1;

        /* 保存复用匹配键; 失败与 create/setCallbacks 失败同属"库线程未启动"分支, 物理销毁安全 */
        ret = saveClientKey(pclient, addr, clientid, username);
        if (ret < 0)
        {
            goto create_fail;
        }
    }
    else
    {
        printf("复用 mqtt 客户端, clientid %s\n", clientid);
    }

    /* ② 公共连接流程: 复位本轮易变状态 -> 设置凭据 -> 连接 */
    pthread_mutex_lock(&(pclient->lockstate));
    pclient->pubmsg.payload = NULL;
    pclient->pubmsg.payloadlen = 0;
    pclient->waitTopic = NULL;
    pclient->waitData = NULL;
    pclient->waitDataLen = 0;
    pclient->waitRetCode = 0;
    pclient->base.msgarrive = NULL;
    pclient->base.connectlost = NULL;
    pclient->base.sendcomplate = NULL;
    pclient->base.context = NULL;
    pclient->state = MQSTATE_CONNECTING;
    pclient->conn_opts.username = username;
    pclient->conn_opts.password = passw;
    pthread_mutex_unlock(&(pclient->lockstate));
    /* 清掉上一轮可能残留的信号量计数(新建对象计数为 0, 无副作用) */
    while (0 == sem_trywait(&(pclient->waitRspTopic)))
        ;

    ret = MQTTClient_connect(pclient->client, &(pclient->conn_opts));
    if (MQTTCLIENT_SUCCESS != ret)
    {
        printf("connect ret errno %d\n", ret);
        ret = -MQERR_CLIENT_CONNECT;
        goto connect_fail;
    }

    pclient->state = MQSTATE_CONNECTED;
    *client = (MQClient*)pclient;
    return 0;

connect_fail:
    /* 连接失败时回调已注册, 库后台线程可能已启动, 因此禁止物理销毁;
     * 按"逻辑销毁"处理(断开 + 标记失效 + 入池), 供后续 create 复用 */
    (void)MQTTClient_disconnect(pclient->client, 0);
    pclient->state = MQSTATE_TOMBSTONE;
    pool_put(pclient);
    return ret;

setback_fail:
create_fail:
    /* 此二分支库后台线程尚未启动(create 未成功 或 未进入连接), 物理销毁安全,
     * 是运行期唯一允许物理销毁的例外 */
    if (NULL != pclient->client)
        MQTTClient_destroy(&(pclient->client));
    pthread_mutex_destroy(&(pclient->lockstate));
    pthread_mutex_destroy(&(pclient->lockmsg));
    sem_destroy(&(pclient->waitRspTopic));
    free(pclient);
    pclient = NULL;
    return ret;
}

int destroyMqttclient(MQClient* client) {
    int ret = 0;
    int needDisconnect = 0;
    MQClientPahoC *pclient = NULL;

    if (NULL == client)
        return -MQERR_CHECK_PARAM;
    if (MQTYPE_PAHOC!=client->type)
        return -MQERR_MQTYPE;

    pclient = (MQClientPahoC *)client;

    /* 幂等检查与状态迁移在同一临界区内完成, 临界区内不提前返回 */
    pthread_mutex_lock(&(pclient->lockstate));
    if (MQSTATE_TOMBSTONE != pclient->state)
    {
        pclient->state = MQSTATE_TOMBSTONE;
        pclient->waitTopic = NULL;  /* 避免回调继续引用已失效的等待主题 */
        needDisconnect = 1;
    }
    pthread_mutex_unlock(&(pclient->lockstate));

    if (0 == needDisconnect)
    {
        /* 幂等: 已逻辑销毁过, 直接返回 */
        return 0;
    }

    /* 只断开, 不做任何物理销毁(见文件头墓碑池说明) */
    ret = MQTTClient_disconnect(pclient->client, 10000);
    if (MQTTCLIENT_SUCCESS !=ret)
    {
        printf("disconnect ret errno %d\n", ret);
        ret = -MQERR_CLIENT_DISCONNECT;
        /* 注意: 断开失败也继续走逻辑销毁, 绝不物理销毁对象 */
    }

    /* 逻辑销毁: 对象保留在墓碑池中, 等待 create 侧按三要素复用 */
    pool_put(pclient);
    return ret;
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
    //printf("topic %s\n", topic);
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
    //printf("publish message OK\n");

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

    //printf("wait message OK\n");

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
    //printf("unlock exit\n");
    // 解锁
    pthread_mutex_unlock(&(pclient->lockmsg));
    //printf("push exit\n");
    return ret;
}

int setCallBack(MQClient* client, void *context, handleMessage msgarrive,
                handleLost connectlost, handleComplate sendcomplate) {
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
    client->context = context;
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


