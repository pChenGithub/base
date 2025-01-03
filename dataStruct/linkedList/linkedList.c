#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "linkedList.h"

int init_linkedlist(LINK_HEAD** head) {
    LINK_HEAD *phead = NULL;
    if (NULL==head)
        return -LKLIST_ERR_CHECKPARAM;
    // 创建链表头
    phead = calloc(1, sizeof(LINK_HEAD));
    if (NULL==phead)
        return -LKLIST_ERR_CALLOCFAIL;
    // 初始化链表头
    phead->node.next = & phead->node;
    phead->node.pre = & phead->node;
    phead->nodecount = 0;
    // 初始化互斥锁
    if (0!=pthread_mutex_init(&phead->lock_linked, NULL)) {
        free(phead);
        return -LKLIST_ERR_INIT_MUTEXLOCK;
    }
    *head = phead;
    return 0;
}

int free_linkedlist(LINK_HEAD* head) {
    if (NULL==head)
        return -LKLIST_ERR_CHECKPARAM;
    // 判断是否清空链表
    if (head->nodecount>0)
        return -LKLIST_ERR_HAS_NODE;
    // 销毁锁
    pthread_mutex_destroy(&head->lock_linked);
    // 销毁链表头
    free(head);
    return 0;
}

int insert_lknode(LINK_HEAD* head, LINK_NODE* node, int index) {
    LINK_NODE* pnode = (LINK_NODE*)head;
    if (NULL==head || NULL==node)
        return -LKLIST_ERR_CHECKPARAM;
    // 链表加锁
    if (0!=pthread_mutex_trylock(&head->lock_linked)) {
        usleep(LKLIST_LOCK_TIMEOUT_US);
        if (0!=pthread_mutex_trylock(&head->lock_linked))
            return -LKLIST_ERR_LOCK;
    }
    // 判断下标,下标应该是在 -(nodecount) 到 nodecount 的闭区间
    if (index>(head->nodecount) || index<(-(head->nodecount))) {
        pthread_mutex_unlock(&head->lock_linked);
        return -LKLIST_ERR_INVALID_INDEX;
    }
    // 找到插入位置
    if (index>0) {
        while (index--)
            pnode = pnode->next;
    } else if (index<0) {
        while (index++)
            pnode = pnode->pre;
    }
    // pnode --- node 
    node->next = pnode->next;
    pnode->next->pre = node;
    pnode->next = node;
    node->pre = pnode;
    // 计数增加
    head->nodecount++;
    // 解锁
    pthread_mutex_unlock(&head->lock_linked);
    return 0;
}

int insert_head(LINK_HEAD* head, LINK_NODE* node) {
    if (NULL==head || NULL==node)
        return -LKLIST_ERR_CHECKPARAM;
    return insert_lknode(head, node, 0);
}

int insert_tail(LINK_HEAD* head, LINK_NODE* node) {
    if (NULL==head || NULL==node)
        return -LKLIST_ERR_CHECKPARAM;
    return insert_lknode(head, node, -1);
}

int remove_lknode(LINK_HEAD* head, int index, LINK_NODE** node) {
    LINK_NODE* pnode = (LINK_NODE*)head;
    if (NULL==head || NULL==node)
        return -LKLIST_ERR_CHECKPARAM;
    // 加锁
    if (0!=pthread_mutex_trylock(&head->lock_linked)) {
        usleep(LKLIST_LOCK_TIMEOUT_US);
        if (0!=pthread_mutex_trylock(&head->lock_linked))
            return -LKLIST_ERR_LOCK;
    }
    // 判断下标,下标应该是在  -(nodecount) 到 nodecount 的闭区间
    if (0==index || index>(head->nodecount) || index<(-(head->nodecount))) {
        pthread_mutex_unlock(&head->lock_linked);
        return -LKLIST_ERR_INVALID_INDEX;
    }
    // 定位
    if (index>0) {
        while (index--)
            pnode = pnode->next;
    } else if (index<0) {
        while (index++)
            pnode = pnode->pre;
    }
    //
    pnode->pre->next = pnode->next;
    pnode->next->pre = pnode->pre;
    *node = pnode;
    // 计数减少
    head->nodecount--;
    // 解锁
    pthread_mutex_unlock(&head->lock_linked);
    return 0;
}

int foreach_lklist(LINK_HEAD* head, hand_node hand) {
    int ret = 0;
    if (NULL==head || NULL==hand)
        return -LKLIST_ERR_CHECKPARAM;
    // 加锁
    if (0!=pthread_mutex_trylock(&head->lock_linked)) {
        usleep(LKLIST_LOCK_TIMEOUT_US);
        if (0!=pthread_mutex_trylock(&head->lock_linked))
            return -LKLIST_ERR_LOCK;
    }
    // 遍历并回调
    LINK_NODE* node = head->node.next;
    for (int i=0;i<head->nodecount;i++) {
        if ((ret=hand(node))<0)
            break;
        node = node->next;
    }
    // 解锁
    pthread_mutex_unlock(&head->lock_linked);
    return ret;
}


