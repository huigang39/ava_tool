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

#define SIZE_128B  (128U)
#define SIZE_256B  (256U)

#define SIZE_1KB   (1024U)
#define SIZE_2KB   (2U * SIZE_1KB)
#define SIZE_4KB   (4U * SIZE_1KB)
#define SIZE_8KB   (8U * SIZE_1KB)
#define SIZE_16KB  (16U * SIZE_1KB)
#define SIZE_32KB  (32U * SIZE_1KB)
#define SIZE_64KB  (64U * SIZE_1KB)
#define SIZE_128KB (128U * SIZE_1KB)

#define SIZE_1MB   (SIZE_1KB * SIZE_1KB)
#define SIZE_2MB   (2U * SIZE_1MB)
#define SIZE_4MB   (4U * SIZE_1MB)
#define SIZE_16MB  (16U * SIZE_1MB)

#define MAGIC_U32(a, b, c, d)                                            \
    ((((uint32_t)(a) & 0xFFU) << 24) | (((uint32_t)(b) & 0xFFU) << 16) | \
     (((uint32_t)(c) & 0xFFU) << 8) | ((uint32_t)(d) & 0xFFU))

#define VERSION_PACK(major, minor, patch)                                       \
    ((((uint32_t)(major) & 0xFFU) << 16) | (((uint32_t)(minor) & 0xFFU) << 8) | \
     ((uint32_t)(patch) & 0xFFU))

#define MACRO_STRINGIFY_IMPL(value) #value
#define MACRO_STRINGIFY(value)      MACRO_STRINGIFY_IMPL(value)
#define VERSION_STRING(major, minor, patch) \
    MACRO_STRINGIFY(major)                  \
    "." MACRO_STRINGIFY(minor) "." MACRO_STRINGIFY(patch)
#define VERSION_STRING4(major, minor, patch, build) \
    VERSION_STRING(major, minor, patch) "." MACRO_STRINGIFY(build)

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
#define AT(sec)     __attribute__((section(sec)))
#define OPTNONE     __attribute__((optnone))
#define ALIGN(n)    __attribute__((aligned(n)))
#define FUNC_UNUSED __attribute__((unused))
#define FUNC_USED   __attribute__((used))
#define WEAK        __attribute__((weak))
#define TYPEOF(var) __typeof__(var)
#define PACKED      __attribute__((packed))
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

#if OS(NONE)
#define ATOMIC_EXEC(code)                              \
    do {                                               \
        volatile uint32_t __primask = __get_PRIMASK(); \
        __disable_irq();                               \
        {code};                                        \
        __set_PRIMASK(__primask);                      \
    } while (0)
#else
#define ATOMIC_EXEC(code) {code}
#endif

#define HAPI            static inline

#define ARG_UNUSED(arg) (void)(arg)

#define ARG_CHECK(arg)       \
    do {                     \
        if (!(arg))          \
            return -MEINVAL; \
    } while (0)

#define CFG_INIT(ptr, config)  \
    do {                       \
        (ptr)->cfg = (config); \
    } while (0)

/* 将采样频率写入一个或多个配置对象的 fs 字段。 */
#define FS_INIT_1(fs_val, p1) \
    do {                      \
        (p1)->fs = (fs_val);  \
    } while (0)
#define FS_INIT_2(fs_val, p1, p2) \
    do {                          \
        FS_INIT_1(fs_val, p1);    \
        FS_INIT_1(fs_val, p2);    \
    } while (0)
#define FS_INIT_3(fs_val, p1, p2, p3) \
    do {                              \
        FS_INIT_2(fs_val, p1, p2);    \
        FS_INIT_1(fs_val, p3);        \
    } while (0)
#define FS_INIT_4(fs_val, p1, p2, p3, p4) \
    do {                                  \
        FS_INIT_3(fs_val, p1, p2, p3);    \
        FS_INIT_1(fs_val, p4);            \
    } while (0)
#define FS_INIT_5(fs_val, p1, p2, p3, p4, p5) \
    do {                                      \
        FS_INIT_4(fs_val, p1, p2, p3, p4);    \
        FS_INIT_1(fs_val, p5);                \
    } while (0)
#define FS_INIT_6(fs_val, p1, p2, p3, p4, p5, p6) \
    do {                                          \
        FS_INIT_5(fs_val, p1, p2, p3, p4, p5);    \
        FS_INIT_1(fs_val, p6);                    \
    } while (0)
#define FS_INIT_7(fs_val, p1, p2, p3, p4, p5, p6, p7) \
    do {                                              \
        FS_INIT_6(fs_val, p1, p2, p3, p4, p5, p6);    \
        FS_INIT_1(fs_val, p7);                        \
    } while (0)
#define FS_INIT_8(fs_val, p1, p2, p3, p4, p5, p6, p7, p8) \
    do {                                                  \
        FS_INIT_7(fs_val, p1, p2, p3, p4, p5, p6, p7);    \
        FS_INIT_1(fs_val, p8);                            \
    } while (0)

#define RENAME(ptr, name)     \
    TYPEOF((ptr)) name = ptr; \
    ARG_UNUSED(name);

#define DECL_1(ptr, a1)                 \
    TYPEOF((ptr)->a1) *a1 = &(ptr)->a1; \
    ARG_UNUSED(a1);
#define DECL_2(ptr, a1, a2)                     DECL_1(ptr, a1) DECL_1(ptr, a2)
#define DECL_3(ptr, a1, a2, a3)                 DECL_2(ptr, a1, a2) DECL_1(ptr, a3)
#define DECL_4(ptr, a1, a2, a3, a4)             DECL_3(ptr, a1, a2, a3) DECL_1(ptr, a4)
#define DECL_5(ptr, a1, a2, a3, a4, a5)         DECL_4(ptr, a1, a2, a3, a4) DECL_1(ptr, a5)
#define DECL_6(ptr, a1, a2, a3, a4, a5, a6)     DECL_5(ptr, a1, a2, a3, a4, a5) DECL_1(ptr, a6)
#define DECL_7(ptr, a1, a2, a3, a4, a5, a6, a7) DECL_6(ptr, a1, a2, a3, a4, a5, a6) DECL_1(ptr, a7)
#define DECL_8(ptr, a1, a2, a3, a4, a5, a6, a7, a8) \
    DECL_7(ptr, a1, a2, a3, a4, a5, a6, a7) DECL_1(ptr, a8)

#define RESET_1(ptr, a1)                     memset(&(ptr)->a1, 0, sizeof((ptr)->a1));
#define RESET_2(ptr, a1, a2)                 RESET_1(ptr, a1) RESET_1(ptr, a2)
#define RESET_3(ptr, a1, a2, a3)             RESET_2(ptr, a1, a2) RESET_1(ptr, a3)
#define RESET_4(ptr, a1, a2, a3, a4)         RESET_3(ptr, a1, a2, a3) RESET_1(ptr, a4)
#define RESET_5(ptr, a1, a2, a3, a4, a5)     RESET_4(ptr, a1, a2, a3, a4) RESET_1(ptr, a5)
#define RESET_6(ptr, a1, a2, a3, a4, a5, a6) RESET_5(ptr, a1, a2, a3, a4, a5) RESET_1(ptr, a6)
#define RESET_7(ptr, a1, a2, a3, a4, a5, a6, a7) \
    RESET_6(ptr, a1, a2, a3, a4, a5, a6) RESET_1(ptr, a7)
#define RESET_8(ptr, a1, a2, a3, a4, a5, a6, a7, a8) \
    RESET_7(ptr, a1, a2, a3, a4, a5, a6, a7) RESET_1(ptr, a8)

#define ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, N, ...) N

#if defined(_MSC_VER)
#define EXPAND(x)            x
#define COUNT_ARGS(...)      EXPAND(ARG_N(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0))

#define DECL_JOIN(a, b)      a##b
#define DECL_N(n)            DECL_JOIN(DECL_, n)
#define DECL(ptr, ...)       EXPAND(DECL_N(COUNT_ARGS(__VA_ARGS__))(ptr, __VA_ARGS__))

#define RESET_M(n)           DECL_JOIN(RESET_, n)
#define RESET(ptr, ...)      EXPAND(RESET_M(COUNT_ARGS(__VA_ARGS__))(ptr, __VA_ARGS__))

#define FS_INIT_M(n)         DECL_JOIN(FS_INIT_, n)
#define FS_INIT(fs_val, ...) EXPAND(FS_INIT_M(COUNT_ARGS(__VA_ARGS__))(fs_val, __VA_ARGS__))
#else
#define GET_MACRO(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME

#define DECL(ptr, ...)                                                                          \
    GET_MACRO(__VA_ARGS__, DECL_8, DECL_7, DECL_6, DECL_5, DECL_4, DECL_3, DECL_2, DECL_1, ...) \
    (ptr, __VA_ARGS__)

#define RESET(ptr, ...)                                                                           \
    GET_MACRO(                                                                                    \
        __VA_ARGS__, RESET_8, RESET_7, RESET_6, RESET_5, RESET_4, RESET_3, RESET_2, RESET_1, ...) \
    (ptr, __VA_ARGS__)

#define FS_INIT(fs_val, ...) \
    GET_MACRO(__VA_ARGS__,   \
              FS_INIT_8,     \
              FS_INIT_7,     \
              FS_INIT_6,     \
              FS_INIT_5,     \
              FS_INIT_4,     \
              FS_INIT_3,     \
              FS_INIT_2,     \
              FS_INIT_1,     \
              ...)           \
    (fs_val, __VA_ARGS__)
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

#define SPIN_LOCK(lock_ptr)                                                         \
    do {                                                                            \
        uint8_t __expected;                                                         \
        size_t  __backoff = SPINLOCK_BACKOFF_MIN;                                   \
        for (;;) {                                                                  \
            __expected = false;                                                     \
            if (ATOMIC_CAS_WEAK_EXPLICIT(                                           \
                    (lock_ptr), &__expected, true, ATOMIC_ACQUIRE, ATOMIC_RELAXED)) \
                break;                                                              \
            SPINLOCK_BACKOFF(__backoff);                                            \
        }                                                                           \
    } while (0)

#define SPIN_UNLOCK(lock_ptr) ATOMIC_STORE_EXPLICIT((lock_ptr), false, ATOMIC_RELEASE)

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

#define MUST_BE_ARRAY(arr)              BUILD_BUG_ON_ZERO(IS_SAME_TYPE((arr), &(arr)[0]))
#define ARRAY_LEN(arr)                  (sizeof(arr) / sizeof(arr[0]) + MUST_BE_ARRAY(arr))

#define CONTAINER_OF(ptr, type, member) (type *)((char *)(ptr) - offsetof(type, member))

/* ---------------------------------- 原子操作 ---------------------------------- */
#ifndef __cplusplus
#include <stdatomic.h>
#define ATOMIC(type)                               _Atomic(type)

#define ATOMIC_LOAD(object)                        atomic_load(object)
#define ATOMIC_LOAD_EXPLICIT(object, memory_order) atomic_load_explicit(object, memory_order)

#define ATOMIC_STORE(object, desired)              atomic_store(object, desired)
#define ATOMIC_STORE_EXPLICIT(object, desired, memory_order) \
    atomic_store_explicit(object, desired, memory_order)

#define ATOMIC_CAS_WEAK(object, expected, desired) \
    atomic_compare_exchange_weak(object, expected, desired)
#define ATOMIC_CAS_WEAK_EXPLICIT(object, expected, desired, memory_order1, memory_order2) \
    atomic_compare_exchange_weak_explicit(object, expected, desired, memory_order1, memory_order2)

#define ATOMIC_CAS_STRONG(object, expected, desired) \
    atomic_compare_exchange_strong(object, expected, desired)
#define ATOMIC_CAS_STRONG_EXPLICIT(object, expected, desired, memory_order1, memory_order2) \
    atomic_compare_exchange_strong_explicit(object, expected, desired, memory_order1, memory_order2)

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
#define ATOMIC(type)                               std::atomic<type>

#define ATOMIC_LOAD(object)                        std::atomic_load(object)
#define ATOMIC_LOAD_EXPLICIT(object, memory_order) std::atomic_load_explicit(object, memory_order)

#define ATOMIC_STORE(object, desired)              std::atomic_store(object, desired)
#define ATOMIC_STORE_EXPLICIT(object, desired, memory_order) \
    std::atomic_store_explicit(object, desired, memory_order)

#define ATOMIC_CAS_WEAK(object, expected, desired) \
    std::atomic_compare_exchange_weak(object, expected, desired)
#define ATOMIC_CAS_WEAK_EXPLICIT(object, expected, desired, memory_order1, memory_order2) \
    std::atomic_compare_exchange_weak_explicit(                                           \
        object, expected, desired, memory_order1, memory_order2)

#define ATOMIC_CAS_STRONG(object, expected, desired) \
    std::atomic_compare_exchange_strong(object, expected, desired)
#define ATOMIC_CAS_STRONG_EXPLICIT(object, expected, desired, memory_order1, memory_order2) \
    std::atomic_compare_exchange_strong_explicit(                                           \
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
/* ---------------------------------- 原子操作 ---------------------------------- */

#endif // !MACRODEF_H
