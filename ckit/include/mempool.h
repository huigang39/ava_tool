#ifndef MEMPOOL_H
#define MEMPOOL_H

#include "list.h"
#include "macrodef.h"
#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdalign.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 默认使用平台基础类型的最大对齐要求, 可由项目覆盖
#ifndef MEMPOOL_ALIGN
#if defined(_MSC_VER) && !defined(__cplusplus)
// MSVC 的 C 标准库未提供 max_align_t, 用基础类型取得所需对齐
union mempool_max_alignment {
    long double floating;
    long long integer;
    void *pointer;
};

#define MEMPOOL_ALIGN (alignof(union mempool_max_alignment))
#else
#define MEMPOOL_ALIGN (alignof(max_align_t))
#endif
#endif
#define MEMPOOL_ALIGN_UP(size) ALIGN_UP((size), (size_t)MEMPOOL_ALIGN)
#define MEMPOOL_BLOCK_HEADER_SIZE MEMPOOL_ALIGN_UP(sizeof(struct mempool_block))
#define MEMPOOL_FIXED_POOL_MAX (8)
#define MEMPOOL_FIXED_BLOCK_MAX (32)
#define MEMPOOL_FIXED_POOL_SIZE(size, cnt) (MEMPOOL_ALIGN_UP(size) * (cnt))

struct mempool_block {
    struct list_head block_node; // 链表节点
    size_t size;                 // 块大小(包含头部)
};

struct mempool_fixed_pool {
    // 固定块存储配置
    void *buf;          // 算法或通信存储缓冲区
    size_t block_size;  // 固定块大小, 单位: byte
    uint32_t block_cnt; // 固定内存池块数量

    // 空闲块状态
    ATOMIC(uint32_t) free_bitmap; // 空闲块位图
};

struct mempool {
    // 普通内存池资源
    void *buf;  // 内存池缓冲区
    size_t cap; // 缓冲区大小

    // 使用状态与并发控制
    size_t offset;               // 当前已分配偏移
    uint32_t nfixed_pools;       // 已注册的固定内存池数量
    ATOMIC(uint8_t) lock;        // 普通池链表和分配位置的锁
    struct list_head block_root; // 空闲块链表头

    // 固定块内存池
    struct mempool_fixed_pool
        fixed_pools[MEMPOOL_FIXED_POOL_MAX]; // 固定块内存池集合
};

/**
 * @brief                 内存池结构体初始化
 * @param[in,out] mempool 内存池结构体
 * @return                成功返回 0, 失败返回负错误码
 */
int mempool_init(struct mempool *mempool);

/**
 * @brief                 从内存池分配 cap 字节
 * @param[in,out] mempool 内存池结构体
 * @param[in]     cap     内存池容量
 * @return                分配成功返回内存地址, 失败返回 NULL
 */
void *mempool_alloc(struct mempool *mempool, size_t cap);

/**
 * @brief                    添加一个使用独立缓冲区的固定尺寸快速池
 * @note                     初始化阶段注册, 缓冲区不得与普通池或其他固定池重叠
 * @param[in,out] mempool    内存池对象
 * @param[in,out] buf        数据缓冲区
 * @param[in]     block_size 固定块大小, 单位: byte
 * @param[in]     block_cnt  固定内存池块数量
 * @return                   成功返回 0, 参数无效或容量不足返回负错误码
 */
int mempool_add_fixed_pool(struct mempool *mempool,
                           void *buf,
                           size_t block_size,
                           uint32_t block_cnt);

/**
 * @brief                获取固定内存池可用块数量
 * @note                 返回指定块大小固定池的可用块数, 不存在时返回 0
 * @param[in] mempool    内存池对象
 * @param[in] block_size 固定块大小, 单位: byte
 * @return               指定大小固定池的可用块数
 */
uint32_t mempool_fixed_available(const struct mempool *mempool,
                                 size_t block_size);

/**
 * @brief                 只从固定尺寸对象池分配, 所有适配池耗尽后立即返回 NULL
 * @param[in,out] mempool 内存池对象
 * @param[in]     cap     分配容量, 单位: byte
 * @return                分配成功返回内存地址, 固定池不可用或耗尽返回 NULL
 */
void *mempool_alloc_fast(struct mempool *mempool, size_t cap);

/**
 * @brief                 从内存池分配 cap 字节并初始化为 0
 * @param[in,out] mempool 内存池结构体
 * @param[in]     cap     内存池容量
 * @return                分配成功返回内存地址, 失败返回 NULL
 */
void *mempool_calloc(struct mempool *mempool, size_t cap);

/**
 * @brief                 分配并清零内存
 * @param[in,out] mempool 内存池对象
 * @param[in]     cap     分配容量, 单位: byte
 * @return                已清零的内存地址, 固定池不可用或耗尽返回 NULL
 */
void *mempool_calloc_fast(struct mempool *mempool, size_t cap);

/**
 * @brief                 释放内存池中的内存块
 * @param[in,out] mempool 内存池结构体
 * @param[in]     ptr     待释放内存地址
 * @return                成功返回 0, 参数无效或重复释放返回负错误码
 */
int mempool_free(struct mempool *mempool, void *ptr);

/**
 * @brief                 重置内存池, 清空所有已分配的内存
 * @param[in,out] mempool 内存池结构体
 * @return                成功返回 0, 参数无效返回负错误码
 * @note                  停止分配和释放后调用, 原有指针全部失效
 */
int mempool_reset(struct mempool *mempool);

#ifdef __cplusplus
}
#endif

#endif
