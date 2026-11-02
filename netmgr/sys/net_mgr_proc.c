/**
 * @file net_mgr_proc.c
 * @brief netmgr 系统适配层：外部进程控制实现（/proc 遍历 + system）
 * @details 仅依赖内核 procfs 与 libc，不依赖 sys 层其它能力。
 */
#include "net_mgr_proc.h"

#include "net_mgr_err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/** 进程文件系统根目录 */
#define PROC_ROOT     "/proc"

/**
 * @brief 读取指定进程号对应的进程名
 * @param pid  进程号
 * @param name 输出缓冲，不可为空
 * @param len  输出缓冲长度
 * @return 1 读取成功；0 进程不存在或读取失败
 */
static int proc_read_name(int pid, char* name, int len)
{
    char path[128] = {0}; /* /proc/<pid>/comm 路径 */
    FILE* fp = NULL;      /* 进程名文件句柄 */
    int ok = 0;           /* 读取结果标志 */

    if (NULL == name || len <= 1)
        return 0;

    snprintf(path, sizeof(path), PROC_ROOT "/%d/comm", pid);
    fp = fopen(path, "r");
    if (NULL == fp)
        return 0; /* 进程已退出属于正常竞争，不视为异常 */

    if (NULL != fgets(name, len, fp))
    {
        name[strcspn(name, "\r\n")] = '\0';
        ok = 1;
    }
    fclose(fp);
    return ok;
}

/**
 * @brief 探测指定名称的进程是否存在
 * @param name 进程名（不含路径），不可为空
 * @return 1 存在；0 不存在或入参非法
 */
int netmgr_proc_exist(const char* name)
{
    DIR* dir = NULL;           /* /proc 目录句柄 */
    struct dirent* ent = NULL; /* 目录项 */
    char comm[64] = {0};       /* 进程名缓冲 */
    int pid = 0;               /* 当前目录项对应的进程号 */
    int found = 0;             /* 是否已找到目标进程 */

    if (NULL == name || '\0' == name[0])
    {
        return 0;
    }

    dir = opendir(PROC_ROOT);
    if (NULL == dir)
    {
        return 0;
    }

    /* 遍历 /proc 下以数字命名的目录，逐个比较 comm 内容 */
    while (NULL != (ent = readdir(dir)))
    {
        if (!isdigit((unsigned char)ent->d_name[0]))
            continue;
        pid = atoi(ent->d_name);
        if (!proc_read_name(pid, comm, (int)sizeof(comm)))
            continue;
        if (0 == strncmp(comm, name, sizeof(comm)))
        {
            found = 1;
            break;
        }
    }

    closedir(dir);
    return found;
}

/**
 * @brief 结束指定名称的全部进程
 * @param name 进程名（不含路径），不可为空
 * @return 0 成功（未找到进程也视为成功）；负值为 NETMGRERR_* 错误码
 */
int netmgr_proc_kill(const char* name)
{
    DIR* dir = NULL;           /* /proc 目录句柄 */
    struct dirent* ent = NULL; /* 目录项 */
    char comm[64] = {0};       /* 进程名缓冲 */
    int pid = 0;               /* 当前目录项对应的进程号 */
    int err = 0;               /* 首个结束失败的错误码，0 表示全部成功 */

    if (NULL == name || '\0' == name[0])
    {
        return -NETMGRERR_PARAM;
    }

    dir = opendir(PROC_ROOT);
    if (NULL == dir)
    {
        return -NETMGRERR_FILE;
    }

    /* 遍历 /proc 结束全部同名进程，某一条失败不影响其它进程 */
    while (NULL != (ent = readdir(dir)))
    {
        if (!isdigit((unsigned char)ent->d_name[0]))
            continue;
        pid = atoi(ent->d_name);
        if (pid == (int)getpid())
            continue;
        if (!proc_read_name(pid, comm, (int)sizeof(comm)))
            continue;
        if (0 != strncmp(comm, name, sizeof(comm)))
            continue;
        if (0 != kill(pid, SIGTERM) && 0 == err)
            err = -NETMGRERR_PROC;
    }

    closedir(dir);
    return err;
}

/**
 * @brief 执行 shell 命令并等待结束
 * @param fmt 命令格式串，不可为空
 * @return 0 命令返回成功；负值为 NETMGRERR_* 错误码
 */
int netmgr_proc_exec(const char* fmt, ...)
{
    char cmd[128] = {0}; /* 完整命令行 */
    va_list ap;          /* 可变参数表 */
    int len = 0;         /* 格式化结果长度 */
    int status = 0;      /* 命令退出状态 */
    int ret = 0;         /* system 返回值 */

    if (NULL == fmt || '\0' == fmt[0])
    {
        return -NETMGRERR_PARAM;
    }

    va_start(ap, fmt);
    len = vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (len <= 0 || len >= (int)sizeof(cmd))
    {
        return -NETMGRERR_BUFF;
    }

    ret = system(cmd);
    if (ret < 0)
    {
        return -NETMGRERR_PROC;
    }
    if (!WIFEXITED(ret))
    {
        return -NETMGRERR_PROC;
    }
    status = WEXITSTATUS(ret);
    if (0 != status)
    {
        return -NETMGRERR_PROC;
    }
    return 0;
}
