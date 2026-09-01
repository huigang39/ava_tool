#include "list.h"

/**
 * @brief                   在相邻节点之间插入节点
 * @param[in,out] entry     待插入节点
 * @param[in,out] prev_node 前驱节点
 * @param[in,out] next_node 后继节点
 */
static void list_add__(struct list_head *entry,
                       struct list_head *prev_node,
                       struct list_head *next_node);

/**
 * @brief                   连接被移除节点的前后节点
 * @param[in,out] prev_node 前驱节点
 * @param[in,out] next_node 后继节点
 */
static void list_del__(struct list_head *prev_node,
                       struct list_head *next_node);

/**
 * @brief              将链表节点接入另一链表
 * @param[in]     list 参数模式
 * @param[in,out] head 链表头节点
 */
static void list_splice__(const struct list_head *list, struct list_head *head);

void list_init(struct list_head *head)
{
    head->next = head;
    head->prev = head;
}

void list_add(struct list_head *entry, struct list_head *head)
{
    list_add__(entry, head, head->next);
}

void list_add_tail(struct list_head *entry, struct list_head *head)
{
    list_add__(entry, head->prev, head);
}

void list_del(struct list_head *entry)
{
    if (entry->prev == NULL || entry->next == NULL) {
        return;
    }

    list_del__(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

uint8_t is_list_empty(const struct list_head *head)
{
    return head->next == head;
}

void list_splice(const struct list_head *list, struct list_head *head)
{
    if (!is_list_empty(list)) {
        list_splice__(list, head);
    }
}

void list_replace(const struct list_head *old_entry,
                  struct list_head *new_entry)
{
    new_entry->next = old_entry->next;
    new_entry->next->prev = new_entry;
    new_entry->prev = old_entry->prev;
    new_entry->prev->next = new_entry;
}

void list_replace_init(struct list_head *old_entry, struct list_head *new_entry)
{
    list_replace(old_entry, new_entry);
    list_init(old_entry);
}

void list_move(struct list_head *entry, struct list_head *head)
{
    list_del__(entry->prev, entry->next);
    list_add(entry, head);
}

void list_move_tail(struct list_head *entry, struct list_head *head)
{
    list_del__(entry->prev, entry->next);
    list_add_tail(entry, head);
}

static void list_add__(struct list_head *entry,
                       struct list_head *prev_node,
                       struct list_head *next_node)
{
    next_node->prev = entry;
    entry->next = next_node;
    entry->prev = prev_node;
    prev_node->next = entry;
}

static void list_del__(struct list_head *prev_node, struct list_head *next_node)
{
    prev_node->next = next_node;
    next_node->prev = prev_node;
}

static void list_splice__(const struct list_head *list, struct list_head *head)
{
    struct list_head *first = list->next;
    struct list_head *prev = list->prev;
    struct list_head *at = head->next;

    first->prev = head;
    head->next = first;

    prev->next = at;
    at->prev = prev;
}
