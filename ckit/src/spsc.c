#include "spsc.h"
#include "errdef.h"
#include "macrodef.h"
#include "mathdef.h"

/**
 * @brief              按满队列策略计算实际写入长度
 * @param[in,out] spsc 队列对象
 * @param[in]     wp   写入位置
 * @param[in]     rp   读取位置
 * @param[in]     size 数据大小, 单位: byte
 */
static size_t spsc_policy(struct spsc *spsc,
                          const size_t wp,
                          const size_t rp,
                          size_t size);

int spsc_init(struct spsc *spsc,
              void *buf,
              const size_t cap,
              const enum spsc_policy policy)
{
    if (!spsc || !buf) {
        return MERR(MENULLPTR);
    }
    const int ret = spsc_init_buf(spsc, cap, policy);
    if (ret != 0) {
        return ret;
    }

    spsc->buf = buf;
    return 0;
}

int spsc_init_buf(struct spsc *spsc,
                  const size_t cap,
                  const enum spsc_policy policy)
{
    if (!spsc) {
        return MERR(MENULLPTR);
    }
    if (!IS_POWER_OF_2(cap) || policy < SPSC_POLICY_TRUNCATE ||
        policy > SPSC_POLICY_REJECT) {
        return MERR(MEINVAL);
    }

    spsc->policy = policy;
    spsc->cap = cap;
    ATOMIC_STORE(&spsc->rp, 0);
    ATOMIC_STORE(&spsc->wp, 0);
    return 0;
}

void spsc_reset(struct spsc *spsc)
{
    ATOMIC_STORE(&spsc->rp, 0);
    ATOMIC_STORE(&spsc->wp, 0);
}

uint8_t is_spsc_empty(struct spsc *spsc)
{
    return spsc_avail(spsc) == 0;
}

uint8_t is_spsc_full(struct spsc *spsc)
{
    return spsc_free(spsc) == 0;
}

size_t spsc_avail(struct spsc *spsc)
{
    return ATOMIC_LOAD(&spsc->wp) - ATOMIC_LOAD(&spsc->rp);
}

size_t spsc_free(struct spsc *spsc)
{
    return spsc->cap - spsc_avail(spsc);
}

size_t spsc_write(struct spsc *spsc, const void *source, const size_t size)
{
    return spsc_write_buf(spsc, spsc->buf, source, size);
}

size_t spsc_read(struct spsc *spsc, void *dest, const size_t size)
{
    return spsc_read_buf(spsc, spsc->buf, dest, size);
}

size_t spsc_write_buf(struct spsc *spsc,
                      void *buf,
                      const void *source,
                      size_t size)
{
    if (spsc->policy == SPSC_POLICY_OVERWRITE && size > spsc->cap) {
        source = (const uint8_t *)source + (size - spsc->cap);
        size = spsc->cap;
    }
    const size_t wp = ATOMIC_LOAD_EXPLICIT(&spsc->wp, ATOMIC_RELAXED);
    const size_t rp = ATOMIC_LOAD_EXPLICIT(&spsc->rp, ATOMIC_ACQUIRE);

    size = spsc_policy(spsc, wp, rp, size);
    if (size == 0) {
        return 0;
    }

    const size_t mask = spsc->cap - 1;
    const size_t offset = wp & mask;
    const size_t first = MIN(size, spsc->cap - offset);
    memcpy((uint8_t *)buf + offset, source, first);
    memcpy((uint8_t *)buf, (uint8_t *)source + first, size - first);

    ATOMIC_STORE_EXPLICIT(&spsc->wp, wp + size, ATOMIC_RELEASE);
    return size;
}

size_t spsc_read_buf(struct spsc *spsc, void *buf, void *dest, size_t size)
{
    const size_t rp = ATOMIC_LOAD_EXPLICIT(&spsc->rp, ATOMIC_RELAXED);
    const size_t wp = ATOMIC_LOAD_EXPLICIT(&spsc->wp, ATOMIC_ACQUIRE);

    const size_t avail_size = wp - rp;
    if (size > avail_size) {
        size = avail_size;
    }
    if (size == 0) {
        return 0;
    }

    const size_t mask = spsc->cap - 1;
    const size_t offset = rp & mask;
    const size_t first = MIN(size, spsc->cap - offset);
    memcpy(dest, (uint8_t *)buf + offset, first);
    memcpy((uint8_t *)dest + first, (uint8_t *)buf, size - first);

    ATOMIC_STORE_EXPLICIT(&spsc->rp, rp + size, ATOMIC_RELEASE);
    return size;
}

size_t spsc_peek(const struct spsc *spsc, void *dest, const size_t size)
{
    return spsc_peek_buf(spsc, spsc->buf, dest, size);
}

size_t spsc_peek_buf(const struct spsc *spsc,
                     const void *buf,
                     void *dest,
                     size_t size)
{
    const size_t rp = ATOMIC_LOAD_EXPLICIT(&spsc->rp, ATOMIC_RELAXED);
    const size_t wp = ATOMIC_LOAD_EXPLICIT(&spsc->wp, ATOMIC_ACQUIRE);
    size = MIN(size, wp - rp);
    if (size == 0U) {
        return 0U;
    }

    const size_t offset = rp & (spsc->cap - 1U);
    const size_t first = MIN(size, spsc->cap - offset);
    memcpy(dest, (const uint8_t *)buf + offset, first);
    memcpy((uint8_t *)dest + first, buf, size - first);
    return size;
}

size_t spsc_discard(struct spsc *spsc, size_t size)
{
    const size_t rp = ATOMIC_LOAD_EXPLICIT(&spsc->rp, ATOMIC_RELAXED);
    const size_t wp = ATOMIC_LOAD_EXPLICIT(&spsc->wp, ATOMIC_ACQUIRE);
    size = MIN(size, wp - rp);
    if (size != 0U) {
        ATOMIC_STORE_EXPLICIT(&spsc->rp, rp + size, ATOMIC_RELEASE);
    }
    return size;
}

static size_t spsc_policy(struct spsc *spsc,
                          const size_t wp,
                          const size_t rp,
                          size_t size)
{
    const size_t free_size = spsc->cap - (wp - rp);
    if (size <= free_size) {
        return size;
    }

    switch (spsc->policy) {
    case SPSC_POLICY_TRUNCATE: {
        size = free_size;
        return size;
    }
    case SPSC_POLICY_OVERWRITE: {
        ATOMIC_FETCH_ADD_EXPLICIT(&spsc->rp, size - free_size, ATOMIC_ACQ_REL);
        return size;
    }
    case SPSC_POLICY_REJECT: {
        return 0;
    }
    }
    return 0;
}
