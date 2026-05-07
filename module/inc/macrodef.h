#ifndef MACRODEF_H
#define MACRODEF_H

#include <stddef.h>
#include <string.h>

#include "errdef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIZE_128B    (128)
#define SIZE_256B    (256)

#define SIZE_1KB     (1024)
#define SIZE_2KB     (2 * SIZE_1KB)
#define SIZE_4KB     (4 * SIZE_1KB)
#define SIZE_8KB     (8 * SIZE_1KB)
#define SIZE_16KB    (16 * SIZE_1KB)
#define SIZE_32KB    (32 * SIZE_1KB)
#define SIZE_64KB    (64 * SIZE_1KB)
#define SIZE_128KB   (128 * SIZE_1KB)

#define SIZE_1MB     (SIZE_1KB * SIZE_1KB)
#define SIZE_2MB     (2 * SIZE_1MB)
#define SIZE_4MB     (4 * SIZE_1MB)
#define SIZE_16MB    (16 * SIZE_1MB)

#define AT(sec)      __attribute__((section(sec)))
#define OPTNONE      __attribute__((optnone))
#define ALIGN(align) __attribute__((aligned(align)))
#define FUNC_UNUSED  __attribute__((unused))
#define TYPEOF(var)  __typeof__(var)

#define ATOMIC_EXEC(code)                                 \
        do {                                              \
                volatile u32 __primask = __get_PRIMASK(); \
                __disable_irq();                          \
                {code};                                   \
                __set_PRIMASK(__primask);                 \
        } while (0)

#define HAPI            static inline

#define ARG_UNUSED(arg) (void)(arg)

#define ARG_CHECK(arg)                   \
        do {                             \
                if (!(arg))              \
                        return -MEINVAL; \
        } while (0)

#define CFG_INIT(ptr, config)          \
        do {                           \
                (ptr)->cfg = (config); \
        } while (0)

#define CFG_CHECK(ptr, f_init)                                                          \
        do {                                                                            \
                if (memcmp(&((ptr)->cfg), &((ptr)->lo.cfg), sizeof((ptr)->cfg)) != 0) { \
                        (ptr)->lo.cfg = (ptr)->cfg;                                     \
                        f_init((ptr), (ptr)->cfg);                                      \
                }                                                                       \
        } while (0)

#define RENAME(ptr, name)         \
        TYPEOF((ptr)) name = ptr; \
        ARG_UNUSED(name);

/**
 * @brief 检查函数指针非空
 *
 */
#define CHECK_FUNC_PTR(func_ptr)                             ((func_ptr) != NULL)

#define GET_MACRO(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME

#define DECL_1(ptr, a1)                     \
        TYPEOF((ptr)->a1) *a1 = &(ptr)->a1; \
        ARG_UNUSED(a1);
#define DECL_2(ptr, a1, a2)                         DECL_1(ptr, a1) DECL_1(ptr, a2)
#define DECL_3(ptr, a1, a2, a3)                     DECL_2(ptr, a1, a2) DECL_1(ptr, a3)
#define DECL_4(ptr, a1, a2, a3, a4)                 DECL_3(ptr, a1, a2, a3) DECL_1(ptr, a4)
#define DECL_5(ptr, a1, a2, a3, a4, a5)             DECL_4(ptr, a1, a2, a3, a4) DECL_1(ptr, a5)
#define DECL_6(ptr, a1, a2, a3, a4, a5, a6)         DECL_5(ptr, a1, a2, a3, a4, a5) DECL_1(ptr, a6)
#define DECL_7(ptr, a1, a2, a3, a4, a5, a6, a7)     DECL_6(ptr, a1, a2, a3, a4, a5, a6) DECL_1(ptr, a7)
#define DECL_8(ptr, a1, a2, a3, a4, a5, a6, a7, a8) DECL_7(ptr, a1, a2, a3, a4, a5, a6, a7) DECL_1(ptr, a8)

#define DECL(ptr, ...)                                                                              \
        GET_MACRO(__VA_ARGS__, DECL_8, DECL_7, DECL_6, DECL_5, DECL_4, DECL_3, DECL_2, DECL_1, ...) \
        (ptr, __VA_ARGS__)

#define RESET_1(ptr, a1)                             memset(&(ptr)->a1, 0, sizeof((ptr)->a1));
#define RESET_2(ptr, a1, a2)                         RESET_1(ptr, a1) RESET_1(ptr, a2)
#define RESET_3(ptr, a1, a2, a3)                     RESET_2(ptr, a1, a2) RESET_1(ptr, a3)
#define RESET_4(ptr, a1, a2, a3, a4)                 RESET_3(ptr, a1, a2, a3) RESET_1(ptr, a4)
#define RESET_5(ptr, a1, a2, a3, a4, a5)             RESET_4(ptr, a1, a2, a3, a4) RESET_1(ptr, a5)
#define RESET_6(ptr, a1, a2, a3, a4, a5, a6)         RESET_5(ptr, a1, a2, a3, a4, a5) RESET_1(ptr, a6)
#define RESET_7(ptr, a1, a2, a3, a4, a5, a6, a7)     RESET_6(ptr, a1, a2, a3, a4, a5, a6) RESET_1(ptr, a7)
#define RESET_8(ptr, a1, a2, a3, a4, a5, a6, a7, a8) RESET_7(ptr, a1, a2, a3, a4, a5, a6, a7) RESET_1(ptr, a8)

#define RESET(ptr, ...)                                                                                     \
        GET_MACRO(__VA_ARGS__, RESET_8, RESET_7, RESET_6, RESET_5, RESET_4, RESET_3, RESET_2, RESET_1, ...) \
        (ptr, __VA_ARGS__)

#define SPINLOCK_BACKOFF_MIN (4)
#define SPINLOCK_BACKOFF_MAX (128)
#if defined(__x86_64__)
#define SPINLOCK_BACKOFF_HOOK __asm volatile("pause" ::: "memory")
#else
#define SPINLOCK_BACKOFF_HOOK
#endif
#define SPINLOCK_BACKOFF(cnt)                      \
        do {                                       \
                for (usize i = (cnt); i != 0; i--) \
                        SPINLOCK_BACKOFF_HOOK;     \
                if ((cnt) < SPINLOCK_BACKOFF_MAX)  \
                        (cnt) += (cnt);            \
        } while (0);

#define SPIN_LOCK(lock_ptr)                                                                                          \
        do {                                                                                                         \
                u8    __expected;                                                                                    \
                usize __backoff = SPINLOCK_BACKOFF_MIN;                                                              \
                for (;;) {                                                                                           \
                        __expected = FALSE;                                                                          \
                        if (ATOMIC_CAS_WEAK_EXPLICIT((lock_ptr), &__expected, TRUE, ATOMIC_ACQUIRE, ATOMIC_RELAXED)) \
                                break;                                                                               \
                        SPINLOCK_BACKOFF(__backoff);                                                                 \
                }                                                                                                    \
        } while (0)

#define SPIN_UNLOCK(lock_ptr) ATOMIC_STORE_EXPLICIT((lock_ptr), FALSE, ATOMIC_RELEASE)

#ifdef __cplusplus
#define IS_SAME_TYPE(a, b) std::is_same_v<decltype(a), decltype(b)>
#else
#define IS_SAME_TYPE(a, b) __builtin_types_compatible_p(TYPEOF(a), TYPEOF(b))
#endif

#ifdef __cplusplus
#define BUILD_BUG_ON_ZERO(e) ((sizeof(char[1 - 2 * !!(e)])) - 1)
#else
#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int : -!!(e); }))
#endif

#define MUST_BE_ARRAY(arr)              BUILD_BUG_ON_ZERO(IS_SAME_TYPE((arr), &(arr)[0]))
#define ARRAY_LEN(arr)                  (sizeof(arr) / sizeof(arr[0]) + MUST_BE_ARRAY(arr))

#define CONTAINER_OF(ptr, type, member) (type *)((char *)(ptr) - offsetof(type, member))

/* ---------------------------------- 原子操作 ---------------------------------- */
#ifndef __cplusplus
#include <stdatomic.h>
#define ATOMIC(type)                                         _Atomic(type)

#define ATOMIC_LOAD(object)                                  atomic_load(object)
#define ATOMIC_LOAD_EXPLICIT(object, memory_order)           atomic_load_explicit(object, memory_order)

#define ATOMIC_STORE(object, desired)                        atomic_store(object, desired)
#define ATOMIC_STORE_EXPLICIT(object, desired, memory_order) atomic_store_explicit(object, desired, memory_order)

#define ATOMIC_CAS_WEAK(object, expected, desired)           atomic_compare_exchange_weak(object, expected, desired)
#define ATOMIC_CAS_WEAK_EXPLICIT(object, expected, desired, memory_order1, memory_order2) \
        atomic_compare_exchange_weak_explicit(object, expected, desired, memory_order1, memory_order2)

#define ATOMIC_CAS_STRONG(object, expected, desired) atomic_compare_exchange_strong(object, expected, desired)
#define ATOMIC_CAS_STRONG_EXPLICIT(object, expected, desired, memory_order1, memory_order2) \
        atomic_compare_exchange_strong_explicit(object, expected, desired, memory_order1, memory_order2)

#define ATOMIC_EXCHANGE(object, desired)                         atomic_exchange(object, desired)
#define ATOMIC_EXCHANGE_EXPLICIT(object, desired, memory_order)  atomic_exchange_explicit(object, desired, memory_order)

#define ATOMIC_FETCH_ADD(object, operand)                        atomic_fetch_add(object, operand)
#define ATOMIC_FETCH_ADD_EXPLICIT(object, operand, memory_order) atomic_fetch_add_explicit(object, operand, memory_order)

#define ATOMIC_FETCH_SUB(object, operand)                        atomic_fetch_sub(object, operand)
#define ATOMIC_FETCH_SUB_EXPLICIT(object, operand, memory_order) atomic_fetch_sub_explicit(object, operand, memory_order)

#define ATOMIC_RELAXED                                           memory_order_relaxed
#define ATOMIC_CONSUME                                           memory_order_consume
#define ATOMIC_ACQUIRE                                           memory_order_acquire
#define ATOMIC_RELEASE                                           memory_order_release
#define ATOMIC_ACQ_REL                                           memory_order_acq_rel
#define ATOMIC_SEQ_CST                                           memory_order_seq_cst
#else
}
#include <atomic>
#define ATOMIC(type)                                         std::atomic<type>

#define ATOMIC_LOAD(object)                                  std::atomic_load(object)
#define ATOMIC_LOAD_EXPLICIT(object, memory_order)           std::atomic_load_explicit(object, memory_order)

#define ATOMIC_STORE(object, desired)                        std::atomic_store(object, desired)
#define ATOMIC_STORE_EXPLICIT(object, desired, memory_order) std::atomic_store_explicit(object, desired, memory_order)

#define ATOMIC_CAS_WEAK(object, expected, desired)           std::atomic_compare_exchange_weak(object, expected, desired)
#define ATOMIC_CAS_WEAK_EXPLICIT(object, expected, desired, memory_order1, memory_order2) \
        std::atomic_compare_exchange_weak_explicit(object, expected, desired, memory_order1, memory_order2)

#define ATOMIC_CAS_STRONG(object, expected, desired) std::atomic_compare_exchange_strong(object, expected, desired)
#define ATOMIC_CAS_STRONG_EXPLICIT(object, expected, desired, memory_order1, memory_order2) \
        std::atomic_compare_exchange_strong_explicit(object, expected, desired, memory_order1, memory_order2)

#define ATOMIC_EXCHANGE(object, desired)                         std::atomic_exchange(object, desired)
#define ATOMIC_EXCHANGE_EXPLICIT(object, desired, memory_order)  std::atomic_exchange_explicit(object, desired, memory_order)

#define ATOMIC_FETCH_ADD(object, operand)                        std::atomic_fetch_add(object, operand)
#define ATOMIC_FETCH_ADD_EXPLICIT(object, operand, memory_order) std::atomic_fetch_add_explicit(object, operand, memory_order)

#define ATOMIC_FETCH_SUB(object, operand)                        std::atomic_fetch_sub(object, operand)
#define ATOMIC_FETCH_SUB_EXPLICIT(object, operand, memory_order) std::atomic_fetch_sub_explicit(object, operand, memory_order)

#define ATOMIC_RELAXED                                           std::memory_order_relaxed
#define ATOMIC_CONSUME                                           std::memory_order_consume
#define ATOMIC_ACQUIRE                                           std::memory_order_acquire
#define ATOMIC_RELEASE                                           std::memory_order_release
#define ATOMIC_ACQ_REL                                           std::memory_order_acq_rel
#define ATOMIC_SEQ_CST                                           std::memory_order_seq_cst
#endif
/* ---------------------------------- 原子操作 ---------------------------------- */

#endif // !MACRODEF_H
