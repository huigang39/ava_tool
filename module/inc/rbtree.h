/**
 * @file rbtree.h
 * @brief Linux 风格红黑树实现
 *
 * 红黑树是一种自平衡二叉搜索树，满足:
 *   1. 每个节点是红色或黑色;
 *   2. 根节点是黑色;
 *   3. 所有叶子节点 (NULL) 视为黑色;
 *   4. 红色节点的子节点必须是黑色;
 *   5. 任意节点到其所有叶子的路径中, 黑色节点数相等.
 *
 * 本实现采用 Linux 内核风格:
 *   - 用最低 2 bit 存颜色 (节省结构体空间);
 *   - 提供插入/删除平衡逻辑;
 *   - 用户负责维护键值比较;
 */

#ifndef RBTREE_H
#define RBTREE_H

#include "macrodef.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

/* 空树定义 */
#define RB_ROOT (rb_root_t) NULL

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef enum rb_color {
        RB_RED   = 0,
        RB_BLACK = 1,
} rb_color_e;

/* 红黑树节点结构 */
typedef struct rb_node {
        usize           rb_parent_color; // 低 2 位存颜色, 高位存父节点指针
        struct rb_node *rb_right;        // 右子节点
        struct rb_node *rb_left;         // 左子节点
} rb_node_t;

/* 红黑树根节点 */
typedef struct rb_root {
        rb_node_t *rb_node;
} rb_root_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

rb_node_t *rb_get_parent(const rb_node_t *rb);
void       rb_set_parent(rb_node_t *rb, rb_node_t *parent);

rb_color_e rb_get_color(const rb_node_t *rb);
void       rb_set_color(rb_node_t *rb, rb_color_e color);

u8 rb_is_red(const rb_node_t *rb);
u8 rb_is_black(const rb_node_t *rb);

void rb_set_red(rb_node_t *rb);
void rb_set_black(rb_node_t *rb);

u8   rb_is_unlinked(const rb_node_t *rb);
void rb_link_node(rb_node_t *node, rb_node_t *parent, rb_node_t **rb_link);

void rb_rotate_left(rb_node_t *node, rb_root_t *root);
void rb_rotate_right(rb_node_t *node, rb_root_t *root);

void rb_insert_color(rb_node_t *node, rb_root_t *root);
void rb_erase_color(rb_node_t *node, rb_node_t *parent, rb_root_t *root);
void rb_erase(rb_node_t *node, rb_root_t *root);

rb_node_t *rb_first(const rb_root_t *root);
rb_node_t *rb_last(const rb_root_t *root);
rb_node_t *rb_next(const rb_node_t *node);
rb_node_t *rb_prev(const rb_node_t *node);

#ifdef __cplusplus
}
#endif

#endif // !RBTREE_H
