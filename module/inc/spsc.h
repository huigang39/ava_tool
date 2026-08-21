#ifndef SPSC_H
#define SPSC_H
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

/* 写入数据超过剩余空间时的处理策略 */
enum spsc_policy {
    SPSC_POLICY_TRUNCATE,  // 截断
    SPSC_POLICY_OVERWRITE, // 覆盖
    SPSC_POLICY_REJECT,    // 拒绝
};

struct spsc {
    enum spsc_policy e_policy; // 写入数据超过剩余空间时的处理策略
    void            *buf;      // 缓冲区
    size_t           cap;      // 缓冲区容量(2^n)
    ATOMIC(size_t) wp;         // 写入位置
    ATOMIC(size_t) rp;         // 读取位置
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

int    spsc_init(struct spsc *spsc, void *buf, size_t cap, enum spsc_policy e_policy);
int    spsc_init_buf(struct spsc *spsc, size_t cap, enum spsc_policy e_policy);
size_t spsc_write(struct spsc *spsc, const void *src, size_t size);
size_t spsc_read(struct spsc *spsc, void *dst, size_t size);
size_t spsc_write_buf(struct spsc *spsc, void *buf, const void *src, size_t size);
size_t spsc_read_buf(struct spsc *spsc, void *buf, void *dst, size_t size);

void    spsc_reset(struct spsc *spsc);
uint8_t spsc_empty(struct spsc *spsc);
uint8_t spsc_full(struct spsc *spsc);
size_t  spsc_avail(struct spsc *spsc);
size_t  spsc_free(struct spsc *spsc);

#ifdef __cplusplus
}
#endif

#endif // !SPSC_H
