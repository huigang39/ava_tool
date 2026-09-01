#include "mempool.h"

#include "bitops.h"
#include "errdef.h"

/**
 * @brief             检查普通池内存块的边界和大小
 * @param[in] mempool 内存池对象
 * @param[in] offset  待检查块相对池起点的偏移
 * @return            位于有效块起点返回 true, 否则返回 false
 * @note              调用时须持有普通池锁
 */
static bool is_mempool_block_valid(const struct mempool *mempool,
                                   size_t offset);

/**
 * @brief            检查两个内存范围是否重叠
 * @param[in] a      第一个比较对象
 * @param[in] a_size 第一个内存范围大小, 单位: byte
 * @param[in] b      第二个比较对象
 * @param[in] b_size 第二个内存范围大小, 单位: byte
 */
static uint8_t is_mempool_range_overlapping(const void *a,
                                            const size_t a_size,
                                            const void *b,
                                            const size_t b_size);

/**
 * @brief          生成固定内存池的有效块掩码
 * @param[in] pool 固定内存池对象
 */
static uint32_t mempool_fixed_valid_mask(const struct mempool_fixed_pool *pool);

/**
 * @brief          检查指针是否属于固定内存池
 * @param[in] pool 固定内存池对象
 * @param[in] ptr  待检查的内存地址
 */
static uint8_t is_mempool_fixed_pointer_in_range(
    const struct mempool_fixed_pool *pool, const void *ptr);

/**
 * @brief              从固定内存池申请空闲块
 * @param[in,out] pool 固定内存池对象
 */
static void *mempool_fixed_alloc(struct mempool_fixed_pool *pool);

/**
 * @brief              释放固定内存池中的块
 * @param[in,out] pool 固定内存池对象
 * @param[in,out] ptr  待检查的内存地址
 */
static int mempool_fixed_free(struct mempool_fixed_pool *pool, void *ptr);

/**
 * @brief                 从适配的固定内存池分配块
 * @param[in,out] mempool 内存池对象
 * @param[in]     cap     所需容量, 单位: byte
 */
static void *mempool_alloc_from_fixed_pools(struct mempool *mempool,
                                            const size_t cap);

/**
 * @brief                查找可满足大小要求的最小空闲块
 * @param[in] mempool    内存池对象
 * @param[in] block_size 固定块大小, 单位: byte
 */
static struct mempool_block *mempool_find_best_fit(
    const struct mempool *mempool, const size_t block_size);

/**
 * @brief               按地址顺序插入空闲块
 * @param[in]     mp    内存池对象
 * @param[in,out] block 内存块
 */
static void mempool_insert_free_block_ordered(const struct mempool *mp,
                                              struct mempool_block *block);

/**
 * @brief               合并地址连续的空闲块
 * @param[in]     mp    内存池对象
 * @param[in,out] block 内存块
 * @note                block 已按地址顺序插入空闲链表
 */
static void mempool_merge_adjacent_blocks(const struct mempool *mp,
                                          struct mempool_block *block);

int mempool_init(struct mempool *mempool)
{
    if (!mempool) {
        return MERR(MENULLPTR);
    }
    if ((mempool->buf == NULL) != (mempool->cap == 0U)) {
        return MERR(MEINVAL);
    }
    if (((uintptr_t)mempool->buf & (MEMPOOL_ALIGN - 1U)) != 0U) {
        return MERR(MEINVAL);
    }
    if (mempool->cap > UINTPTR_MAX - (uintptr_t)mempool->buf) {
        return MERR(MEOVERFLOW);
    }
    list_init(&mempool->block_root);
    mempool->offset = 0;
    mempool->nfixed_pools = 0;
    memset(mempool->fixed_pools, 0, sizeof(mempool->fixed_pools));
    ATOMIC_STORE(&mempool->lock, 0);
    return MOK;
}

int mempool_add_fixed_pool(struct mempool *mempool,
                           void *buf,
                           const size_t block_size,
                           const uint32_t block_cnt)
{
    if (!mempool || !buf) {
        return MERR(MENULLPTR);
    }
    if (!block_size || !block_cnt || ((uintptr_t)buf & (MEMPOOL_ALIGN - 1U))) {
        return MERR(MEINVAL);
    }
    if (block_cnt > MEMPOOL_FIXED_BLOCK_MAX ||
        mempool->nfixed_pools >= MEMPOOL_FIXED_POOL_MAX) {
        return MERR(MEOVERFLOW);
    }

    if (block_size > SIZE_MAX - (MEMPOOL_ALIGN - 1U)) {
        return MERR(MEOVERFLOW);
    }
    const size_t aligned_block_size = MEMPOOL_ALIGN_UP(block_size);
    if (aligned_block_size > SIZE_MAX / block_cnt) {
        return MERR(MEOVERFLOW);
    }
    const size_t pool_size = aligned_block_size * block_cnt;
    if (pool_size > UINTPTR_MAX - (uintptr_t)buf) {
        return MERR(MEOVERFLOW);
    }
    if (mempool->buf && mempool->cap &&
        is_mempool_range_overlapping(
            mempool->buf, mempool->cap, buf, pool_size)) {
        return MERR(MEINVAL);
    }

    for (uint32_t i = 0; i < mempool->nfixed_pools; ++i) {
        struct mempool_fixed_pool *curr = &mempool->fixed_pools[i];
        if (is_mempool_range_overlapping(curr->buf,
                                         curr->block_size * curr->block_cnt,
                                         buf,
                                         pool_size)) {
            return MERR(MEXIST);
        }
    }

    struct mempool_fixed_pool *pool =
        &mempool->fixed_pools[mempool->nfixed_pools++];
    pool->buf = buf;
    pool->block_size = aligned_block_size;
    pool->block_cnt = block_cnt;
    ATOMIC_STORE(&pool->free_bitmap, mempool_fixed_valid_mask(pool));
    return 0;
}

uint32_t mempool_fixed_available(const struct mempool *mempool,
                                 const size_t block_size)
{
    if (!mempool || !block_size ||
        block_size > SIZE_MAX - (MEMPOOL_ALIGN - 1U)) {
        return 0;
    }
    const size_t aligned_size = MEMPOOL_ALIGN_UP(block_size);
    for (uint32_t i = 0; i < mempool->nfixed_pools; ++i) {
        if (mempool->fixed_pools[i].block_size == aligned_size) {
            return popcount32(ATOMIC_LOAD_EXPLICIT(
                &mempool->fixed_pools[i].free_bitmap, ATOMIC_ACQUIRE));
        }
    }
    return 0;
}

void *mempool_alloc_fast(struct mempool *mempool, const size_t cap)
{
    return mempool && cap ? mempool_alloc_from_fixed_pools(mempool, cap) : NULL;
}

void *mempool_alloc(struct mempool *mempool, size_t cap)
{
    if (!mempool || !cap) {
        return NULL;
    }

    void *fast_ptr = mempool_alloc_from_fixed_pools(mempool, cap);
    if (fast_ptr) {
        return fast_ptr;
    }

    if (cap > SIZE_MAX - MEMPOOL_BLOCK_HEADER_SIZE - (MEMPOOL_ALIGN - 1U)) {
        return NULL;
    }
    const size_t block_size = MEMPOOL_BLOCK_HEADER_SIZE + MEMPOOL_ALIGN_UP(cap);

    SPIN_LOCK(&mempool->lock);

    struct mempool_block *block = mempool_find_best_fit(mempool, block_size);

    if (block) {
        list_del(&block->block_node);

        if (block->size - block_size >=
            MEMPOOL_BLOCK_HEADER_SIZE + MEMPOOL_ALIGN) {
            struct mempool_block *split_block =
                (struct mempool_block *)((uint8_t *)block + block_size);
            split_block->size = block->size - block_size;
            block->size = block_size;

            mempool_insert_free_block_ordered(mempool, split_block);
        }

        SPIN_UNLOCK(&mempool->lock);
        return (uint8_t *)block + MEMPOOL_BLOCK_HEADER_SIZE;
    }

    if (mempool->offset > mempool->cap ||
        block_size > mempool->cap - mempool->offset) {
        SPIN_UNLOCK(&mempool->lock);
        return NULL;
    }

    uint8_t *buf_ptr = (uint8_t *)mempool->buf;
    struct mempool_block *fresh_block =
        (struct mempool_block *)(buf_ptr + mempool->offset);
    fresh_block->size = block_size;
    mempool->offset += block_size;

    SPIN_UNLOCK(&mempool->lock);
    return (uint8_t *)fresh_block + MEMPOOL_BLOCK_HEADER_SIZE;
}

void *mempool_calloc(struct mempool *mempool, const size_t cap)
{
    void *ptr = mempool_alloc(mempool, cap);
    if (!ptr) {
        return NULL;
    }

    memset(ptr, 0, cap);
    return ptr;
}

void *mempool_calloc_fast(struct mempool *mempool, const size_t cap)
{
    void *ptr = mempool_alloc_fast(mempool, cap);
    if (ptr) {
        memset(ptr, 0, cap);
    }
    return ptr;
}

int mempool_free(struct mempool *mempool, void *ptr)
{
    if (!mempool || !ptr) {
        return MERR(MENULLPTR);
    }

    for (uint32_t i = 0; i < mempool->nfixed_pools; ++i) {
        if (is_mempool_fixed_pointer_in_range(&mempool->fixed_pools[i], ptr)) {
            return mempool_fixed_free(&mempool->fixed_pools[i], ptr);
        }
    }

    const uintptr_t begin = (uintptr_t)mempool->buf;
    const uintptr_t addr = (uintptr_t)ptr;
    if (!mempool->buf || addr < begin ||
        addr - begin < MEMPOOL_BLOCK_HEADER_SIZE ||
        addr - begin >= mempool->cap) {
        return MERR(MEINVAL);
    }
    const size_t offset = (size_t)(addr - begin) - MEMPOOL_BLOCK_HEADER_SIZE;
    if (offset % MEMPOOL_ALIGN != 0U) {
        return MERR(MEINVAL);
    }

    SPIN_LOCK(&mempool->lock);

    struct list_head *node;
    LIST_FOR_EACH(node, &mempool->block_root)
    {
        const struct mempool_block *free_block =
            CONTAINER_OF(node, struct mempool_block, block_node);
        const size_t free_offset = (size_t)((uintptr_t)free_block - begin);
        if (offset >= free_offset && offset - free_offset < free_block->size) {
            SPIN_UNLOCK(&mempool->lock);
            return MERR(MEXIST);
        }
    }
    if (!is_mempool_block_valid(mempool, offset)) {
        SPIN_UNLOCK(&mempool->lock);
        return MERR(MEINVAL);
    }
    struct mempool_block *block =
        (struct mempool_block *)((uint8_t *)mempool->buf + offset);
    mempool_insert_free_block_ordered(mempool, block);

    mempool_merge_adjacent_blocks(mempool, block);

    SPIN_UNLOCK(&mempool->lock);
    return MOK;
}

int mempool_reset(struct mempool *mempool)
{
    if (!mempool) {
        return MERR(MENULLPTR);
    }
    SPIN_LOCK(&mempool->lock);
    mempool->offset = 0;
    list_init(&mempool->block_root);
    for (uint32_t i = 0; i < mempool->nfixed_pools; ++i) {
        ATOMIC_STORE_EXPLICIT(
            &mempool->fixed_pools[i].free_bitmap,
            mempool_fixed_valid_mask(&mempool->fixed_pools[i]),
            ATOMIC_RELEASE);
    }
    SPIN_UNLOCK(&mempool->lock);
    return MOK;
}

static bool is_mempool_block_valid(const struct mempool *mempool,
                                   const size_t offset)
{
    if (mempool->offset > mempool->cap || offset >= mempool->offset) {
        return false;
    }
    size_t curr = 0U;
    while (curr <= offset) {
        const size_t remaining = mempool->offset - curr;
        if (remaining < MEMPOOL_BLOCK_HEADER_SIZE) {
            return false;
        }
        const struct mempool_block *block =
            (const struct mempool_block *)((const uint8_t *)mempool->buf +
                                           curr);
        if (block->size < MEMPOOL_BLOCK_HEADER_SIZE + MEMPOOL_ALIGN ||
            block->size > remaining || block->size % MEMPOOL_ALIGN != 0U) {
            return false;
        }
        if (curr == offset) {
            return true;
        }
        curr += block->size;
    }
    return false;
}

static uint8_t is_mempool_range_overlapping(const void *a,
                                            const size_t a_size,
                                            const void *b,
                                            const size_t b_size)
{
    const uintptr_t a_begin = (uintptr_t)a;
    const uintptr_t b_begin = (uintptr_t)b;
    return a_begin < b_begin + b_size && b_begin < a_begin + a_size;
}

static uint32_t mempool_fixed_valid_mask(const struct mempool_fixed_pool *pool)
{
    return pool->block_cnt == MEMPOOL_FIXED_BLOCK_MAX
               ? UINT32_MAX
               : (((uint32_t)1U << pool->block_cnt) - 1U);
}

static uint8_t is_mempool_fixed_pointer_in_range(
    const struct mempool_fixed_pool *pool, const void *ptr)
{
    if (!pool || !pool->buf || !ptr || !pool->block_size || !pool->block_cnt) {
        return false;
    }
    const uintptr_t begin = (uintptr_t)pool->buf;
    const uintptr_t end = begin + pool->block_size * pool->block_cnt;
    const uintptr_t val = (uintptr_t)ptr;
    return val >= begin && val < end;
}

static void *mempool_fixed_alloc(struct mempool_fixed_pool *pool)
{
    uint32_t bitmap = ATOMIC_LOAD_EXPLICIT(&pool->free_bitmap, ATOMIC_ACQUIRE);
    while (bitmap) {
        const uint32_t idx = ctz32(bitmap);
        const uint32_t desired = bitmap & ~((uint32_t)1U << idx);
        if (ATOMIC_CAS_WEAK_EXPLICIT(&pool->free_bitmap,
                                     &bitmap,
                                     desired,
                                     ATOMIC_ACQ_REL,
                                     ATOMIC_ACQUIRE)) {
            return (uint8_t *)pool->buf + (size_t)idx * pool->block_size;
        }
    }
    return NULL;
}

static int mempool_fixed_free(struct mempool_fixed_pool *pool, void *ptr)
{
    if (!is_mempool_fixed_pointer_in_range(pool, ptr)) {
        return MERR(MEINVAL);
    }
    const uintptr_t offset = (uintptr_t)ptr - (uintptr_t)pool->buf;
    if (offset % pool->block_size != 0) {
        return MERR(MEINVAL);
    }
    const uint32_t idx = (uint32_t)(offset / pool->block_size);
    const uint32_t bit = (uint32_t)1U << idx;
    uint32_t bitmap = ATOMIC_LOAD_EXPLICIT(&pool->free_bitmap, ATOMIC_ACQUIRE);
    for (;;) {
        if (bitmap & bit) {
            return MERR(MEXIST);
        }
        if (ATOMIC_CAS_WEAK_EXPLICIT(&pool->free_bitmap,
                                     &bitmap,
                                     bitmap | bit,
                                     ATOMIC_ACQ_REL,
                                     ATOMIC_ACQUIRE)) {
            return 0;
        }
    }
}

static void *mempool_alloc_from_fixed_pools(struct mempool *mempool,
                                            const size_t cap)
{
    uint8_t attempted[MEMPOOL_FIXED_POOL_MAX] = {0};
    for (uint32_t pass = 0; pass < mempool->nfixed_pools; ++pass) {
        struct mempool_fixed_pool *best = NULL;
        size_t best_size = (size_t)-1;
        uint32_t best_idx = 0;
        for (uint32_t i = 0; i < mempool->nfixed_pools; ++i) {
            struct mempool_fixed_pool *pool = &mempool->fixed_pools[i];
            if (!attempted[i] && pool->block_size >= cap &&
                pool->block_size < best_size) {
                best = pool;
                best_size = pool->block_size;
                best_idx = i;
            }
        }
        if (!best) {
            return NULL;
        }
        attempted[best_idx] = true;
        void *ptr = mempool_fixed_alloc(best);
        if (ptr) {
            return ptr;
        }
    }
    return NULL;
}

static struct mempool_block *mempool_find_best_fit(
    const struct mempool *mempool, const size_t block_size)
{
    struct mempool_block *best_block = NULL;
    size_t best_size = mempool->cap + 1;

    struct list_head *node;
    LIST_FOR_EACH(node, &mempool->block_root)
    {
        struct mempool_block *block =
            CONTAINER_OF(node, struct mempool_block, block_node);
        if (block->size >= block_size && block->size < best_size) {
            best_block = block;
            best_size = block->size;
            if (block->size == block_size) {
                break;
            }
        }
    }

    return best_block;
}

static void mempool_insert_free_block_ordered(const struct mempool *mp,
                                              struct mempool_block *block)
{
    struct list_head *pos = mp->block_root.next;
    while (pos != &mp->block_root &&
           (uint8_t *)CONTAINER_OF(pos, struct mempool_block, block_node) <
               (uint8_t *)block) {
        pos = pos->next;
    }

    list_add_tail(&block->block_node, pos);
}

static void mempool_merge_adjacent_blocks(const struct mempool *mp,
                                          struct mempool_block *block)
{
    struct list_head *prev_node = block->block_node.prev;
    if (prev_node != &mp->block_root) {
        struct mempool_block *prev_block =
            CONTAINER_OF(prev_node, struct mempool_block, block_node);
        if ((uint8_t *)prev_block + prev_block->size == (uint8_t *)block) {
            list_del(&block->block_node);
            prev_block->size += block->size;
            block = prev_block;
        }
    }

    struct list_head *next_node = block->block_node.next;
    if (next_node != &mp->block_root) {
        struct mempool_block *next_block =
            CONTAINER_OF(next_node, struct mempool_block, block_node);
        if ((uint8_t *)block + block->size == (uint8_t *)next_block) {
            list_del(next_node);
            block->size += next_block->size;
        }
    }
}
