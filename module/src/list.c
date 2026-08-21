#include "list.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 在 prev_node 与 next_node 之间插入 entry
 *
 * @param entry
 * @param prev_node
 * @param next_node
 */
static void
list_add__(struct list_head *entry, struct list_head *prev_node, struct list_head *next_node)
{
    next_node->prev = entry;
    entry->next     = next_node;
    entry->prev     = prev_node;
    prev_node->next = entry;
}

/**
 * @brief 删除 prev_node 与 next_node 间的节点连接
 *
 * @param prev_node
 * @param next_node
 */
static void
list_del__(struct list_head *prev_node, struct list_head *next_node)
{
    prev_node->next = next_node;
    next_node->prev = prev_node;
}

/**
 * @brief 将 list 整体拼接到 head 之后
 *
 * @param list
 * @param head
 */
static void
list_splice__(const struct list_head *list, struct list_head *head)
{
    struct list_head *first = list->next;
    struct list_head *last  = list->prev;
    struct list_head *at    = head->next;

    first->prev = head;
    head->next  = first;

    last->next = at;
    at->prev   = last;
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
list_init(struct list_head *head)
{
    head->next = head;
    head->prev = head;
}

void
list_add(struct list_head *entry, struct list_head *head)
{
    list_add__(entry, head, head->next);
}

void
list_add_tail(struct list_head *entry, struct list_head *head)
{
    list_add__(entry, head->prev, head);
}

void
list_del(struct list_head *entry)
{
    if (entry->prev == NULL || entry->next == NULL)
        return;

    list_del__(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

uint8_t
list_empty(const struct list_head *head)
{
    return head->next == head;
}

void
list_splice(const struct list_head *list, struct list_head *head)
{
    if (!list_empty(list))
        list_splice__(list, head);
}

void
list_replace(const struct list_head *old_entry, struct list_head *new_entry)
{
    new_entry->next       = old_entry->next;
    new_entry->next->prev = new_entry;
    new_entry->prev       = old_entry->prev;
    new_entry->prev->next = new_entry;
}

void
list_replace_init(struct list_head *old_entry, struct list_head *new_entry)
{
    list_replace(old_entry, new_entry);
    list_init(old_entry);
}

void
list_move(struct list_head *entry, struct list_head *head)
{
    list_del__(entry->prev, entry->next);
    list_add(entry, head);
}

void
list_move_tail(struct list_head *entry, struct list_head *head)
{
    list_del__(entry->prev, entry->next);
    list_add_tail(entry, head);
}
