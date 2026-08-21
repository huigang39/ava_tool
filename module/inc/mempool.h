#ifndef MEMPOOL_H
#define MEMPOOL_H

#include "list.h"
#include "macrodef.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

#define MEMPOOL_ALIGN                        sizeof(size_t)
#define MEMPOOL_ALIGN_UP(size)               (((size) + (MEMPOOL_ALIGN - 1)) & ~(MEMPOOL_ALIGN - 1))
#define MEMPOOL_BLOCK_HEADER_SIZE            MEMPOOL_ALIGN_UP(sizeof(struct mempool_blk))
#define MEMPOOL_FIXED_POOL_MAX               (8)
#define MEMPOOL_FIXED_BLOCK_MAX              (32)
#define MEMPOOL_FIXED_POOL_SIZE(size, count) (MEMPOOL_ALIGN_UP(size) * (count))

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

struct mempool_blk {
    struct list_head blk_node; // 链表节点
    size_t           size;     // 块大小(包含头部)
};

struct mempool_fixed_pool {
    void    *buf;
    size_t   block_size;
    uint32_t block_count;
    ATOMIC(uint32_t) free_bitmap;
};

struct mempool {
    struct list_head          blk_root; // 空闲块链表头
    void                     *buf;      // 内存池缓冲区
    size_t                    cap;      // 缓冲区大小
    size_t                    offset;   // 当前已分配偏移
    struct mempool_fixed_pool fixed_pools[MEMPOOL_FIXED_POOL_MAX];
    uint32_t                  nfixed_pools;
    ATOMIC(uint8_t) lock;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 内存池结构体初始化
 *
 * @param mempool 内存池结构体
 */
void mempool_init(struct mempool *mempool);

/**
 * @brief 从内存池分配 cap 字节
 *
 * @param mempool 内存池结构体
 * @param cap     内存池容量
 * @return        void* 指向分配内存的指针,失败返回 NULL
 */
void *mempool_alloc(struct mempool *mempool, size_t cap);

/**
 * @brief 添加一个使用独立缓冲区的固定尺寸快速池
 * @note 缓冲区不得与
 * mempool->buf 或其他固定池重叠, block_count 最大为 32.
 */
int
mempool_add_fixed_pool(struct mempool *mempool, void *buf, size_t block_size, uint32_t block_count);

/** @brief 返回指定块大小固定池的可用块数, 不存在时返回 0. */
uint32_t mempool_fixed_available(const struct mempool *mempool, size_t block_size);

/**
 * @brief 只从固定尺寸对象池分配, 所有适配池耗尽后立即返回 NULL
 */
void *mempool_alloc_fast(struct mempool *mempool, size_t cap);

/**
 * @brief 从内存池分配 cap 字节并初始化为 0
 *
 * @param mempool 内存池结构体
 * @param cap     内存池容量
 * @return        void* 指向分配内存的指针,失败返回 NULL
 */
void *mempool_calloc(struct mempool *mempool, size_t cap);
void *mempool_calloc_fast(struct mempool *mempool, size_t cap);

/**
 * @brief 释放内存池中的内存块
 *
 * @param mempool 内存池结构体
 * @param ptr     需要释放的内存块指针
 * @return        void
 */
void mempool_free(struct mempool *mempool, void *ptr);

/**
 * @brief 重置内存池, 清空所有已分配的内存
 *
 * @param mempool 内存池结构体
 * @return        void
 */
void mempool_reset(struct mempool *mempool);

#ifdef __cplusplus
}
#endif

#endif // !MEMPOOL_H
