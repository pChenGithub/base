#include "mixShell.h"
#include "mixShell_errno.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief exeShell
 * 执行shell命令 获取第一行结果
 * @param cmd 命令
 * @param out 接收内容数组
 * @param out_len out数组长度
 * @return
 */
int exeShell(const char* cmd, char* out, const unsigned int out_len) {
    int ret = 0;
    // 验证参数
    if (NULL==cmd)
        return -MIXSHELLERR_CHECKPARAM;

    // 
    FILE* fp = popen(cmd, "r");
    if (NULL==fp)
        return -MIXSHELLERR_POPEN_FAIL;

    if (NULL==out || 0==out_len)
        goto closefd_exit;

    memset(out,0x00,out_len);
    // 读取执行结果
    // 需要命令里面控制获取结果行数，这里默认只取第一行
    if (NULL==fgets(out, out_len-1, fp))
        ret = -MIXSHELLERR_FGETS_FAIL;

    // 执行命令成功

closefd_exit:
    pclose(fp);
    fp = NULL;
    return ret;
}

#define RETSTR_BUFF_LEN     128
int exeShellWait(const char* cmd, shell_ret_line handret) {
    int ret = 0;
    int tmpret = 0;
    // 验证参数
    if (NULL==cmd)
        return -MIXSHELLERR_CHECKPARAM;

    // 执行失败，注意是执行
    FILE* fp = popen(cmd, "r");
    if (NULL==fp)
        return -MIXSHELLERR_POPEN_FAIL;

    if (NULL==handret)
        goto closefd_exit;

    char* retstr = calloc(RETSTR_BUFF_LEN, 1);
    // 读取执行结果
    // 需要命令里面控制获取结果行数，每次获取最新行
    // 初始化ret,如果没有获取到命令结果,也视为失败
    ret = -MIXSHELLERR_EXESHELL_TIMEOUT;
    while (NULL!=fgets(retstr, RETSTR_BUFF_LEN-1, fp)) {
        tmpret = handret(retstr);
        if (SHELL_RET_ERR==tmpret) {
            ret = -MIXSHELLERR_EXESHELL_FAIL;
            break;
        } else if (SHELL_RET_OK==tmpret) {
            ret = 0;
            break;
        }
        // 返回的是 SHELL_RET_IGNORE 就继续往下读取
    }
    // 执行命令成功/失败
    //printf("执行命令结果%d\n", ret);

    free(retstr);
    retstr = NULL;

closefd_exit:
    pclose(fp);
    fp = NULL;
    return ret;
}


