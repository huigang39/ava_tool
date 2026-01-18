#ifndef SPSC_H
#define SPSC_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

/* 写入数据超过剩余空间时的处理策略 */
typedef enum spsc_policy {
        SPSC_POLICY_TRUNCATE,  // 截断
        SPSC_POLICY_OVERWRITE, // 覆盖
        SPSC_POLICY_REJECT,    // 拒绝
} spsc_policy_e;

typedef struct spsc {
        spsc_policy_e e_policy; // 写入数据超过剩余空间时的处理策略
        void         *buf;      // 缓冲区
        usize         cap;      // 缓冲区容量(2^n)
        ATOMIC(usize) wp;       // 写入位置
        ATOMIC(usize) rp;       // 读取位置
} spsc_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

int   spsc_init(spsc_t *spsc, void *buf, usize cap, spsc_policy_e e_policy);
int   spsc_init_buf(spsc_t *spsc, usize cap, spsc_policy_e e_policy);
usize spsc_write(spsc_t *spsc, const void *src, usize size);
usize spsc_read(spsc_t *spsc, void *dst, usize size);
usize spsc_write_buf(spsc_t *spsc, void *buf, const void *src, usize size);
usize spsc_read_buf(spsc_t *spsc, void *buf, void *dst, usize size);

void  spsc_reset(spsc_t *spsc);
u8    spsc_empty(spsc_t *spsc);
u8    spsc_full(spsc_t *spsc);
usize spsc_avail(spsc_t *spsc);
usize spsc_free(spsc_t *spsc);
usize spsc_policy(spsc_t *spsc, usize wp, usize rp, usize size);

#ifdef __cplusplus
}
#endif

#endif // !SPSC_H
