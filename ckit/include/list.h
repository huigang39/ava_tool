#ifndef LIST_H
#define LIST_H

#include "macrodef.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 获取链表首节点所在的宿主结构体指针
#define LIST_FIRST_ENTRY(entry, type, member) \
    CONTAINER_OF((entry)->next, type, member)

// 正向遍历链表节点指针
#define LIST_FOR_EACH(entry, head) \
    for ((entry) = (head)->next; (entry) != (head); (entry) = (entry)->next)

// 反向遍历链表节点指针
#define LIST_FOR_EACH_PREVIOUS(entry, head) \
    for ((entry) = (head)->prev; (entry) != (head); (entry) = (entry)->prev)

// 正向遍历链表,并将节点转换为宿主结构体
#define LIST_FOR_EACH_ENTRY(entry, head, member)                           \
    for ((entry) = CONTAINER_OF((head)->next, TYPEOF(*(entry)), (member)); \
         &(entry)->member != (head);                                       \
         (entry) =                                                         \
             CONTAINER_OF((entry)->member.next, TYPEOF(*(entry)), (member)))

// 反向遍历链表,并将节点转换为宿主结构体
#define LIST_FOR_EACH_ENTRY_REVERSE(entry, head, member)                   \
    for ((entry) = CONTAINER_OF((head)->prev, TYPEOF(*(entry)), (member)); \
         &(entry)->member != (head);                                       \
         (entry) =                                                         \
             CONTAINER_OF((entry)->member.prev, TYPEOF(*(entry)), (member)))

// 双向循环链表节点
struct list_head {
    struct list_head *prev; // 前驱节点
    struct list_head *next; // 后继节点
};

/**
 * @brief              初始化链表头节点,使其自指
 * @param[in,out] head 链表头
 */
void list_init(struct list_head *head);

/**
 * @brief               将 entry 插入到 head 之后 (头插)
 * @param[in,out] entry 待操作节点
 * @param[in,out] head  链表头
 */
void list_add(struct list_head *entry, struct list_head *head);

/**
 * @brief               将 entry 插入到 head 之后 (尾插)
 * @param[in,out] entry 待操作节点
 * @param[in,out] head  链表头
 */
void list_add_tail(struct list_head *entry, struct list_head *head);

/**
 * @brief               将 entry 从链表中删除, 并将指针置空便于调试
 * @param[in,out] entry 待操作节点
 */
void list_del(struct list_head *entry);

/**
 * @brief          判断链表是否为空
 * @param[in] head 链表头
 */
uint8_t is_list_empty(const struct list_head *head);

/**
 * @brief              若 list 非空,则拼接到 head
 * @param[in]     list 待操作链表
 * @param[in,out] head 链表头
 */
void list_splice(const struct list_head *list, struct list_head *head);

/**
 * @brief                   用 new_entry 替换 old_entry
 * @param[in]     old_entry 原节点
 * @param[in,out] new_entry 替换节点
 */
void list_replace(const struct list_head *old_entry,
                  struct list_head *new_entry);

/**
 * @brief                   替换后重置 old_entry 为独立链表
 * @param[in,out] old_entry 原节点
 * @param[in,out] new_entry 替换节点
 */
void list_replace_init(struct list_head *old_entry,
                       struct list_head *new_entry);

/**
 * @brief               将 entry 移动到 head 之后
 * @param[in,out] entry 待操作节点
 * @param[in,out] head  链表头
 */
void list_move(struct list_head *entry, struct list_head *head);

/**
 * @brief               将 entry 移动到 head 之前
 * @param[in,out] entry 待操作节点
 * @param[in,out] head  链表头
 */
void list_move_tail(struct list_head *entry, struct list_head *head);

#ifdef __cplusplus
}
#endif

#endif
