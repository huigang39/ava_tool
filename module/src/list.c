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
list_add__(list_head_t *entry, list_head_t *prev_node, list_head_t *next_node)
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
list_del__(list_head_t *prev_node, list_head_t *next_node)
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
list_splice__(const list_head_t *list, list_head_t *head)
{
        list_head_t *first = list->next;
        list_head_t *last  = list->prev;
        list_head_t *at    = head->next;

        first->prev = head;
        head->next  = first;

        last->next = at;
        at->prev   = last;
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
list_init(list_head_t *head)
{
        head->next = head;
        head->prev = head;
}

void
list_add(list_head_t *entry, list_head_t *head)
{
        list_add__(entry, head, head->next);
}

void
list_add_tail(list_head_t *entry, list_head_t *head)
{
        list_add__(entry, head->prev, head);
}

void
list_del(list_head_t *entry)
{
        if (entry->prev == NULL || entry->next == NULL)
                return;

        list_del__(entry->prev, entry->next);
        entry->next = NULL;
        entry->prev = NULL;
}

u8
list_empty(const list_head_t *head)
{
        return head->next == head;
}

void
list_splice(const list_head_t *list, list_head_t *head)
{
        if (!list_empty(list))
                list_splice__(list, head);
}

void
list_replace(const list_head_t *old_entry, list_head_t *new_entry)
{
        new_entry->next       = old_entry->next;
        new_entry->next->prev = new_entry;
        new_entry->prev       = old_entry->prev;
        new_entry->prev->next = new_entry;
}

void
list_replace_init(list_head_t *old_entry, list_head_t *new_entry)
{
        list_replace(old_entry, new_entry);
        list_init(old_entry);
}

void
list_move(list_head_t *entry, list_head_t *head)
{
        list_del__(entry->prev, entry->next);
        list_add(entry, head);
}

void
list_move_tail(list_head_t *entry, list_head_t *head)
{
        list_del__(entry->prev, entry->next);
        list_add_tail(entry, head);
}
