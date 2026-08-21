#include "spsc.h"
#include "macrodef.h"
#include "mathdef.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

static size_t
spsc_policy(struct spsc *spsc, const size_t wp, const size_t rp, size_t size)
{
    const size_t free_size = spsc->cap - (wp - rp);
    if (size <= free_size)
        return size;

    switch (spsc->e_policy) {
        case SPSC_POLICY_TRUNCATE: {
            size = free_size;
            return size;
        }
        case SPSC_POLICY_OVERWRITE: {
            ATOMIC_FETCH_ADD_EXPLICIT(&spsc->rp, size - free_size, ATOMIC_ACQ_REL);
            return size;
        }
        case SPSC_POLICY_REJECT:
            return 0;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

int
spsc_init(struct spsc *spsc, void *buf, const size_t cap, const enum spsc_policy e_policy)
{
    const int ret = spsc_init_buf(spsc, cap, e_policy);
    if (ret != 0)
        return ret;

    spsc->buf = buf;
    return 0;
}

int
spsc_init_buf(struct spsc *spsc, const size_t cap, const enum spsc_policy e_policy)
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
spsc_reset(struct spsc *spsc)
{
    ATOMIC_STORE(&spsc->rp, 0);
    ATOMIC_STORE(&spsc->wp, 0);
}

uint8_t
spsc_empty(struct spsc *spsc)
{
    return spsc_avail(spsc) == 0;
}

uint8_t
spsc_full(struct spsc *spsc)
{
    return spsc_free(spsc) == 0;
}

size_t
spsc_avail(struct spsc *spsc)
{
    return ATOMIC_LOAD(&spsc->wp) - ATOMIC_LOAD(&spsc->rp);
}

size_t
spsc_free(struct spsc *spsc)
{
    return spsc->cap - spsc_avail(spsc);
}

size_t
spsc_write(struct spsc *spsc, const void *src, const size_t size)
{
    return spsc_write_buf(spsc, spsc->buf, src, size);
}

size_t
spsc_read(struct spsc *spsc, void *dst, const size_t size)
{
    return spsc_read_buf(spsc, spsc->buf, dst, size);
}

size_t
spsc_write_buf(struct spsc *spsc, void *buf, const void *src, size_t size)
{
    const size_t wp = ATOMIC_LOAD_EXPLICIT(&spsc->wp, ATOMIC_RELAXED);
    const size_t rp = ATOMIC_LOAD_EXPLICIT(&spsc->rp, ATOMIC_ACQUIRE);

    size = spsc_policy(spsc, wp, rp, size);
    if (size == 0)
        return 0;

    const size_t mask   = spsc->cap - 1;
    const size_t offset = wp & mask;
    const size_t first  = MIN(size, spsc->cap - offset);
    memcpy((uint8_t *)buf + offset, src, first);
    memcpy((uint8_t *)buf, (uint8_t *)src + first, size - first);

    ATOMIC_STORE_EXPLICIT(&spsc->wp, wp + size, ATOMIC_RELEASE);
    return size;
}

size_t
spsc_read_buf(struct spsc *spsc, void *buf, void *dst, size_t size)
{
    const size_t rp = ATOMIC_LOAD_EXPLICIT(&spsc->rp, ATOMIC_RELAXED);
    const size_t wp = ATOMIC_LOAD_EXPLICIT(&spsc->wp, ATOMIC_ACQUIRE);

    const size_t avail_size = wp - rp;
    if (size > avail_size)
        size = avail_size;
    if (size == 0)
        return 0;

    const size_t mask   = spsc->cap - 1;
    const size_t offset = rp & mask;
    const size_t first  = MIN(size, spsc->cap - offset);
    memcpy(dst, (uint8_t *)buf + offset, first);
    memcpy((uint8_t *)dst + first, (uint8_t *)buf, size - first);

    ATOMIC_STORE_EXPLICIT(&spsc->rp, rp + size, ATOMIC_RELEASE);
    return size;
}
