#include "mempool.h"

#include "bitops.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 检查指针是否在内存池有效范围内
 */
static uint8_t
mempool_is_valid_ptr(const struct mempool *mempool, const void *ptr)
{
    const uint8_t *p       = (const uint8_t *)ptr;
    const uint8_t *buf     = (const uint8_t *)mempool->buf;
    const uint8_t *buf_end = buf + mempool->cap;
    return p >= buf && p < buf_end;
}

static uint8_t
mempool_ranges_overlap(const void *a, const size_t a_size, const void *b, const size_t b_size)
{
    const uintptr_t a_begin = (uintptr_t)a;
    const uintptr_t b_begin = (uintptr_t)b;
    return a_begin < b_begin + b_size && b_begin < a_begin + a_size;
}

static uint32_t
mempool_fixed_valid_mask(const struct mempool_fixed_pool *pool)
{
    return pool->block_count == MEMPOOL_FIXED_BLOCK_MAX
               ? UINT32_MAX
               : (((uint32_t)1U << pool->block_count) - 1U);
}

static uint8_t
mempool_fixed_contains(const struct mempool_fixed_pool *pool, const void *ptr)
{
    if (!pool || !pool->buf || !ptr || !pool->block_size || !pool->block_count)
        return false;
    const uintptr_t begin = (uintptr_t)pool->buf;
    const uintptr_t end   = begin + pool->block_size * pool->block_count;
    const uintptr_t value = (uintptr_t)ptr;
    return value >= begin && value < end;
}

static void *
mempool_fixed_alloc(struct mempool_fixed_pool *pool)
{
    uint32_t bitmap = ATOMIC_LOAD_EXPLICIT(&pool->free_bitmap, ATOMIC_ACQUIRE);
    while (bitmap) {
        const uint32_t index   = ctz32(bitmap);
        const uint32_t desired = bitmap & ~((uint32_t)1U << index);
        if (ATOMIC_CAS_WEAK_EXPLICIT(
                &pool->free_bitmap, &bitmap, desired, ATOMIC_ACQ_REL, ATOMIC_ACQUIRE))
            return (uint8_t *)pool->buf + (size_t)index * pool->block_size;
    }
    return NULL;
}

static int
mempool_fixed_free(struct mempool_fixed_pool *pool, void *ptr)
{
    if (!mempool_fixed_contains(pool, ptr))
        return -MEINVAL;
    const uintptr_t offset = (uintptr_t)ptr - (uintptr_t)pool->buf;
    if (offset % pool->block_size != 0)
        return -MEINVAL;
    const uint32_t index  = (uint32_t)(offset / pool->block_size);
    const uint32_t bit    = (uint32_t)1U << index;
    uint32_t       bitmap = ATOMIC_LOAD_EXPLICIT(&pool->free_bitmap, ATOMIC_ACQUIRE);
    for (;;) {
        if (bitmap & bit)
            return -MEXIST;
        if (ATOMIC_CAS_WEAK_EXPLICIT(
                &pool->free_bitmap, &bitmap, bitmap | bit, ATOMIC_ACQ_REL, ATOMIC_ACQUIRE))
            return 0;
    }
}

static void *
mempool_alloc_from_fixed_pools(struct mempool *mempool, const size_t cap)
{
    uint8_t attempted[MEMPOOL_FIXED_POOL_MAX] = {0};
    for (uint32_t pass = 0; pass < mempool->nfixed_pools; ++pass) {
        struct mempool_fixed_pool *best      = NULL;
        size_t                     best_size = (size_t)-1;
        uint32_t                   best_idx  = 0;
        for (uint32_t i = 0; i < mempool->nfixed_pools; ++i) {
            struct mempool_fixed_pool *pool = &mempool->fixed_pools[i];
            if (!attempted[i] && pool->block_size >= cap && pool->block_size < best_size) {
                best      = pool;
                best_size = pool->block_size;
                best_idx  = i;
            }
        }
        if (!best)
            return NULL;
        attempted[best_idx] = true;
        void *ptr           = mempool_fixed_alloc(best);
        if (ptr)
            return ptr;
    }
    return NULL;
}

/**
 * @brief 在空闲链表中查找最佳匹配块
 */
static struct mempool_blk *
mempool_find_best_fit(const struct mempool *mempool, const size_t block_size)
{
    struct mempool_blk *best_blk  = NULL;
    size_t              best_size = mempool->cap + 1;

    struct list_head *node;
    LIST_FOR_EACH(node, &mempool->blk_root)
    {
        struct mempool_blk *blk = CONTAINER_OF(node, struct mempool_blk, blk_node);
        if (blk->size >= block_size && blk->size < best_size) {
            best_blk  = blk;
            best_size = blk->size;
            if (blk->size == block_size)
                break;
        }
    }

    return best_blk;
}

/**
 * @brief 将空闲块按地址顺序插入链表
 */
static void
mempool_insert_free_block_ordered(const struct mempool *mp, struct mempool_blk *blk)
{
    struct list_head *pos = mp->blk_root.next;
    while (pos != &mp->blk_root &&
           (uint8_t *)CONTAINER_OF(pos, struct mempool_blk, blk_node) < (uint8_t *)blk)
        pos = pos->next;

    list_add_tail(&blk->blk_node, pos);
}

/**
 * @brief 尝试合并相邻的空闲块
 * @note 调用此函数前,blk 必须已经按地址有序插入空闲链表.
 *       基于地址有序不变量,prev/next 链表邻居就是空间上的潜在邻居,O(1) 完成.
 */
static void
mempool_merge_adjacent_blocks(const struct mempool *mp, struct mempool_blk *blk)
{
    struct list_head *prev_node = blk->blk_node.prev;
    if (prev_node != &mp->blk_root) {
        struct mempool_blk *prev_blk = CONTAINER_OF(prev_node, struct mempool_blk, blk_node);
        if ((uint8_t *)prev_blk + prev_blk->size == (uint8_t *)blk) {
            list_del(&blk->blk_node);
            prev_blk->size += blk->size;
            blk             = prev_blk;
        }
    }

    struct list_head *next_node = blk->blk_node.next;
    if (next_node != &mp->blk_root) {
        struct mempool_blk *next_blk = CONTAINER_OF(next_node, struct mempool_blk, blk_node);
        if ((uint8_t *)blk + blk->size == (uint8_t *)next_blk) {
            list_del(next_node);
            blk->size += next_blk->size;
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
mempool_init(struct mempool *mempool)
{
    list_init(&mempool->blk_root);
    mempool->offset       = 0;
    mempool->nfixed_pools = 0;
    memset(mempool->fixed_pools, 0, sizeof(mempool->fixed_pools));
    ATOMIC_STORE(&mempool->lock, 0);
}

int
mempool_add_fixed_pool(struct mempool *mempool,
                       void           *buf,
                       const size_t    block_size,
                       const uint32_t  block_count)
{
    if (!mempool || !buf || !block_size || !block_count || block_count > MEMPOOL_FIXED_BLOCK_MAX ||
        mempool->nfixed_pools >= MEMPOOL_FIXED_POOL_MAX || ((uintptr_t)buf & (MEMPOOL_ALIGN - 1U)))
        return -MEINVAL;

    const size_t aligned_block_size = MEMPOOL_ALIGN_UP(block_size);
    const size_t pool_size          = aligned_block_size * block_count;
    if (mempool->buf && mempool->cap &&
        mempool_ranges_overlap(mempool->buf, mempool->cap, buf, pool_size))
        return -MEINVAL;

    for (uint32_t i = 0; i < mempool->nfixed_pools; ++i) {
        struct mempool_fixed_pool *current = &mempool->fixed_pools[i];
        if (mempool_ranges_overlap(
                current->buf, current->block_size * current->block_count, buf, pool_size))
            return -MEXIST;
    }

    struct mempool_fixed_pool *pool = &mempool->fixed_pools[mempool->nfixed_pools++];
    pool->buf                       = buf;
    pool->block_size                = aligned_block_size;
    pool->block_count               = block_count;
    ATOMIC_STORE(&pool->free_bitmap, mempool_fixed_valid_mask(pool));
    return 0;
}

uint32_t
mempool_fixed_available(const struct mempool *mempool, const size_t block_size)
{
    if (!mempool)
        return 0;
    const size_t aligned_size = MEMPOOL_ALIGN_UP(block_size);
    for (uint32_t i = 0; i < mempool->nfixed_pools; ++i)
        if (mempool->fixed_pools[i].block_size == aligned_size)
            return popcount32(
                ATOMIC_LOAD_EXPLICIT(&mempool->fixed_pools[i].free_bitmap, ATOMIC_ACQUIRE));
    return 0;
}

void *
mempool_alloc_fast(struct mempool *mempool, const size_t cap)
{
    return mempool ? mempool_alloc_from_fixed_pools(mempool, cap) : NULL;
}

void *
mempool_alloc(struct mempool *mempool, size_t cap)
{
    if (!mempool)
        return NULL;

    void *fast_ptr = mempool_alloc_from_fixed_pools(mempool, cap);
    if (fast_ptr)
        return fast_ptr;

    const size_t block_size = MEMPOOL_BLOCK_HEADER_SIZE + MEMPOOL_ALIGN_UP(cap);

    SPIN_LOCK(&mempool->lock);

    struct mempool_blk *blk = mempool_find_best_fit(mempool, block_size);

    if (blk) {
        list_del(&blk->blk_node);

        if (blk->size >= block_size + MEMPOOL_BLOCK_HEADER_SIZE + MEMPOOL_ALIGN) {
            struct mempool_blk *split_blk = (struct mempool_blk *)((uint8_t *)blk + block_size);
            split_blk->size               = blk->size - block_size;
            blk->size                     = block_size;

            mempool_insert_free_block_ordered(mempool, split_blk);
        }

        SPIN_UNLOCK(&mempool->lock);
        return (uint8_t *)blk + MEMPOOL_BLOCK_HEADER_SIZE;
    }

    if (mempool->offset + block_size > mempool->cap) {
        SPIN_UNLOCK(&mempool->lock);
        return NULL;
    }

    uint8_t            *buf_ptr    = (uint8_t *)mempool->buf;
    struct mempool_blk *fresh_blk  = (struct mempool_blk *)(buf_ptr + mempool->offset);
    fresh_blk->size                = block_size;
    mempool->offset               += block_size;

    SPIN_UNLOCK(&mempool->lock);
    return (uint8_t *)fresh_blk + MEMPOOL_BLOCK_HEADER_SIZE;
}

void *
mempool_calloc(struct mempool *mempool, const size_t cap)
{
    void *ptr = mempool_alloc(mempool, cap);
    if (!ptr)
        return NULL;

    memset(ptr, 0, cap);
    return ptr;
}

void *
mempool_calloc_fast(struct mempool *mempool, const size_t cap)
{
    void *ptr = mempool_alloc_fast(mempool, cap);
    if (ptr)
        memset(ptr, 0, cap);
    return ptr;
}

void
mempool_free(struct mempool *mempool, void *ptr)
{
    if (!mempool || !ptr)
        return;

    for (uint32_t i = 0; i < mempool->nfixed_pools; ++i) {
        if (mempool_fixed_contains(&mempool->fixed_pools[i], ptr)) {
            (void)mempool_fixed_free(&mempool->fixed_pools[i], ptr);
            return;
        }
    }

    struct mempool_blk *block = (struct mempool_blk *)((uint8_t *)ptr - MEMPOOL_BLOCK_HEADER_SIZE);

    if (!mempool_is_valid_ptr(mempool, block) || !mempool_is_valid_ptr(mempool, ptr)) {
        return;
    }

    SPIN_LOCK(&mempool->lock);

    mempool_insert_free_block_ordered(mempool, block);

    mempool_merge_adjacent_blocks(mempool, block);

    SPIN_UNLOCK(&mempool->lock);
}

void
mempool_reset(struct mempool *mempool)
{
    SPIN_LOCK(&mempool->lock);
    mempool->offset = 0;
    list_init(&mempool->blk_root);
    for (uint32_t i = 0; i < mempool->nfixed_pools; ++i)
        ATOMIC_STORE_EXPLICIT(&mempool->fixed_pools[i].free_bitmap,
                              mempool_fixed_valid_mask(&mempool->fixed_pools[i]),
                              ATOMIC_RELEASE);
    SPIN_UNLOCK(&mempool->lock);
}
