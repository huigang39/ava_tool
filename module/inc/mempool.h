#ifndef MEMPOOL_H
#define MEMPOOL_H

#include "list.h"
#include "macrodef.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

#define MEMPOOL_ALIGN             sizeof(usize)
#define MEMPOOL_ALIGN_UP(size)    (((size) + (MEMPOOL_ALIGN - 1)) & ~(MEMPOOL_ALIGN - 1))
#define MEMPOOL_BLOCK_HEADER_SIZE MEMPOOL_ALIGN_UP(sizeof(mempool_blk_t))

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct mempool_blk {
        list_head_t blk_node; // 链表节点
        usize       size;     // 块大小(包含头部)
} mempool_blk_t;

typedef struct mempool {
        list_head_t blk_root; // 空闲块链表头
        void       *buf;      // 内存池缓冲区
        usize       cap;      // 缓冲区大小
        usize       offset;   // 当前已分配偏移
        ATOMIC(u8) lock;
} mempool_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 内存池结构体初始化
 *
 * @param mempool 内存池结构体
 */
void mempool_init(mempool_t *mempool);

/**
 * @brief 从内存池分配 cap 字节
 *
 * @param mempool 内存池结构体
 * @param cap     内存池容量
 * @return        void* 指向分配内存的指针，失败返回 NULL
 */
void *mempool_alloc(mempool_t *mempool, usize cap);

/**
 * @brief 从内存池分配 cap 字节并初始化为 0
 *
 * @param mempool 内存池结构体
 * @param cap     内存池容量
 * @return        void* 指向分配内存的指针，失败返回 NULL
 */
void *mempool_calloc(mempool_t *mempool, usize cap);

/**
 * @brief 释放内存池中的内存块
 *
 * @param mempool 内存池结构体
 * @param ptr     需要释放的内存块指针
 * @return        void
 */
void mempool_free(mempool_t *mempool, void *ptr);

/**
 * @brief 重置内存池, 清空所有已分配的内存
 *
 * @param mempool 内存池结构体
 * @return        void
 */
void mempool_reset(mempool_t *mempool);

#ifdef __cplusplus
}
#endif

#endif // !MEMPOOL_H
