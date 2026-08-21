/**
 * @file rbtree.h
 * @brief Linux 风格红黑树实现
 *
 * 红黑树是一种自平衡二叉搜索树,满足:
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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

/* 空树定义 */
#define RB_ROOT (struct rb_root) NULL

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

enum rb_color {
    RB_RED   = 0,
    RB_BLACK = 1,
};

/* 红黑树节点结构 */
struct rb_node {
    size_t          rb_parent_color; // 低 2 位存颜色, 高位存父节点指针
    struct rb_node *rb_right;        // 右子节点
    struct rb_node *rb_left;         // 左子节点
};

/* 红黑树根节点 */
struct rb_root {
    struct rb_node *rb_node;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

struct rb_node *rb_get_parent(const struct rb_node *rb);
void            rb_set_parent(struct rb_node *rb, struct rb_node *parent);

enum rb_color rb_get_color(const struct rb_node *rb);
void          rb_set_color(struct rb_node *rb, enum rb_color color);

uint8_t rb_is_red(const struct rb_node *rb);
uint8_t rb_is_black(const struct rb_node *rb);

void rb_set_red(struct rb_node *rb);
void rb_set_black(struct rb_node *rb);

uint8_t rb_is_unlinked(const struct rb_node *rb);
void    rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **rb_link);

void rb_rotate_left(struct rb_node *node, struct rb_root *root);
void rb_rotate_right(struct rb_node *node, struct rb_root *root);

void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase_color(struct rb_node *node, struct rb_node *parent, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);

struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);

#ifdef __cplusplus
}
#endif

#endif // !RBTREE_H
