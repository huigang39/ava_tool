#ifndef MPSC_H
#define MPSC_H

#include <stdint.h>

#include "macrodef.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

/* 侵入式链表节点 */
struct mpsc_node;
struct mpsc_node {
    ATOMIC(struct mpsc_node *) next;
};

/* 全局无锁队列 */
struct mpsc {
    ATOMIC(struct mpsc_node *) head; // 生产者:原子交换头指针
    struct mpsc_node *tail;          // 消费者:单线程读取尾指针
    struct mpsc_node  stub;          // 哨兵节点
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化无锁队列
 */
void mpsc_init(struct mpsc *mpsc);

/**
 * @brief 生产者:将节点推入队列 (绝对 Wait-Free, 无锁)
 */
void mpsc_push(struct mpsc *mpsc, struct mpsc_node *node);

/**
 * @brief 消费者:从队列中弹出一个节点
 * @return struct mpsc_node* 成功返回节点指针,队列为空或生产者正在写入时返回 NULL
 */
struct mpsc_node *mpsc_pop(struct mpsc *mpsc);

#ifdef __cplusplus
}
#endif

#endif // !MPSC_H
