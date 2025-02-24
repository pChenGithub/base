#include "uart.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

int uart_open(const char* dev, BAUDRATE_TYPE baudrate, STOPBIT_TYPE bit, EVENT_TYPE even) {
    int ret = 0;
    if (NULL==dev)
        return -UARTOPTERR_CHECKPARAM;

    // O_NOCTTY ：告诉内核这个程序不想作为控制终端。如果不指定该标志，任何输入都会影响程序（比如键盘中止信号等）。
    // O_NDELAY ：告诉内核这个程序不关注DCD信号线的状态。如果不指定该标志，当DCD信号线是空电压值的时候，程序将会进入睡眠
    int fd = open(dev, O_RDWR|O_NOCTTY|O_NDELAY);
    if (fd<0)
        return -UARTOPTERR_OPEN_FAIL;

    // F_SETFL,F_GETFL
    // 设置/获取文件flags
    //fcntl(fd,F_SETFL,flags);
    // 设置成非阻塞
    // fcntl(fd, F_SETFL, FNDELAY);
    // 设置成阻塞
    fcntl(fd, F_SETFL, 0);

    ret = uart_setopt(fd, baudrate, bit, even);
    if (ret<0)
        goto exit;

    return fd;

exit:
    close(fd);
    return ret;
}

int uart_close(int fd) {
    close(fd);
    return 0;
}

int uart_setopt(int fd, BAUDRATE_TYPE baudrate, STOPBIT_TYPE bit, EVENT_TYPE even) {
    struct termios options;
    if (fd<0)
        return -UARTOPTERR_CHECKPARAM;

#if 0
    struct termios
    {
        tcflag_t c_iflag;		/* input mode flags */
        tcflag_t c_oflag;		/* output mode flags */
        tcflag_t c_cflag;		/* control mode flags */
        tcflag_t c_lflag;		/* local mode flags */
        cc_t c_line;			/* line discipline */
        cc_t c_cc[NCCS];		/* control characters */
        speed_t c_ispeed;		/* input speed */
        speed_t c_ospeed;		/* output speed */
        #define _HAVE_STRUCT_TERMIOS_C_ISPEED 1
        #define _HAVE_STRUCT_TERMIOS_C_OSPEED 1
    };

    字段说明
    c_cflag	    控制设置
    c_lflag	    本地设置
    c_iflag	    输入设置
    c_oflag	    输出设置
    c_cc	    控制字符
    c_ispeed	输入速率
    c_ospeed	输出速率
#endif
    tcgetattr(fd, &options);

    // cfsetspeed()
    // 设置波特率
    switch (baudrate)
    {
    case BAUDRATE_9600:
        cfsetispeed(&options, B9600);
        cfsetospeed(&options, B9600);
        break;
    case BAUDRATE_19200:
        cfsetispeed(&options, B19200);
        cfsetospeed(&options, B19200);
        break;
    case BAUDRATE_115200:
        cfsetispeed(&options, B115200);
        cfsetospeed(&options, B115200);
        break;
    default:
        break;
    }

    options.c_cflag &= ~CSIZE; /* Mask the character size bits */
#if 0
    options.c_cflag |= CS5;
    options.c_cflag |= CS6;
    options.c_cflag |= CS7;
#else
    options.c_cflag |= CS8;    /* Select 8 data bits */
#endif

    // CLOCAL 修改控制模式，保证程序不会占用串口
    // CREAD 修改控制模式，能够从串口读取数据
    options.c_cflag |= (CLOCAL | CREAD);

    // 不使用流控制
    options.c_cflag &= ~CRTSCTS;
    // 硬件流控
    //opt.c_cflag |= CRTSCTS;
    // 软件流控
    //opt.c_cflag |= IXON|IXOFF|IXANY;

    /** 
     *  奇偶校验可以选择偶校验、奇校验、空格等方式，也可以不使用校验。
     *  如果要设置为偶校验的话，首先要将termios结构体中c_cflag设置PARENB标志，并清除PARODD标志。
        如果要设置奇校验，要同时设置termios结构体中c_cflag设置PARENB标志和PARODD标志。
        如果不想使用任何校验的话，清除termios结构体中c_cflag的PARENB位.
     */
    /** 
     * 设置奇偶校验位可以通过修改 termios 结构体中的 c_cflag 成员来实现，
     * 若无校验，则将 PARENB 位设为 0；
     * 若有校验，则 PARENB 为 1。之后再根据 PARODD 来区分奇偶校验，PARODD 为 1 表示奇校验，PARODD 为 0 表示偶校验
    */
    // 设置奇偶校验位
    // 无校验
    
    // https://wenku.baidu.com/view/7a3ceac982c758f5f61fb7360b4c2e3f5627255c.html
    // https://blog.csdn.net/weixin_31205717/article/details/116770726
    #if 0
    // 无校验
    options.c_cflag &= ~PARENB;
    // 偶校验
    options.c_cflag |= PARENB;
    options.c_cflag &= ~PARODD;    
    // 奇校验
    opt.c_cflag |= (PARODD | PARENB);
    // 空格校验
    opt.c_cflag &= ~PARENB;
    opt.c_cflag &= ~CSTOPB;
    #endif

    /** 
     * CSTOPB 位为 1 表示 2 位停止位，CSTOPB 位为 0 标志 1 位停止位
    */
    // 设置停止位
    options.c_cflag &= ~CSTOPB;

    //150 单位：百毫秒，没有满足VMIN条件，这边会超时返回，设置为0,没有超时
    // 设置最少字符和等待时间
    options.c_cc[VMIN] = 1;     // 读数据的最小字节数
    /*
     * 0：缓冲区有>=0的数据就会读，返回，
     * 1：缓冲区有1个字节数据就会读，返回，
     * 注意这个数字只是决定读buffer这个操作会不会做，不是说一定要读设置的个数，
     * 如果期望读取的字节数（传下来的长度len），
     * 当<VMIN，VMIN会被设置成len，
     * 当>VMIN，达到VMIN条件，read返回，
     * 其他类推;否则阻塞
     */
    options.c_cc[VTIME]  = 0;   //等待第1个数据，单位是10s

    // 修改输出模式，原始数据输出
    options.c_oflag &= ~OPOST;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

#if 0
    // 参考312设置
    options.c_lflag&=~(ICANON|ECHO|ECHOE|ISIG);
    options.c_oflag&=~OPOST;
    options.c_oflag&=~ONLCR;
    options.c_oflag&=~OCRNL;

    options.c_cflag|=CLOCAL;
    options.c_cflag&=~CRTSCTS;

    options.c_iflag &= ~(IXON | IXOFF | IXANY);
    options.c_iflag &= ~(INLCR | IGNCR | ICRNL);

    // m
    options.c_cflag |= PARENB;
    options.c_cflag |= PARODD;
    options.c_cflag |= CMSPAR;
    options.c_iflag |= INPCK;      /* Disnable parity checking */
#endif

    // 清空终端未完成的数据
    tcflush(fd, TCIFLUSH);

    // 设置新的参数
    tcsetattr(fd, TCSANOW, &options);
    return 0;
}

int uart_read(int fd, char* buff, int bufflen) {
    if (fd<0 || NULL==buff || bufflen<=0)
        return -UARTOPTERR_CHECKPARAM;

    int ret = read(fd, buff, bufflen);
    if (ret<0)
        return -UARTOPTERR_READ_FAIL;

    if (0==ret)
        return -UARTOPTERR_READ_ZERO;
    return ret;
}

int uart_read_timeout(int fd, char* buff, int bufflen, int ms) {

    if (fd<0 || NULL==buff || bufflen<=0)
        return -UARTOPTERR_CHECKPARAM;

    int ret = 0;
    fd_set rfds;
    struct timeval tv;
    // 每次循环都要清空集合，否则不能检测描述符变化
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    // 设置超时
    tv.tv_sec = ms/1000;
    tv.tv_usec = 1000*(ms%1000);

    // 第一个参数,监听最大描述符值加1
    ret = select(fd+1, &rfds, NULL, NULL, &tv);
    if (ret<0) {
        return -UARTOPTERR_SELECT_ERR;
    } else if (0==ret) {
        // 超时
        return -UARTOPTERR_SELECT_TIMEOUT;
    }

    // 读取串口,这里如果不读,select会一直返回
    ret = read(fd, buff, bufflen);
    if (ret<0)
        return -UARTOPTERR_READ_FAIL;

    if (0==ret)
        return -UARTOPTERR_READ_ZERO;
    return ret;
}

int uart_write(int fd, char* buff, int bufflen) {
    if (fd<0 || NULL==buff || bufflen<=0)
        return -UARTOPTERR_CHECKPARAM;
    
    int ret = write(fd, buff, bufflen);
    if (ret<0)
        return -UARTOPTERR_READ_FAIL;
    return ret;
}

