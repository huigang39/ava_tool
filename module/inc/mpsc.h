#ifndef MPSC_H
#define MPSC_H

#include "macrodef.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

#ifdef MCU
#define MPSC_OFFSET_MASK   (0x0000FFFF)
#define MPSC_WRAP_LOCK_BIT (0x80000000)
#define MPSC_OFFSET_MAX    (UINT32_MAX & ~MPSC_WRAP_LOCK_BIT)
#define MPSC_WRAP_COUNTER  (0x7FFF0000)
#define MPSC_WRAP_INCR(x)  (((x) + 0x00010000U) & MPSC_WRAP_COUNTER)
#else
#define MPSC_OFFSET_MASK   (0x00000000FFFFFFFF)
#define MPSC_WRAP_LOCK_BIT (0x8000000000000000)
#define MPSC_OFFSET_MAX    (UINT64_MAX & ~MPSC_WRAP_LOCK_BIT)
#define MPSC_WRAP_COUNTER  (0x7FFFFFFF00000000)
#define MPSC_WRAP_INCR(x)  (((x) + 0x0000000100000000UL) & MPSC_WRAP_COUNTER)
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct mpsc_p {
        ATOMIC(usize) write_end; // 当前生产者申请的写区终点
        ATOMIC(u8) active;       // 是否活跃
} mpsc_p_t;

typedef struct mpsc {
        void *buf;            // 环形缓冲区存放实际数据
        usize cap;            // 环形缓冲区容量
        usize warp_end;       // wrap 回绕的标记位置
        ATOMIC(usize) wp;     // 全局写指针(生产者共享)
        ATOMIC(usize) rp;     // 全局读指针(消费者独占)
        mpsc_p_t *producers;  // 生产者状态数组
        usize     nproducers; // 生产者数量
} mpsc_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void  mpsc_init(mpsc_t *mpsc, void *buf, usize cap, mpsc_p_t *producers, usize nproducers);
void  mpsc_reg(const mpsc_t *mpsc, usize id);
void  mpsc_unreg(const mpsc_t *mpsc, usize id);
isize mpsc_write(mpsc_t *mpsc, usize id, const void *src, usize size);
usize mpsc_read(mpsc_t *mpsc, void *dst, usize size);

isize mpsc_alloc(mpsc_t *mpsc, usize id, usize size);
void  mpsc_commit(const mpsc_t *mpsc, usize id);
usize mpsc_claim(mpsc_t *mpsc, usize *offset);
void  mpsc_free(mpsc_t *mpsc, usize size);
void  mpsc_append(const mpsc_t *mpsc, usize offset, const void *src, usize size);

#ifdef __cplusplus
}
#endif

#endif // !MPSC_H
