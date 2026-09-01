#ifndef SPSC_H
#define SPSC_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

// 写入数据超过剩余空间时的处理策略
enum spsc_policy {
    SPSC_POLICY_TRUNCATE,  // 截断
    SPSC_POLICY_OVERWRITE, // 覆盖; 读写须外部串行化, 大块仅保留末尾 cap 字节
    SPSC_POLICY_REJECT,    // 拒绝
};

struct spsc {
    // 写入策略与缓冲区
    CACHELINE_ALIGN enum spsc_policy policy; // 写入数据超过剩余空间时的处理策略
    void *buf;                               // 缓冲区
    size_t cap;                              // 缓冲区容量(2^n)

    // 生产者写入位置
    CACHELINE_ALIGN ATOMIC(size_t) wp; // 写入位置, 开启优化后独立对齐

    // 消费者读取位置
    CACHELINE_ALIGN ATOMIC(size_t) rp; // 读取位置, 开启优化后独立对齐
};

#if CACHELINE_ENABLE
static_assert(CACHELINE_SIZE >= sizeof(ATOMIC(size_t)),
              "SPSC cache line is too small");
#endif

/**
 * @brief                初始化队列状态
 * @param[in,out] spsc   有效的队列对象
 * @param[in,out] buf    调用方提供的队列存储
 * @param[in]     cap    缓冲区容量, 必须为 2 的幂, 单位: byte
 * @param[in]     policy 空间不足时的写入策略
 * @return               成功返回 0, 参数无效返回负错误码
 */
int spsc_init(struct spsc *spsc,
              void *buf,
              size_t cap,
              enum spsc_policy policy);

/**
 * @brief                初始化队列状态
 * @param[in,out] spsc   有效的队列对象
 * @param[in]     cap    缓冲区容量, 必须为 2 的幂, 单位: byte
 * @param[in]     policy 空间不足时的写入策略
 * @return               成功返回 0, 参数无效返回负错误码
 */
int spsc_init_buf(struct spsc *spsc, size_t cap, enum spsc_policy policy);

/**
 * @brief                写入队列数据
 * @param[in,out] spsc   已初始化的队列
 * @param[in]     source 写入数据源
 * @param[in]     size   请求长度, 单位: byte
 * @return               实际传输的数据长度, 单位: byte
 */
size_t spsc_write(struct spsc *spsc, const void *source, size_t size);

/**
 * @brief              读取并移除队头数据
 * @param[in,out] spsc 已初始化的队列
 * @param[out]    dest 读取目标存储
 * @param[in]     size 请求长度, 单位: byte
 * @return             实际传输的数据长度, 单位: byte
 */
size_t spsc_read(struct spsc *spsc, void *dest, size_t size);

/**
 * @brief                写入队列数据
 * @param[in,out] spsc   已初始化的队列
 * @param[in,out] buf    调用方提供的队列存储
 * @param[in]     source 写入数据源
 * @param[in]     size   请求长度, 单位: byte
 * @return               实际传输的数据长度, 单位: byte
 */
size_t spsc_write_buf(struct spsc *spsc,
                      void *buf,
                      const void *source,
                      size_t size);

/**
 * @brief              读取并移除队头数据
 * @param[in,out] spsc 已初始化的队列
 * @param[in]     buf  调用方提供的队列存储
 * @param[out]    dest 读取目标存储
 * @param[in]     size 请求长度, 单位: byte
 * @return             实际传输的数据长度, 单位: byte
 */
size_t spsc_read_buf(struct spsc *spsc, void *buf, void *dest, size_t size);

/**
 * @brief           查看队列头部数据, 不推进读指针
 * @param[in]  spsc 已初始化的队列
 * @param[out] dest 接收数据的存储, 容量至少为 size 字节, 不得与队列存储重叠
 * @param[in]  size 最多查看的字节数
 * @return          实际复制的字节数, 不超过当前可读数据量
 * @note            仅由消费者调用, 支持环形回绕; 覆盖策略仍需外部串行化
 */
size_t spsc_peek(const struct spsc *spsc, void *dest, size_t size);

/**
 * @brief           使用外部存储查看队列头部数据, 不推进读指针
 * @param[in]  spsc 已初始化的队列
 * @param[in]  buf  与队列读写使用的同一存储
 * @param[out] dest 接收数据的存储, 容量至少为 size 字节, 不得与 buf 重叠
 * @param[in]  size 最多查看的字节数
 * @return          实际复制的字节数, 空队列或 size 为 0 时返回 0
 * @note            并发约束与 spsc_peek 相同
 */
size_t spsc_peek_buf(const struct spsc *spsc,
                     const void *buf,
                     void *dest,
                     size_t size);

/**
 * @brief              丢弃队列头部数据, 不复制或清零存储
 * @param[in,out] spsc 已初始化的队列
 * @param[in]     size 最多丢弃的字节数
 * @return             实际丢弃的字节数, 不超过当前可读数据量
 * @note               仅由消费者调用, 与 read 共用读指针; 覆盖策略仍需外部串行化
 */
size_t spsc_discard(struct spsc *spsc, size_t size);

/**
 * @brief              重置队列读写位置
 * @param[in,out] spsc 队列对象
 */
void spsc_reset(struct spsc *spsc);

/**
 * @brief              判断队列是否为空
 * @param[in,out] spsc 队列对象
 * @return             队列为空返回 true, 否则返回 false
 */
uint8_t is_spsc_empty(struct spsc *spsc);

/**
 * @brief              判断队列是否已满
 * @param[in,out] spsc 队列对象
 * @return             队列已满返回 true, 否则返回 false
 */
uint8_t is_spsc_full(struct spsc *spsc);

/**
 * @brief              获取可读取的元素数量
 * @param[in,out] spsc 队列对象
 * @return             可读取的字节数
 */
size_t spsc_avail(struct spsc *spsc);

/**
 * @brief              获取剩余可写入的元素数量
 * @param[in,out] spsc 队列对象
 * @return             可写入的字节数
 */
size_t spsc_free(struct spsc *spsc);

#ifdef __cplusplus
}
#endif

#endif
