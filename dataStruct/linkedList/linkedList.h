#ifndef __LINKEDLIST_H__
#define __LINKEDLIST_H__
#ifdef __cplusplus
extern "C" {
#endif
#include <pthread.h>
#define LKLIST_ERR_CHECKPARAM   1   // 参数错误
#define LKLIST_ERR_CALLOCFAIL   2   // 执行calloc失败
#define LKLIST_ERR_INIT_MUTEXLOCK   3   // 初始化锁失败
#define LKLIST_ERR_LOCK   4   // 加锁锁失败
#define LKLIST_ERR_HAS_NODE   5   // 未清空链表
#define LKLIST_ERR_INVALID_INDEX   6   // 无效的下标
#define LKLIST_ERR_FOREACH_BREAK   7   // 遍历中断
#define LKLIST_ERR_INVALID_NODE   8   // 无效的节点，不属于该链表头
// 模块内拿锁超时事件设置
#define LKLIST_LOCK_TIMEOUT_US 10000  // 拿锁超时事件,微妙
typedef struct _node{
    struct _node* pre;      // 上一个节点
    struct _node* next;     // 下一个节点
    void* head; // 指向链表头
} LINK_NODE;

typedef struct {
    LINK_NODE node;
    unsigned int nodecount;      // 链表节点数量,不包括head
    pthread_mutex_t lock_linked;
} LINK_HEAD;

/************************ 链表头操作 ******************************** */
// 静态申请一个链表头
#define INIT_LINKEDLIST(head) \
    LINK_HEAD head = { \
        {&head, &head}, \
        0, \
        PTHREAD_MUTEX_INITIALIZER, \
    } \
// 创建一个链表头
int init_linkedlist(LINK_HEAD** head);
// 释放链表头
int free_linkedlist(LINK_HEAD* head);

/************************ 链表插入节点 ******************************** */
// 指定位置插入一项
// index表示在哪个地方插入,
// index=0表示在head后面插入,1表示在第一个node后插入,-1表示在倒数第一个node后插入
int insert_lknode(LINK_HEAD* head, LINK_NODE* node, int index);
// 前插,即在链表头后面插入,
int insert_head(LINK_HEAD* head, LINK_NODE* node);
// 后插,即在链表头前面插入
int insert_tail(LINK_HEAD* head, LINK_NODE* node);

/************************ 链表遍历节点 ******************************** */
// 遍历,回调函数,返回 <0可以中断遍历,并且返回值将会 在 foreach_lklist 返回
typedef int (*hand_node)(LINK_NODE* node);
// hand 回调函数
int foreach_lklist(LINK_HEAD* head, hand_node hand);
// head:链表头指针,
// _mnode:节点指针
#define FOREACH_LKLIST(head, _mnode) \
    for (_mnode=(head)->node.next;_mnode!=(&(head)->node);_mnode=_mnode->next)

/************************ node和业务结构体转换 ******************************** */
// 结构体操作
// 计算域在结构体中的地址偏移
#define OFFSETOF(type, member) ((size_t)&((type*)0)->member)
// 根据node地址获取结构体地址
// ptr:node指针,
// type:实体结构体类型,
// member:node在结构体的域名称
#define CONTAINER_OF(ptr, type, member) \
({ \
    (type*)((char*)ptr-OFFSETOF(type, member)); \
})

/************************ 链表删除节点 ******************************** */
// 指定位置删除一项
// 1表示在第一个node,-1表示倒数第一个node
// node会返回删除的node的指针，删除节点，必须拿到节点然后用户回收
int remove_lknode_index(LINK_HEAD* head, int index, LINK_NODE** node);
// 指定node删除
int remove_lknode(LINK_HEAD* head, LINK_NODE* node);
// 清空链表,用户必须在 hand 回调中处理用户数据,否则后果自负
int clear_lklist(LINK_HEAD* head, hand_node hand);
#define CLEAR_LKLIST(head, type, member) ({ \
    LINK_NODE* _mnode = NULL; \
    for (_mnode=(head)->node.next;_mnode!=(&((head)->node));) { \
        type* _mdata = CONTAINER_OF(_mnode, type, member); \
        _mnode = _mnode->next; \
        free(_mdata); \
    } \
    (head)->nodecount = 0; \
    (head)->node.pre = (LINK_NODE*)(head); \
    (head)->node.next = (LINK_NODE*)(head); \
})

#ifdef __cplusplus
}
#endif
#endif

