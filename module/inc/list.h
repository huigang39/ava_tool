#ifndef LIST_H
#define LIST_H

#include "macrodef.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

/* 获取链表首节点所在的宿主结构体指针 */
#define LIST_FIRST_ENTRY(entry, type, member) CONTAINER_OF((entry)->next, (type), (member))

/* 正向遍历链表节点指针 */
#define LIST_FOR_EACH(entry, head)            for ((entry) = (head)->next; (entry) != (head); (entry) = (entry)->next)

/* 反向遍历链表节点指针 */
#define LIST_FOR_EACH_PREV(entry, head)       for ((entry) = (head)->prev; (entry) != (head); (entry) = (entry)->prev)

/* 正向遍历链表，并将节点转换为宿主结构体 */
#define LIST_FOR_EACH_ENTRY(entry, head, member)                                                           \
        for ((entry) = CONTAINER_OF((head)->next, typeof(*(entry)), (member)); &(entry)->member != (head); \
             (entry) = CONTAINER_OF((entry)->member.next, typeof(*(entry)), (member)))

/* 反向遍历链表，并将节点转换为宿主结构体 */
#define LIST_FOR_EACH_ENTRY_REVERSE(entry, head, member)                                                   \
        for ((entry) = CONTAINER_OF((head)->prev, typeof(*(entry)), (member)); &(entry)->member != (head); \
             (entry) = CONTAINER_OF((entry)->member.prev, typeof(*(entry)), (member)))

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

/* 双向循环链表节点 */
typedef struct list_head {
        struct list_head *prev;
        struct list_head *next;
} list_head_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化链表头节点，使其自指
 *
 * @param head
 */
void list_init(list_head_t *head);

/**
 * @brief 将 entry 插入到 head 之后 (头插)
 *
 * @param entry
 * @param head
 */
void list_add(list_head_t *entry, list_head_t *head);

/**
 * @brief 将 entry 插入到 head 之后 (尾插)
 *
 * @param entry
 * @param head
 */
void list_add_tail(list_head_t *entry, list_head_t *head);

/**
 * @brief 将 entry 从链表中删除, 并将指针置空便于调试
 *
 * @param entry
 */
void list_del(list_head_t *entry);

/**
 * @brief 判断链表是否为空
 *
 * @param head
 */
u8 list_empty(const list_head_t *head);

/**
 * @brief 若 list 非空，则拼接到 head
 *
 * @param list
 * @param head
 */
void list_splice(const list_head_t *list, list_head_t *head);

/**
 * @brief 用 new_entry 替换 old_entry
 *
 * @param old_entry
 * @param new_entry
 */
void list_replace(const list_head_t *old_entry, list_head_t *new_entry);

/**
 * @brief 替换后重置 old_entry 为独立链表
 *
 * @param old_entry
 * @param new_entry
 */
void list_replace_init(list_head_t *old_entry, list_head_t *new_entry);

/**
 * @brief 将 entry 移动到 head 之后
 *
 * @param entry
 * @param head
 */
void list_move(list_head_t *entry, list_head_t *head);

/**
 * @brief 将 entry 移动到 head 之前
 *
 * @param entry
 * @param head
 */
void list_move_tail(list_head_t *entry, list_head_t *head);

#ifdef __cplusplus
}
#endif

#endif // !LIST_H
