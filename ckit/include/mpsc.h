#ifndef MPSC_H
#define MPSC_H

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

// 侵入式链表节点
struct mpsc_node;

struct mpsc_node {
    ATOMIC(struct mpsc_node *) next;
};

// 多生产者单消费者无锁队列
struct mpsc {
    // 生产者共享状态
    ATOMIC(struct mpsc_node *) head; // 生产者: 原子交换头指针

    // 消费者私有状态
    struct mpsc_node *tail; // 消费者: 单线程读取尾指针

    // 队列哨兵
    struct mpsc_node stub; // 哨兵节点
};

/**
 * @brief           初始化多生产者单消费者队列
 * @param[out] mpsc 有效的队列对象
 */
void mpsc_init(struct mpsc *mpsc);

/**
 * @brief              向队列追加节点
 * @param[in,out] mpsc 已初始化的队列
 * @param[in,out] node 待入队节点, 出队前必须保持有效
 */
void mpsc_push(struct mpsc *mpsc, struct mpsc_node *node);

/**
 * @brief              由单一消费者取出队头节点
 * @param[in,out] mpsc 已初始化的队列
 * @return             节点指针, 空队列或生产者尚未完成链接时返回 NULL
 */
struct mpsc_node *mpsc_pop(struct mpsc *mpsc);

#ifdef __cplusplus
}
#endif

#endif
