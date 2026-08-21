#ifndef LIST_H
#define LIST_H

#include "macrodef.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

/* 获取链表首节点所在的宿主结构体指针 */
#define LIST_FIRST_ENTRY(entry, type, member) CONTAINER_OF((entry)->next, type, member)

/* 正向遍历链表节点指针 */
#define LIST_FOR_EACH(entry, head) \
    for ((entry) = (head)->next; (entry) != (head); (entry) = (entry)->next)

/* 反向遍历链表节点指针 */
#define LIST_FOR_EACH_PREV(entry, head) \
    for ((entry) = (head)->prev; (entry) != (head); (entry) = (entry)->prev)

/* 正向遍历链表,并将节点转换为宿主结构体 */
#define LIST_FOR_EACH_ENTRY(entry, head, member)                           \
    for ((entry) = CONTAINER_OF((head)->next, TYPEOF(*(entry)), (member)); \
         &(entry)->member != (head);                                       \
         (entry) = CONTAINER_OF((entry)->member.next, TYPEOF(*(entry)), (member)))

/* 反向遍历链表,并将节点转换为宿主结构体 */
#define LIST_FOR_EACH_ENTRY_REVERSE(entry, head, member)                   \
    for ((entry) = CONTAINER_OF((head)->prev, TYPEOF(*(entry)), (member)); \
         &(entry)->member != (head);                                       \
         (entry) = CONTAINER_OF((entry)->member.prev, TYPEOF(*(entry)), (member)))

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

/* 双向循环链表节点 */
struct list_head {
    struct list_head *prev;
    struct list_head *next;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化链表头节点,使其自指
 *
 * @param head
 */
void list_init(struct list_head *head);

/**
 * @brief 将 entry 插入到 head 之后 (头插)
 *
 * @param entry
 * @param head
 */
void list_add(struct list_head *entry, struct list_head *head);

/**
 * @brief 将 entry 插入到 head 之后 (尾插)
 *
 * @param entry
 * @param head
 */
void list_add_tail(struct list_head *entry, struct list_head *head);

/**
 * @brief 将 entry 从链表中删除, 并将指针置空便于调试
 *
 * @param entry
 */
void list_del(struct list_head *entry);

/**
 * @brief 判断链表是否为空
 *
 * @param head
 */
uint8_t list_empty(const struct list_head *head);

/**
 * @brief 若 list 非空,则拼接到 head
 *
 * @param list
 * @param head
 */
void list_splice(const struct list_head *list, struct list_head *head);

/**
 * @brief 用 new_entry 替换 old_entry
 *
 * @param old_entry
 * @param new_entry
 */
void list_replace(const struct list_head *old_entry, struct list_head *new_entry);

/**
 * @brief 替换后重置 old_entry 为独立链表
 *
 * @param old_entry
 * @param new_entry
 */
void list_replace_init(struct list_head *old_entry, struct list_head *new_entry);

/**
 * @brief 将 entry 移动到 head 之后
 *
 * @param entry
 * @param head
 */
void list_move(struct list_head *entry, struct list_head *head);

/**
 * @brief 将 entry 移动到 head 之前
 *
 * @param entry
 * @param head
 */
void list_move_tail(struct list_head *entry, struct list_head *head);

#ifdef __cplusplus
}
#endif

#endif // !LIST_H
