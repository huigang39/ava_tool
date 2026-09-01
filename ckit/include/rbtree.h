/**
 * @brief Linux 风格红黑树实现
 * 红黑树是一种自平衡二叉搜索树,满足:
 *   1 每个节点是红色或黑色;
 *   2 根节点是黑色;
 *   3 所有叶子节点 (NULL) 视为黑色;
 *   4 红色节点的子节点必须是黑色;
 *   5 任意节点到其所有叶子的路径中, 黑色节点数相等
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

// 空树定义
#define RB_ROOT ((struct rb_root){.rb_node = NULL})

enum rb_color {
    RB_RED = 0,   // 红色节点
    RB_BLACK = 1, // 黑色节点
};

// 红黑树节点结构
struct rb_node {
    size_t rb_parent_color;   // 低 2 位存颜色, 高位存父节点指针
    struct rb_node *rb_right; // 右子节点
    struct rb_node *rb_left;  // 左子节点
};

// 红黑树根节点
struct rb_root {
    struct rb_node *rb_node; // 红黑树根节点
};

/**
 * @brief        获取父节点
 * @param[in] rb 树节点
 * @return       父节点地址
 */
struct rb_node *rb_get_parent(const struct rb_node *rb);

/**
 * @brief                设置父节点
 * @param[in,out] rb     树节点
 * @param[in,out] parent 父节点
 */
void rb_set_parent(struct rb_node *rb, struct rb_node *parent);

/**
 * @brief        获取节点颜色
 * @param[in] rb 树节点
 * @return       节点颜色
 */
enum rb_color rb_get_color(const struct rb_node *rb);

/**
 * @brief               设置节点颜色
 * @param[in,out] rb    树节点
 * @param[in]     color 节点颜色或终端颜色
 */
void rb_set_color(struct rb_node *rb, enum rb_color color);

/**
 * @brief        判断节点是否为红色
 * @param[in] rb 树节点
 * @return       节点查询结果
 */
uint8_t is_rb_node_red(const struct rb_node *rb);

/**
 * @brief        判断节点是否为黑色
 * @param[in] rb 树节点
 * @return       节点查询结果
 */
uint8_t is_rb_node_black(const struct rb_node *rb);

/**
 * @brief            将节点标记为红色
 * @param[in,out] rb 树节点
 */
void rb_set_red(struct rb_node *rb);

/**
 * @brief            将节点标记为黑色
 * @param[in,out] rb 树节点
 */
void rb_set_black(struct rb_node *rb);

/**
 * @brief        判断节点是否未链接
 * @param[in] rb 树节点
 * @return       节点查询结果
 */
uint8_t is_rb_node_unlinked(const struct rb_node *rb);

/**
 * @brief                 将节点链接到指定父节点
 * @param[in,out] node    待操作树节点
 * @param[in,out] parent  父节点
 * @param[in,out] rb_link 父节点的子节点链接地址
 */
void rb_link_node(struct rb_node *node,
                  struct rb_node *parent,
                  struct rb_node **rb_link);

/**
 * @brief              对指定节点执行左旋
 * @param[in,out] node 待操作树节点
 * @param[in,out] root 红黑树根
 */
void rb_rotate_left(struct rb_node *node, struct rb_root *root);

/**
 * @brief              对指定节点执行右旋
 * @param[in,out] node 待操作树节点
 * @param[in,out] root 红黑树根
 */
void rb_rotate_right(struct rb_node *node, struct rb_root *root);

/**
 * @brief              插入后恢复红黑树平衡
 * @param[in,out] node 待操作树节点
 * @param[in,out] root 红黑树根
 */
void rb_insert_color(struct rb_node *node, struct rb_root *root);

/**
 * @brief                删除后恢复红黑树平衡
 * @param[in,out] node   待操作树节点
 * @param[in,out] parent 父节点
 * @param[in,out] root   红黑树根
 */
void rb_erase_color(struct rb_node *node,
                    struct rb_node *parent,
                    struct rb_root *root);

/**
 * @brief              删除节点并维护红黑树
 * @param[in,out] node 待操作树节点
 * @param[in,out] root 红黑树根
 */
void rb_erase(struct rb_node *node, struct rb_root *root);

/**
 * @brief          获取最小节点
 * @param[in] root 红黑树根
 * @return         最小节点, 空树返回 NULL
 */
struct rb_node *rb_first(const struct rb_root *root);

/**
 * @brief          获取最大节点
 * @param[in] root 红黑树根
 * @return         最大节点, 空树返回 NULL
 */
struct rb_node *rb_last(const struct rb_root *root);

/**
 * @brief          获取后继节点
 * @param[in] node 待操作树节点
 * @return         后继节点, 不存在时返回 NULL
 */
struct rb_node *rb_next(const struct rb_node *node);

/**
 * @brief          获取前驱节点
 * @param[in] node 待操作树节点
 * @return         前驱节点, 不存在时返回 NULL
 */
struct rb_node *rb_prev(const struct rb_node *node);

#ifdef __cplusplus
}
#endif

#endif
