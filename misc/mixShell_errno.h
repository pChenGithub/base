#ifndef __MIXSHELL_ERRNO_H__
#define __MIXSHELL_ERRNO_H__
#define MIXSHELLERR_CHECKPARAM      1
#define MIXSHELLERR_POPEN_FAIL      2   // 执行popen出错
#define MIXSHELLERR_FGETS_FAIL      3   // 执行fgets失败
#define MIXSHELLERR_EXESHELL_FAIL   4   // 最终结果，shell结果是失败
#define MIXSHELLERR_EXESHELL_OK     5   // 最终结果，shell结果是成功
#define MIXSHELLERR_EXESHELL_TIMEOUT    6   // 最终结果，shell执行超时

#endif

