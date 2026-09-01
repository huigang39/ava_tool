#ifndef MACRODEF_H
#define MACRODEF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "errdef.h"
#include "platdef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIZE_128B (128U)
#define SIZE_256B (256U)

#define SIZE_1KB (1024U)
#define SIZE_2KB (2U * SIZE_1KB)
#define SIZE_4KB (4U * SIZE_1KB)
#define SIZE_8KB (8U * SIZE_1KB)
#define SIZE_16KB (16U * SIZE_1KB)
#define SIZE_32KB (32U * SIZE_1KB)
#define SIZE_64KB (64U * SIZE_1KB)
#define SIZE_128KB (128U * SIZE_1KB)

#define SIZE_1MB (SIZE_1KB * SIZE_1KB)
#define SIZE_2MB (2U * SIZE_1MB)
#define SIZE_4MB (4U * SIZE_1MB)
#define SIZE_16MB (16U * SIZE_1MB)

#define MAGIC_U32(ver, a, b, c)                                            \
    ((((uint32_t)(ver) & 0xFFU) << 24) | (((uint32_t)(a) & 0xFFU) << 16) | \
     (((uint32_t)(b) & 0xFFU) << 8) | ((uint32_t)(c) & 0xFFU))
#define MAGIC_U16(ver, a) \
    ((((uint16_t)(ver) & 0xFFU) << 8) | ((uint16_t)(a) & 0xFFU))

#define MAGIC_VER_U32(magic) ((uint8_t)(((uint32_t)(magic) >> 24) & 0xFFU))
#define MAGIC_VER_U16(magic) ((uint8_t)(((uint16_t)(magic) >> 8) & 0xFFU))
#define MAGIC_VER(magic) MAGIC_VER_U32(magic)

// 提取标识部分, 不包含高字节版本号
#define MAGIC_ID_U32(magic) ((uint32_t)(magic) & 0x00FFFFFFU)
#define MAGIC_ID_U16(magic) ((uint16_t)((uint16_t)(magic) & 0x00FFU))
#define MAGIC_ID(magic) MAGIC_ID_U32(magic)

// 返回清除标识后的值, 保留版本号
#define MAGIC_CLEAR_ID_U32(magic) ((uint32_t)(magic) & 0xFF000000U)
#define MAGIC_CLEAR_ID_U16(magic) ((uint16_t)((uint16_t)(magic) & 0xFF00U))
#define MAGIC_CLEAR_ID(magic) MAGIC_CLEAR_ID_U32(magic)

#define VER_PACK(major, minor, patch)      \
    ((((uint32_t)(major) & 0xFFU) << 16) | \
     (((uint32_t)(minor) & 0xFFU) << 8) | ((uint32_t)(patch) & 0xFFU))

#define MACRO_STRINGIFY_IMPL(val) #val
#define MACRO_STRINGIFY(val) MACRO_STRINGIFY_IMPL(val)
#define VER_STR(major, minor, patch) \
    MACRO_STRINGIFY(major)           \
    "." MACRO_STRINGIFY(minor) "." MACRO_STRINGIFY(patch)
#define VER_STR4(major, minor, patch, build) \
    VER_STR(major, minor, patch) "." MACRO_STRINGIFY(build)

#if defined(_MSC_VER) && !defined(__clang__)
#define AT(sec) __declspec(allocate(sec))
#define OPTNONE
#define ALIGN(n) __declspec(align(n))
#define FUNC_UNUSED
#define FUNC_USED
#define WEAK __declspec(selectany)
#ifdef __cplusplus
#define TYPEOF(var) decltype(var)
#else
#define TYPEOF(var) typeof(var)
#endif
#define PACKED __declspec(align(1))
#define NO_ASAN
#else
#define AT(sec) __attribute__((section(sec)))
#define OPTNONE __attribute__((optnone))
#define ALIGN(n) __attribute__((aligned(n)))
#define FUNC_UNUSED __attribute__((unused))
#define FUNC_USED __attribute__((used))
#define WEAK __attribute__((weak))
#define TYPEOF(var) __typeof__(var)
#define PACKED __attribute__((packed))
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NO_ASAN __attribute__((no_sanitize("address")))
#endif
#endif
#if !defined(NO_ASAN) && defined(__SANITIZE_ADDRESS__)
#define NO_ASAN __attribute__((no_sanitize("address")))
#endif
#ifndef NO_ASAN
#define NO_ASAN
#endif
#endif

// 向上对齐无符号整数, alignment 必须大于 0 且不含副作用
#define ALIGN_UP(size, alignment) \
    (((size) + (alignment) - 1U) / (alignment) * (alignment))

/*
 * Cache line 对齐配置, 可通过编译宏覆盖
 * 所有使用方须保持配置一致, 动态分配时也须满足对象对齐要求
 */
#ifndef CACHELINE_ENABLE
#define CACHELINE_ENABLE (0U)
#endif

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE (32U)
#endif

#if CACHELINE_ENABLE != 0U && CACHELINE_ENABLE != 1U
#error "CACHELINE_ENABLE must be 0 or 1"
#endif

#if CACHELINE_SIZE < 1U || (CACHELINE_SIZE & (CACHELINE_SIZE - 1U)) != 0U
#error "CACHELINE_SIZE must be a positive power of two"
#endif

// 仅控制对象对齐, 不控制硬件缓存启用或缓存维护
#if CACHELINE_ENABLE
#define CACHELINE_ALIGN ALIGN(CACHELINE_SIZE)
#else
#define CACHELINE_ALIGN
#endif

// Cortex-M 中断控制, mask 为 PRIMASK 的保存值
#if OS(NONE)
#define IRQ_DISABLE() __disable_irq()
#define IRQ_ENABLE() __enable_irq()
#define IRQ_GET_MASK() __get_PRIMASK()
#define IRQ_RESTORE(mask) __set_PRIMASK(mask)

// 执行临界区代码, 结束后恢复进入前的中断屏蔽状态
#define ATOMIC_EXEC(code)                          \
    do {                                           \
        const uint32_t __primask = IRQ_GET_MASK(); \
        IRQ_DISABLE();                             \
        {code};                                    \
        IRQ_RESTORE(__primask);                    \
    } while (0)
#else
#define ATOMIC_EXEC(code) {code}
#endif

#define ARG_UNUSED(arg) (void)(arg)

#define ARG_CHECK(arg)       \
    do {                     \
        if (!(arg))          \
            return -MEINVAL; \
    } while (0)

#define CFG_INIT(ptr, initial_cfg)  \
    do {                            \
        (ptr)->cfg = (initial_cfg); \
    } while (0)

#define CFG_READ(ptr)                            \
    const TYPEOF((ptr)->cfg) *cfg = &(ptr)->cfg; \
    ARG_UNUSED(cfg)

#define RENAME(ptr, name)     \
    TYPEOF((ptr)) name = ptr; \
    ARG_UNUSED(name);

#define DECL_1(ptr, a1)                 \
    TYPEOF((ptr)->a1) *a1 = &(ptr)->a1; \
    ARG_UNUSED(a1);
#define DECL_2(ptr, a1, a2) DECL_1(ptr, a1) DECL_1(ptr, a2)
#define DECL_3(ptr, a1, a2, a3) DECL_2(ptr, a1, a2) DECL_1(ptr, a3)
#define DECL_4(ptr, a1, a2, a3, a4) DECL_3(ptr, a1, a2, a3) DECL_1(ptr, a4)
#define DECL_5(ptr, a1, a2, a3, a4, a5) \
    DECL_4(ptr, a1, a2, a3, a4) DECL_1(ptr, a5)
#define DECL_6(ptr, a1, a2, a3, a4, a5, a6) \
    DECL_5(ptr, a1, a2, a3, a4, a5) DECL_1(ptr, a6)
#define DECL_7(ptr, a1, a2, a3, a4, a5, a6, a7) \
    DECL_6(ptr, a1, a2, a3, a4, a5, a6) DECL_1(ptr, a7)
#define DECL_8(ptr, a1, a2, a3, a4, a5, a6, a7, a8) \
    DECL_7(ptr, a1, a2, a3, a4, a5, a6, a7) DECL_1(ptr, a8)

#define RESET_1(ptr, a1) memset(&(ptr)->a1, 0, sizeof((ptr)->a1));
#define RESET_2(ptr, a1, a2) RESET_1(ptr, a1) RESET_1(ptr, a2)
#define RESET_3(ptr, a1, a2, a3) RESET_2(ptr, a1, a2) RESET_1(ptr, a3)
#define RESET_4(ptr, a1, a2, a3, a4) RESET_3(ptr, a1, a2, a3) RESET_1(ptr, a4)
#define RESET_5(ptr, a1, a2, a3, a4, a5) \
    RESET_4(ptr, a1, a2, a3, a4) RESET_1(ptr, a5)
#define RESET_6(ptr, a1, a2, a3, a4, a5, a6) \
    RESET_5(ptr, a1, a2, a3, a4, a5) RESET_1(ptr, a6)
#define RESET_7(ptr, a1, a2, a3, a4, a5, a6, a7) \
    RESET_6(ptr, a1, a2, a3, a4, a5, a6) RESET_1(ptr, a7)
#define RESET_8(ptr, a1, a2, a3, a4, a5, a6, a7, a8) \
    RESET_7(ptr, a1, a2, a3, a4, a5, a6, a7) RESET_1(ptr, a8)

#define ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, N, ...) N

#if defined(_MSC_VER)
#define EXPAND(x) x
#define CNT_ARGS(...) EXPAND(ARG_N(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0))

#define DECL_JOIN(a, b) a##b
#define DECL_N(n) DECL_JOIN(DECL_, n)
#define DECL(ptr, ...) EXPAND(DECL_N(CNT_ARGS(__VA_ARGS__))(ptr, __VA_ARGS__))

#define RESET_M(n) DECL_JOIN(RESET_, n)
#define RESET(ptr, ...) EXPAND(RESET_M(CNT_ARGS(__VA_ARGS__))(ptr, __VA_ARGS__))

#else
#define GET_MACRO(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME

#define DECL(ptr, ...)     \
    GET_MACRO(__VA_ARGS__, \
              DECL_8,      \
              DECL_7,      \
              DECL_6,      \
              DECL_5,      \
              DECL_4,      \
              DECL_3,      \
              DECL_2,      \
              DECL_1,      \
              ...)         \
    (ptr, __VA_ARGS__)

#define RESET(ptr, ...)    \
    GET_MACRO(__VA_ARGS__, \
              RESET_8,     \
              RESET_7,     \
              RESET_6,     \
              RESET_5,     \
              RESET_4,     \
              RESET_3,     \
              RESET_2,     \
              RESET_1,     \
              ...)         \
    (ptr, __VA_ARGS__)

#endif

#define SPINLOCK_BACKOFF_MIN (4U)
#define SPINLOCK_BACKOFF_MAX (128U)
#if ARCH(X86_FAMILY)
#if COMPILER(MSVC)
#include <intrin.h>
#define SPINLOCK_BACKOFF_HOOK _mm_pause()
#else
#define SPINLOCK_BACKOFF_HOOK __asm volatile("pause" ::: "memory")
#endif
#elif ARCH(ARM_FAMILY)
#define SPINLOCK_BACKOFF_HOOK __asm volatile("yield" ::: "memory")
#else
#define SPINLOCK_BACKOFF_HOOK
#endif
#define SPINLOCK_BACKOFF(cnt)               \
    do {                                    \
        for (size_t i = (cnt); i != 0; i--) \
            SPINLOCK_BACKOFF_HOOK;          \
        if ((cnt) < SPINLOCK_BACKOFF_MAX)   \
            (cnt) += (cnt);                 \
    } while (0);

#define SPIN_LOCK(lock_ptr)                               \
    do {                                                  \
        uint8_t __expected;                               \
        size_t __backoff = SPINLOCK_BACKOFF_MIN;          \
        for (;;) {                                        \
            __expected = false;                           \
            if (ATOMIC_CAS_WEAK_EXPLICIT((lock_ptr),      \
                                         &__expected,     \
                                         true,            \
                                         ATOMIC_ACQUIRE,  \
                                         ATOMIC_RELAXED)) \
                break;                                    \
            SPINLOCK_BACKOFF(__backoff);                  \
        }                                                 \
    } while (0)

#define SPIN_UNLOCK(lock_ptr) \
    ATOMIC_STORE_EXPLICIT((lock_ptr), false, ATOMIC_RELEASE)

#ifdef __cplusplus
#define IS_SAME_TYPE(a, b) std::is_same_v<decltype(a), decltype(b)>
#elif defined(_MSC_VER)
#define IS_SAME_TYPE(a, b) 0 // MSVC C 对类型检查的支持有限
#else
#define IS_SAME_TYPE(a, b) __builtin_types_compatible_p(TYPEOF(a), TYPEOF(b))
#endif

#ifdef __cplusplus
#define BUILD_BUG_ON_ZERO(e) ((sizeof(char[1 - 2 * !!(e)])) - 1)
#elif defined(_MSC_VER)
#define BUILD_BUG_ON_ZERO(e) (sizeof(char) * (e ? -1 : 0)) // MSVC 简化实现
#else
#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int : -!!(e); }))
#endif

#define MUST_BE_ARRAY(arr) BUILD_BUG_ON_ZERO(IS_SAME_TYPE((arr), &(arr)[0]))
#define ARRAY_LEN(arr) (sizeof(arr) / sizeof(arr[0]) + MUST_BE_ARRAY(arr))

#define CONTAINER_OF(ptr, type, member) \
    (type *)((char *)(ptr) - offsetof(type, member))

// ---------------------------------- 原子操作 ----------------------------------
#ifndef __cplusplus
#include <stdatomic.h>
#define ATOMIC(type) _Atomic(type)

#define ATOMIC_LOAD(object) atomic_load(object)
#define ATOMIC_LOAD_EXPLICIT(object, memory_order) \
    atomic_load_explicit(object, memory_order)

#define ATOMIC_STORE(object, desired) atomic_store(object, desired)
#define ATOMIC_STORE_EXPLICIT(object, desired, memory_order) \
    atomic_store_explicit(object, desired, memory_order)

#define ATOMIC_CAS_WEAK(object, expected, desired) \
    atomic_compare_exchange_weak(object, expected, desired)
#define ATOMIC_CAS_WEAK_EXPLICIT(                            \
    object, expected, desired, memory_order1, memory_order2) \
    atomic_compare_exchange_weak_explicit(                   \
        object, expected, desired, memory_order1, memory_order2)

#define ATOMIC_CAS_STRONG(object, expected, desired) \
    atomic_compare_exchange_strong(object, expected, desired)
#define ATOMIC_CAS_STRONG_EXPLICIT(                          \
    object, expected, desired, memory_order1, memory_order2) \
    atomic_compare_exchange_strong_explicit(                 \
        object, expected, desired, memory_order1, memory_order2)

#define ATOMIC_EXCHANGE(object, desired) atomic_exchange(object, desired)
#define ATOMIC_EXCHANGE_EXPLICIT(object, desired, memory_order) \
    atomic_exchange_explicit(object, desired, memory_order)

#define ATOMIC_FETCH_ADD(object, operand) atomic_fetch_add(object, operand)
#define ATOMIC_FETCH_ADD_EXPLICIT(object, operand, memory_order) \
    atomic_fetch_add_explicit(object, operand, memory_order)

#define ATOMIC_FETCH_SUB(object, operand) atomic_fetch_sub(object, operand)
#define ATOMIC_FETCH_SUB_EXPLICIT(object, operand, memory_order) \
    atomic_fetch_sub_explicit(object, operand, memory_order)

#define ATOMIC_RELAXED memory_order_relaxed
#define ATOMIC_CONSUME memory_order_consume
#define ATOMIC_ACQUIRE memory_order_acquire
#define ATOMIC_RELEASE memory_order_release
#define ATOMIC_ACQ_REL memory_order_acq_rel
#define ATOMIC_SEQ_CST memory_order_seq_cst
#else
}
#include <atomic>
#define ATOMIC(type) std::atomic<type>

#define ATOMIC_LOAD(object) std::atomic_load(object)
#define ATOMIC_LOAD_EXPLICIT(object, memory_order) \
    std::atomic_load_explicit(object, memory_order)

#define ATOMIC_STORE(object, desired) std::atomic_store(object, desired)
#define ATOMIC_STORE_EXPLICIT(object, desired, memory_order) \
    std::atomic_store_explicit(object, desired, memory_order)

#define ATOMIC_CAS_WEAK(object, expected, desired) \
    std::atomic_compare_exchange_weak(object, expected, desired)
#define ATOMIC_CAS_WEAK_EXPLICIT(                            \
    object, expected, desired, memory_order1, memory_order2) \
    std::atomic_compare_exchange_weak_explicit(              \
        object, expected, desired, memory_order1, memory_order2)

#define ATOMIC_CAS_STRONG(object, expected, desired) \
    std::atomic_compare_exchange_strong(object, expected, desired)
#define ATOMIC_CAS_STRONG_EXPLICIT(                          \
    object, expected, desired, memory_order1, memory_order2) \
    std::atomic_compare_exchange_strong_explicit(            \
        object, expected, desired, memory_order1, memory_order2)

#define ATOMIC_EXCHANGE(object, desired) std::atomic_exchange(object, desired)
#define ATOMIC_EXCHANGE_EXPLICIT(object, desired, memory_order) \
    std::atomic_exchange_explicit(object, desired, memory_order)

#define ATOMIC_FETCH_ADD(object, operand) std::atomic_fetch_add(object, operand)
#define ATOMIC_FETCH_ADD_EXPLICIT(object, operand, memory_order) \
    std::atomic_fetch_add_explicit(object, operand, memory_order)

#define ATOMIC_FETCH_SUB(object, operand) std::atomic_fetch_sub(object, operand)
#define ATOMIC_FETCH_SUB_EXPLICIT(object, operand, memory_order) \
    std::atomic_fetch_sub_explicit(object, operand, memory_order)

#define ATOMIC_RELAXED std::memory_order_relaxed
#define ATOMIC_CONSUME std::memory_order_consume
#define ATOMIC_ACQUIRE std::memory_order_acquire
#define ATOMIC_RELEASE std::memory_order_release
#define ATOMIC_ACQ_REL std::memory_order_acq_rel
#define ATOMIC_SEQ_CST std::memory_order_seq_cst
#endif
// ---------------------------------- 原子操作 ----------------------------------

// Cortex-M 数据同步屏障, 等待此前的显式内存访问完成
#if OS(NONE)
#define DATA_SYNC_BARRIER() __DSB()

// Cortex-M 指令同步屏障, 使后续指令使用更新后的处理器配置
#define INST_SYNC_BARRIER() __ISB()
#endif

/*
 * 内存访问顺序屏障, 用于发布或读取共享状态
 * 不替代缓存维护, DMA 完成检查或临界区
 */
#if defined(__CORTEX_M)
#define MEMORY_BARRIER() __DMB()
#elif defined(__cplusplus)
#define MEMORY_BARRIER() std::atomic_thread_fence(std::memory_order_seq_cst)
#else
#define MEMORY_BARRIER() atomic_thread_fence(memory_order_seq_cst)
#endif

#endif
