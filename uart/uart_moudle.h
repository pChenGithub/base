#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>
#include <semaphore.h>

#define UART_MOUDLE_ERR_CHECKPARAM   1   // 参数检查失败
#define UART_MOUDLE_ERR_MALLOC              2   // 内存分配失败
#define UART_MOUDLE_ERR_OPEN              3   // 打开串口失败
#define UART_MOUDLE_ERR_PTHREAD              4  // 创建线程失败
#define UART_MOUDLE_ERR_GETATTR              5  // 获取串口属性失败
#define UART_MOUDLE_ERR_SETATTR              6  // 获取串口属性失败
#define UART_MOUDLE_ERR_SETSPEED              7  // 设置串口波特率是吧
#define UART_MOUDLE_ERR_TIMEOUT              8  // 设置串口波特率是吧
#define UART_MOUDLE_ERR_RECVTIMEOUT      9 // 等待消息超时
#define UART_MOUDLE_ERR_BUFFSIZE      10 // 缓存太小

typedef enum {
    BAUDRATE_9600 = 0,
    BAUDRATE_19200,
    BAUDRATE_115200,
} UART_MOUDLE_BAUDRATE_TYPE;

typedef enum {
    SBIT_1 = 0,
    SBIT_1_5,
    SBIT_2,
} UART_MOUDLE_STOPBIT_TYPE;

typedef enum {
    EVENT_ODD = 0,
    EVENT_EVEN,
    EVENT_NONE,
} UART_MOUDLE_EVENT_TYPE;

#define UM_LOCKTIMEOUT_10MS 50 // 拿锁超时时间(10ms)

typedef void (*hand_uart_msg)(unsigned char* buff, int datasize);
typedef struct {
    int fd;
#if 0
    // 设置波特率
    UART_MOUDLE_BAUDRATE_TYPE   baudrate;
    // 设置停止位
    UART_MOUDLE_STOPBIT_TYPE        stopbit;
    // 设置校验格式
    UART_MOUDLE_EVENT_TYPE             event;
#endif
    pthread_t pid;
    //char devname[16];
    // 接收串口缓存
    // 这里如果不是用的 unsigned 会导致收到数据打印不对
    // 7e 80 00 01 00 00 80 00 00 01 ca ca 7
    // 7e ffffff80 00 01 00 00 ffffff80 00 00 01 ffffffca ffffffca 7e
    unsigned char rcvbuff[64];
    // 每次提交发送消息需要加锁
    pthread_mutex_t lockmsg;
    /*********************************************/
    // 异步读取处理参数
    hand_uart_msg hand_msg;
    /*********************************************/
    /* 同步读取控制参数 */
    sem_t wait_recv;
    // 同步消息返回的code
    int wait_code;
    // 同步数据目标缓存和长度
    unsigned char* sync_buff;
    int sync_buff_len;
    // 判断同步消息
    char sync_flag;
} UART_ENTRY;

int start_uart(UART_ENTRY** entry, const char* devname, UART_MOUDLE_BAUDRATE_TYPE speed, 
    UART_MOUDLE_STOPBIT_TYPE stop, UART_MOUDLE_EVENT_TYPE event);
int stop_uart(UART_ENTRY* entry);
int set_uart(UART_ENTRY* entry, UART_MOUDLE_BAUDRATE_TYPE speed, 
    UART_MOUDLE_STOPBIT_TYPE stop, UART_MOUDLE_EVENT_TYPE event);
// 设置回调处理异步消息
int set_uart_ck(UART_ENTRY* entry, hand_uart_msg hand);
// 发送消息,同步等回复
int send_byte_uart_wait(UART_ENTRY* entry, const unsigned char *send, int sendlen, unsigned char *recv, int recvlen, int waitMs);
int send_uart_wait(UART_ENTRY* entry, const char* send, char* recv, int recvlen, int waitMs);
// 发送消息,不等,异步
int send_uart_one();

#ifdef __cplusplus
}
#endif
