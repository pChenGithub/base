#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include "net_dhcp.h"
#include "net_errno.h"
#include "mixShell.h"

#define CMDSTR_LEN  64
#define IFNAME_LEN  16
#define DIRTY_FLAG  0x01
typedef struct {
    int flag;
    pthread_t pid;
    int timeouts;
    hand_dhcpstatus hand;
    char ifname[IFNAME_LEN];
    char cmdstr[CMDSTR_LEN];
} DHCP_PTHREAD;

SHELL_RET_TYPE hand_retstr(const char* linestr) {
    printf("shell结果 %s\n", linestr);
    if (!strncmp(linestr, "No lease, failing", 17))
        return SHELL_RET_ERR;
#if 0
    else if (!strncmp(linestr, "Sending select for", 18))
        return SHELL_RET_OK;
#endif
    return SHELL_RET_IGNORE;
}

static void* run_dhcp(void* arg) {
    int ret = 0;
    DHCP_PTHREAD* pcmd = (DHCP_PTHREAD*)arg;
    memset(pcmd->cmdstr, 0, CMDSTR_LEN);
    snprintf(pcmd->cmdstr, CMDSTR_LEN, "udhcpc -i %s", pcmd->ifname);
    if (pcmd->timeouts>0) {
        int cmdlen = strlen(pcmd->cmdstr);
        snprintf(pcmd->cmdstr+cmdlen, CMDSTR_LEN-cmdlen, " -n -T %d", pcmd->timeouts);
    }

    printf("执行的命令 %s\n", pcmd->cmdstr);

    if (pcmd->hand)
        pcmd->hand(DHCP_RUNNING, pcmd->ifname);

dhcp_again:
    ret = exeShellWait(pcmd->cmdstr, hand_retstr);
    if (ret<0 && -MIXSHELLERR_EXESHELL_TIMEOUT!=ret) {
        //printf("执行的命令失败，错误码 %d\n", ret);
        ret = 0;
        if (pcmd->hand)
            ret = pcmd->hand(DHCP_FAIL_END, pcmd->ifname);
        if (-NET_DHCP_AGAIN==ret)
            goto dhcp_again;
        else
            goto exit;
    }
    //printf("执行的命令成功，结果 %s\n", pcmd->cmdstr);   
    if (pcmd->hand)
        pcmd->hand(DHCP_SUCC_END, pcmd->ifname);

exit:
    // 变回干净
    printf("标记为干净\n");
    pcmd->flag &= ~(DIRTY_FLAG);
    return NULL;
}

#define MAX_DHCP_COUNT  2
static DHCP_PTHREAD cmd_entry[MAX_DHCP_COUNT] = {0};
int start_dhcp(const char* ifname, int timeouts, hand_dhcpstatus hand) {
    int ret = 0;
    int i = 0;
    int index = -1;  // 
    DHCP_PTHREAD* pcmd = cmd_entry;
    if (NULL==ifname || timeouts<0)
        return -NETERR_CHECK_PARAM;

    // 找到干净的cmd_entry，并且遍历是否已经启动ifname的dhcp
    for (i=0;i<MAX_DHCP_COUNT;i++) {
        if (DIRTY_FLAG&pcmd[i].flag && !strncmp(pcmd[i].ifname, ifname, strlen(ifname)+1))
            return -NETERR_DHCP_HASTHREAD;
        else if (index<0 && !(DIRTY_FLAG&pcmd[i].flag))
            index = i;
    }

    if (index<0)
        return -NETERR_FINDNO_ENTRY;

    pcmd += index;
    pcmd->timeouts = timeouts;
    pcmd->hand = hand;
    snprintf(pcmd->ifname, IFNAME_LEN, "%s", ifname);
    ret = pthread_create(&(pcmd->pid), NULL, run_dhcp, pcmd);
    if (ret<0)
        return -NETERR_PTHREADCREATE_FAIL;      
    pthread_detach(pcmd->pid);
    // 标记为脏
    printf("标记为脏\n");
    pcmd->flag |= DIRTY_FLAG;
    return 0;
}

int stop_dhcp(const char* ifname) {

    if (NULL==ifname)
        return -NETERR_CHECK_PARAM;

    int i = 0;
    DHCP_PTHREAD* pcmd = cmd_entry;

    for (i=0;i<MAX_DHCP_COUNT;i++) {
        if (DIRTY_FLAG&pcmd[i].flag && !strncmp(pcmd[i].ifname, ifname, strlen(ifname)+1))
            break;
    }

    if (i>=MAX_DHCP_COUNT)
        return -NETERR_FINDNO_ENTRY;

    pcmd += i;
    if (0!=pthread_cancel(pcmd->pid))
        return -NETERR_DHCP_STOPTHREAD;

    // 等待线程退出
    pthread_join(pcmd->pid, NULL);
    // 变回干净
    pcmd->flag &= ~(DIRTY_FLAG);
    return 0;
}
