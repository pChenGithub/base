/** 
 * 串口操作集
*/
#ifndef __UART_H__
#define __UART_H__
#ifdef __cplusplus
extern "C" {
#endif

#define UARTOPTERR_CHECKPARAM   1   // 参数检查失败
#define UARTOPTERR_OPEN_FAIL    2
#define UARTOPTERR_READ_FAIL    3
#define UARTOPTERR_READ_ZERO    4   // 读取0长度内容
#define UARTOPTERR_SELECT_ERR   5   // select错误
#define UARTOPTERR_SELECT_TIMEOUT   6   // select超时


typedef enum {
    BAUDRATE_9600 = 0,
    BAUDRATE_19200,
    BAUDRATE_115200,
} BAUDRATE_TYPE;

typedef enum {
    SBIT_1 = 0,
    SBIT_1_5,
    SBIT_2,
} STOPBIT_TYPE;

typedef enum {
    EVENT_ODD = 0,
    EVENT_EVEN,
} EVENT_TYPE;

// 打开串口
int uart_open(const char* dev, BAUDRATE_TYPE baudrate, STOPBIT_TYPE bit, EVENT_TYPE even);
int uart_setopt(int fd, BAUDRATE_TYPE baudrate, STOPBIT_TYPE bit, EVENT_TYPE even);
int uart_read(int fd, char* buff, int bufflen);
int uart_read_timeout(int fd, char* buff, int bufflen, int ms);
int uart_write(int fd, char* buff, int bufflen);
int uart_close(int fd);

#ifdef __cplusplus
}
#endif
#endif

