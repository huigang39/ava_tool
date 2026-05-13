#include "mempool.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 检查指针是否在内存池有效范围内
 */
static u8
mempool_is_valid_ptr(const mempool_t *mempool, const void *ptr)
{
        const u8 *p       = (const u8 *)ptr;
        const u8 *buf     = (const u8 *)mempool->buf;
        const u8 *buf_end = buf + mempool->cap;
        return p >= buf && p < buf_end;
}

/**
 * @brief 在空闲链表中查找最佳匹配块
 */
static mempool_blk_t *
mempool_find_best_fit(const mempool_t *mempool, const usize block_size)
{
        mempool_blk_t *best_blk  = NULL;
        usize          best_size = mempool->cap + 1; // 初始化为最大值

        list_head_t *node;
        LIST_FOR_EACH(node, &mempool->blk_root)
        {
                mempool_blk_t *blk = CONTAINER_OF(node, mempool_blk_t, blk_node);
                if (blk->size >= block_size && blk->size < best_size) {
                        best_blk  = blk;
                        best_size = blk->size;
                        // 如果找到完全匹配的块，提前返回
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
mempool_insert_free_block_ordered(const mempool_t *mp, mempool_blk_t *blk)
{
        list_head_t *pos = mp->blk_root.next;
        while (pos != &mp->blk_root && (u8 *)CONTAINER_OF(pos, mempool_blk_t, blk_node) < (u8 *)blk)
                pos = pos->next;

        list_add_tail(&blk->blk_node, pos);
}

/**
 * @brief 尝试合并相邻的空闲块
 * @note 调用此函数前，blk 必须已经按地址有序插入空闲链表。
 *       基于地址有序不变量，prev/next 链表邻居就是空间上的潜在邻居，O(1) 完成。
 */
static void
mempool_merge_adjacent_blocks(const mempool_t *mp, mempool_blk_t *blk)
{
        list_head_t *prev_node = blk->blk_node.prev;
        if (prev_node != &mp->blk_root) {
                mempool_blk_t *prev_blk = CONTAINER_OF(prev_node, mempool_blk_t, blk_node);
                if ((u8 *)prev_blk + prev_blk->size == (u8 *)blk) {
                        list_del(&blk->blk_node);
                        prev_blk->size += blk->size;
                        blk             = prev_blk;
                }
        }

        list_head_t *next_node = blk->blk_node.next;
        if (next_node != &mp->blk_root) {
                mempool_blk_t *next_blk = CONTAINER_OF(next_node, mempool_blk_t, blk_node);
                if ((u8 *)blk + blk->size == (u8 *)next_blk) {
                        list_del(next_node);
                        blk->size += next_blk->size;
                }
        }
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
mempool_init(mempool_t *mempool)
{
        list_init(&mempool->blk_root);
        mempool->offset = 0;
        ATOMIC_STORE(&mempool->lock, 0);
}

void *
mempool_alloc(mempool_t *mempool, usize cap)
{
        const usize block_size = MEMPOOL_BLOCK_HEADER_SIZE + MEMPOOL_ALIGN_UP(cap);

        SPIN_LOCK(&mempool->lock);

        // 使用 best-fit 策略查找最佳匹配块
        mempool_blk_t *blk = mempool_find_best_fit(mempool, block_size);

        if (blk) {
                // 找到合适的空闲块
                list_del(&blk->blk_node);

                if (blk->size >= block_size + MEMPOOL_BLOCK_HEADER_SIZE + MEMPOOL_ALIGN) {
                        // 能切分出一个新的空闲块
                        mempool_blk_t *split_blk = (mempool_blk_t *)((u8 *)blk + block_size);
                        split_blk->size          = blk->size - block_size;
                        blk->size                = block_size;

                        // 将切分出的块按地址顺序插回空闲链表
                        mempool_insert_free_block_ordered(mempool, split_blk);
                }

                SPIN_UNLOCK(&mempool->lock);
                return (u8 *)blk + MEMPOOL_BLOCK_HEADER_SIZE;
        }

        // 空闲链表没有合适块，则尝试向后线性分配
        if (mempool->offset + block_size > mempool->cap) {
                SPIN_UNLOCK(&mempool->lock);
                return NULL; // 内存池耗尽
        }

        u8            *buf_ptr    = (u8 *)mempool->buf;
        mempool_blk_t *fresh_blk  = (mempool_blk_t *)(buf_ptr + mempool->offset);
        fresh_blk->size           = block_size;
        mempool->offset          += block_size;

        SPIN_UNLOCK(&mempool->lock);
        return (u8 *)fresh_blk + MEMPOOL_BLOCK_HEADER_SIZE;
}

void *
mempool_calloc(mempool_t *mempool, const usize cap)
{
        void *ptr = mempool_alloc(mempool, cap);
        if (!ptr)
                return NULL;

        memset(ptr, 0, cap);
        return ptr;
}

void
mempool_free(mempool_t *mempool, void *ptr)
{
        if (!ptr)
                return;

        // 回退到块头部，恢复块结构
        mempool_blk_t *block = (mempool_blk_t *)((u8 *)ptr - MEMPOOL_BLOCK_HEADER_SIZE);

        // 边界检查：验证指针是否在有效范围内
        if (!mempool_is_valid_ptr(mempool, block) || !mempool_is_valid_ptr(mempool, ptr)) {
                // 无效指针，静默忽略（或可以添加日志）
                return;
        }

        SPIN_LOCK(&mempool->lock);

        // 将释放块按地址顺序插入空闲链表
        mempool_insert_free_block_ordered(mempool, block);

        // 尝试合并相邻的空闲块以减少碎片
        mempool_merge_adjacent_blocks(mempool, block);

        SPIN_UNLOCK(&mempool->lock);
}

void
mempool_reset(mempool_t *mempool)
{
        SPIN_LOCK(&mempool->lock);
        mempool->offset = 0;           // 回收整块缓冲区
        list_init(&mempool->blk_root); // 清空空闲链表
        SPIN_UNLOCK(&mempool->lock);
}
