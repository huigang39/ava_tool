#ifndef MPSC_H
#define MPSC_H

#include <stdint.h>

#include "macrodef.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

/* 侵入式链表节点 */
struct mpsc_node;
typedef struct mpsc_node {
        ATOMIC(struct mpsc_node *) next;
} mpsc_node_t;

/* 全局无锁队列 */
typedef struct mpsc {
        ATOMIC(mpsc_node_t *) head; // 生产者：原子交换头指针
        mpsc_node_t *tail;          // 消费者：单线程读取尾指针
        mpsc_node_t  stub;          // 哨兵节点
} mpsc_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化无锁队列
 */
void mpsc_init(mpsc_t *mpsc);

/**
 * @brief 生产者：将节点推入队列 (绝对 Wait-Free, 无锁)
 */
void mpsc_push(mpsc_t *mpsc, mpsc_node_t *node);

/**
 * @brief 消费者：从队列中弹出一个节点
 * @return mpsc_node_t* 成功返回节点指针，队列为空或生产者正在写入时返回 NULL
 */
mpsc_node_t *mpsc_pop(mpsc_t *mpsc);

#ifdef __cplusplus
}
#endif

#endif // !MPSC_H
