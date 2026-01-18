#include "mpsc.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
mpsc_init(mpsc_t *mpsc, void *buf, const usize cap, mpsc_p_t *producers, const usize nproducers)
{
        mpsc->buf        = buf;
        mpsc->cap        = cap;
        mpsc->warp_end   = MPSC_OFFSET_MAX;
        mpsc->nproducers = nproducers;
        mpsc->producers  = producers;
}

void
mpsc_reg(const mpsc_t *mpsc, const usize id)
{
        ATOMIC_STORE_EXPLICIT(&mpsc->producers[id].write_end, MPSC_OFFSET_MAX, memory_order_relaxed);
        ATOMIC_STORE_EXPLICIT(&mpsc->producers[id].active, 1, memory_order_release);
}

void
mpsc_unreg(const mpsc_t *mpsc, const usize id)
{
        ATOMIC_STORE_EXPLICIT(&mpsc->producers[id].active, 0, memory_order_release);
}

usize
mpsc_get_wp(mpsc_t *mpsc)
{
        u32   cnt = SPINLOCK_BACKOFF_MIN;
        usize wp;

retry:
        wp = ATOMIC_LOAD_EXPLICIT(&mpsc->wp, memory_order_acquire);
        if (wp & MPSC_WRAP_LOCK_BIT) {
                SPINLOCK_BACKOFF(cnt);
                goto retry;
        }
        return wp;
}

usize
mpsc_get_reserve_pos(const mpsc_t *mpsc, const usize id)
{
        u32   cnt = SPINLOCK_BACKOFF_MIN;
        usize reserve_pos;

retry:
        reserve_pos = ATOMIC_LOAD_EXPLICIT(&mpsc->producers[id].write_end, memory_order_acquire);
        if (reserve_pos & MPSC_WRAP_LOCK_BIT) {
                SPINLOCK_BACKOFF(cnt);
                goto retry;
        }
        return reserve_pos;
}

/**
 * @brief 生产者申请在 MPSC 环形缓冲区中写入 size 的空间
 *
 * @param mpsc
 * @param id
 * @param size
 * @return 写入偏移 (逻辑 offset) 或者 -1 (表示空间不足)
 */
isize
mpsc_alloc(mpsc_t *mpsc, const usize id, const usize size)
{
        usize wp, offset, target;

        do {
                // 读取全局写指针
                wp = mpsc_get_wp(mpsc);

                // 提取实际偏移
                offset = wp & MPSC_OFFSET_MASK;

                // 标记正在申请写入
                ATOMIC_STORE_EXPLICIT(&mpsc->producers[id].write_end, offset | MPSC_WRAP_LOCK_BIT, memory_order_relaxed);

                // 尝试申请的终点位置
                target         = offset + size;
                const usize rp = ATOMIC_LOAD_EXPLICIT(&mpsc->rp, memory_order_relaxed);
                if (offset < rp && target >= rp) {
                        ATOMIC_STORE_EXPLICIT(&mpsc->producers[id].write_end, MPSC_OFFSET_MAX, memory_order_release);
                        return -1;
                }

                // 如果申请空间超过环尾
                if (target >= mpsc->cap) {
                        target = (target > mpsc->cap) ? (MPSC_WRAP_LOCK_BIT | size) : 0;
                        if ((target & MPSC_OFFSET_MASK) >= rp) {
                                ATOMIC_STORE_EXPLICIT(&mpsc->producers[id].write_end, MPSC_OFFSET_MAX, memory_order_release);
                                return -1;
                        }
                        target |= MPSC_WRAP_INCR(wp & MPSC_WRAP_COUNTER);
                } else
                        target |= wp & MPSC_WRAP_COUNTER;
        } while (!atomic_compare_exchange_weak(&mpsc->wp, &wp, target));

        // 清除 wrap lock bit，标记 reserve_pos 申请完成
        ATOMIC_STORE_EXPLICIT(
            &mpsc->producers[id].write_end, mpsc->producers[id].write_end & ~MPSC_WRAP_LOCK_BIT, memory_order_relaxed);

        // 如果申请触发 wrap
        if (target & MPSC_WRAP_LOCK_BIT) {
                mpsc->warp_end = offset;
                ATOMIC_STORE_EXPLICIT(&mpsc->wp, (target & ~MPSC_WRAP_LOCK_BIT), memory_order_release);
                offset = 0;
        }

        return (isize)offset;
}

void
mpsc_commit(const mpsc_t *mpsc, const usize id)
{
        ATOMIC_STORE_EXPLICIT(&mpsc->producers[id].write_end, MPSC_OFFSET_MAX, memory_order_release);
}

usize
mpsc_claim(mpsc_t *mpsc, usize *offset)
{
        usize rp = ATOMIC_LOAD_EXPLICIT(&mpsc->rp, memory_order_relaxed);
        usize wp;

retry:
        wp = mpsc_get_wp(mpsc) & MPSC_OFFSET_MASK;
        if (rp == wp)
                return 0;

        // 遍历所有 producer 的 reserve_pos
        // 找到最小的 reserve_pos (还没写完的数据)
        // consumer 只能读到这个位置，保证不读到未完成数据

        // 本次 pop 能安全读取的最大逻辑偏移量
        usize ready = MPSC_OFFSET_MAX;
        for (usize id = 0; id < mpsc->nproducers; id++) {
                if (!ATOMIC_LOAD_EXPLICIT(&mpsc->producers[id].active, memory_order_relaxed))
                        continue;

                const usize reserve_pos = mpsc_get_reserve_pos(mpsc, id);
                if (reserve_pos >= rp) {
                        if (reserve_pos < ready)
                                ready = reserve_pos;
                }
        }

        // 处理环形缓冲 wrap
        if (wp < rp) {
                const usize warp_end = (mpsc->warp_end == MPSC_OFFSET_MAX) ? mpsc->cap : mpsc->warp_end;
                if (ready == MPSC_OFFSET_MAX && rp == warp_end) {
                        if (mpsc->warp_end != MPSC_OFFSET_MAX)
                                mpsc->warp_end = MPSC_OFFSET_MAX;
                        rp = 0;
                        ATOMIC_STORE_EXPLICIT(&mpsc->rp, rp, memory_order_release);
                        goto retry;
                }
                ready = (ready < warp_end) ? ready : warp_end;
        } else
                ready = (ready < wp) ? ready : wp;

        const usize write_size = ready - rp;
        *offset                = rp;
        return write_size;
}

void
mpsc_free(mpsc_t *mpsc, const usize size)
{
        const usize write_size = mpsc->rp + size;
        mpsc->rp               = (write_size == mpsc->cap) ? 0 : write_size;
}

void
mpsc_append(const mpsc_t *mpsc, const usize offset, const void *src, const usize size)
{
        if (offset + size <= mpsc->cap)
                memcpy((u8 *)mpsc->buf + offset, src, size);
        else {
                const usize first = mpsc->cap - offset;
                memcpy((u8 *)mpsc->buf + offset, src, first);
                memcpy(mpsc->buf, (u8 *)src + first, size - first);
        }
}

isize
mpsc_write(mpsc_t *mpsc, const usize id, const void *src, const usize size)
{
        const isize offset = mpsc_alloc(mpsc, id, size);
        if (offset < 0)
                return -1;

        mpsc_append(mpsc, offset, src, size);
        mpsc_commit(mpsc, id);
        return offset;
}

usize
mpsc_read(mpsc_t *mpsc, void *dst, const usize size)
{
        usize       offset;
        const usize avail_size = mpsc_claim(mpsc, &offset);
        if (avail_size == 0)
                return 0;

        const usize read_size = (avail_size < size) ? 0 : size;

        if (offset + read_size <= mpsc->cap)
                memcpy(dst, (u8 *)mpsc->buf + offset, read_size);
        else {
                const usize first = mpsc->cap - offset;
                memcpy(dst, (u8 *)mpsc->buf + offset, first);
                memcpy((u8 *)dst + first, mpsc->buf, read_size - first);
        }

        mpsc_free(mpsc, read_size);
        return read_size;
}
