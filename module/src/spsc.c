#include "spsc.h"
#include "mathdef.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

int
spsc_init(spsc_t *spsc, void *buf, const usize cap, const spsc_policy_e e_policy)
{
        const int ret = spsc_init_buf(spsc, cap, e_policy);
        if (ret != 0)
                return ret;

        spsc->buf = buf;
        return 0;
}

int
spsc_init_buf(spsc_t *spsc, const usize cap, const spsc_policy_e e_policy)
{
        if (!IS_POWER_OF_2(cap))
                return -1;

        spsc->e_policy = e_policy;
        spsc->cap      = cap;
        ATOMIC_STORE(&spsc->rp, 0);
        ATOMIC_STORE(&spsc->wp, 0);
        return 0;
}

void
spsc_reset(spsc_t *spsc)
{
        ATOMIC_STORE(&spsc->rp, 0);
        ATOMIC_STORE(&spsc->wp, 0);
}

u8
spsc_empty(spsc_t *spsc)
{
        return spsc_avail(spsc) == 0;
}

u8
spsc_full(spsc_t *spsc)
{
        return spsc_free(spsc) == 0;
}

usize
spsc_avail(spsc_t *spsc)
{
        return ATOMIC_LOAD(&spsc->wp) - ATOMIC_LOAD(&spsc->rp);
}

usize
spsc_free(spsc_t *spsc)
{
        return spsc->cap - spsc_avail(spsc);
}

usize
spsc_policy(spsc_t *spsc, const usize wp, const usize rp, usize size)
{
        const usize free_size = spsc->cap - (wp - rp);
        if (size <= free_size)
                return size;

        switch (spsc->e_policy) {
                case SPSC_POLICY_TRUNCATE: {
                        size = free_size;
                        return size;
                }
                case SPSC_POLICY_OVERWRITE: {
                        atomic_fetch_add_explicit(&spsc->rp, size - free_size, memory_order_acq_rel);
                        return size;
                }
                case SPSC_POLICY_REJECT:
                        return 0;
        }
        return 0;
}

usize
spsc_write(spsc_t *spsc, const void *src, const usize size)
{
        return spsc_write_buf(spsc, spsc->buf, src, size);
}

usize
spsc_read(spsc_t *spsc, void *dst, const usize size)
{
        return spsc_read_buf(spsc, spsc->buf, dst, size);
}

usize
spsc_write_buf(spsc_t *spsc, void *buf, const void *src, usize size)
{
        const usize wp = ATOMIC_LOAD_EXPLICIT(&spsc->wp, memory_order_relaxed);
        const usize rp = ATOMIC_LOAD_EXPLICIT(&spsc->rp, memory_order_acquire);

        size = spsc_policy(spsc, wp, rp, size);
        if (size == 0)
                return 0;

        const usize mask   = spsc->cap - 1;
        const usize offset = wp & mask;
        const usize first  = MIN(size, spsc->cap - offset);
        memcpy((u8 *)buf + offset, src, first);
        memcpy((u8 *)buf, (u8 *)src + first, size - first);

        ATOMIC_STORE_EXPLICIT(&spsc->wp, wp + size, memory_order_release);
        return size;
}

usize
spsc_read_buf(spsc_t *spsc, void *buf, void *dst, usize size)
{
        const usize rp = ATOMIC_LOAD_EXPLICIT(&spsc->rp, memory_order_relaxed);
        const usize wp = ATOMIC_LOAD_EXPLICIT(&spsc->wp, memory_order_acquire);

        const usize avail_size = wp - rp;
        if (size > avail_size)
                size = avail_size;
        if (size == 0)
                return 0;

        const usize mask   = spsc->cap - 1;
        const usize offset = rp & mask;
        const usize first  = MIN(size, spsc->cap - offset);
        memcpy(dst, (u8 *)buf + offset, first);
        memcpy((u8 *)dst + first, (u8 *)buf, size - first);

        ATOMIC_STORE_EXPLICIT(&spsc->rp, rp + size, memory_order_release);
        return size;
}
