#ifndef __MIXSHELL_H__
#define __MIXSHELL_H__
#ifdef __cplusplus
extern "C" {
#endif
#include "mixShell_errno.h"
typedef enum {
    SHELL_RET_OK = 0,
    SHELL_RET_ERR,
    SHELL_RET_IGNORE,
} SHELL_RET_TYPE;
typedef SHELL_RET_TYPE (*shell_ret_line)(const char* linestr);
// C代码混合shell编程
// 执行一个shell命令，并输出命令结果（如果有结果），返回值返回该条命令执行的结果（是否成功/执行状态）
// 输出结果只取第一行,如果需要所有的结果,用exeShellWait
int exeShell(const char* cmd, char* out, const unsigned int out_len);
// 执行命令,实现一个回调函数shell_ret_line获取结果,并返回对结果的判断
int exeShellWait(const char* cmd, shell_ret_line handret);
#ifdef __cplusplus
}
#endif
#endif


