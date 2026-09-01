#ifndef PLATDEF_H
#define PLATDEF_H

#include <stdint.h>

#define HAPI static inline

#ifdef __cplusplus
extern "C" {
#endif

// 操作系统
#if defined(_WIN32) || defined(_WIN64)
#define OS_WIN 1
#else
#define OS_WIN 0
#endif

#if defined(__APPLE__) && defined(__MACH__)
#define OS_MAC 1
#else
#define OS_MAC 0
#endif

#if defined(__linux__)
#define OS_LINUX 1
#else
#define OS_LINUX 0
#endif

#if OS_LINUX || OS_MAC
#define OS_POSIX 1
#else
#define OS_POSIX 0
#endif

#if OS_WIN || OS_MAC || OS_LINUX
#define OS_HOSTED 1
#define OS_NONE 0
#else
#define OS_HOSTED 0
#define OS_NONE 1
#endif

// CPU 架构
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#define ARCH_X86_64 1
#else
#define ARCH_X86_64 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
#define ARCH_ARM64 1
#else
#define ARCH_ARM64 0
#endif

#if !ARCH_ARM64 && (defined(__arm__) || defined(_M_ARM) || defined(__ARM_ARCH))
#define ARCH_ARM32 1
#else
#define ARCH_ARM32 0
#endif

#if !ARCH_X86_64 && (defined(__i386__) || defined(_M_IX86))
#define ARCH_X86 1
#else
#define ARCH_X86 0
#endif

#if ARCH_X86_64 || ARCH_X86
#define ARCH_X86_FAMILY 1
#else
#define ARCH_X86_FAMILY 0
#endif

#if ARCH_ARM64 || ARCH_ARM32
#define ARCH_ARM_FAMILY 1
#else
#define ARCH_ARM_FAMILY 0
#endif

#if !(ARCH_X86_FAMILY || ARCH_ARM_FAMILY)
#define ARCH_UNKNOWN 1
#else
#define ARCH_UNKNOWN 0
#endif

// 编译器
#if defined(__clang__)
#define COMPILER_CLANG 1
#else
#define COMPILER_CLANG 0
#endif

#if defined(__GNUC__) && !COMPILER_CLANG
#define COMPILER_GCC 1
#else
#define COMPILER_GCC 0
#endif

#if defined(_MSC_VER)
#define COMPILER_MSVC 1
#else
#define COMPILER_MSVC 0
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
#define COMPILER_MINGW 1
#else
#define COMPILER_MINGW 0
#endif

// SIMD 能力
#if defined(__AVX2__)
#define HAS_AVX2 1
#else
#define HAS_AVX2 0
#endif

#if defined(__AVX__)
#define HAS_AVX 1
#else
#define HAS_AVX 0
#endif

#if defined(__SSE__) || ARCH_X86_64
#define HAS_SSE 1
#else
#define HAS_SSE 0
#endif

#if defined(__ARM_NEON) || ARCH_ARM64
#define HAS_NEON 1
#else
#define HAS_NEON 0
#endif

#define OS(FEATURE) OS_##FEATURE
#define ARCH(FEATURE) ARCH_##FEATURE
#define COMPILER(FEATURE) COMPILER_##FEATURE
#define HAS(FEATURE) HAS_##FEATURE

// 外部已定义浮点类型时, 同时定义对应的 FLOATxx_T_DEFINED 宏
#ifndef FLOAT32_T_DEFINED
typedef float float32_t;
#define FLOAT32_T_DEFINED 1
#endif

#ifndef FLOAT64_T_DEFINED
typedef double float64_t;
#define FLOAT64_T_DEFINED 1
#endif

// FLOAT16_NATIVE 为 1 表示可参与浮点运算, 为 0 表示仅保存 binary16 原始位模式
#ifndef FLOAT16_T_DEFINED

#if COMPILER_CLANG

/*
 * Clang 的 ARM NEON 头文件将 float16_t 定义为 __fp16, 这里必须使用相同的
 * 底层类型, 否则在后续包含 <arm_neon.h> 时会与 _Float16 别名冲突
 */
#if ARCH_ARM_FAMILY
typedef __fp16 float16_t;
#define FLOAT16_NATIVE 1
#elif ARCH_X86_FAMILY
typedef _Float16 float16_t;
#define FLOAT16_NATIVE 1
#else
// 未知架构使用 uint16_t 保存 binary16 原始位模式
typedef uint16_t float16_t;
#define FLOAT16_NATIVE 0
#endif

#elif COMPILER_GCC || COMPILER_MINGW

// GCC 在 x86/x86_64 和 AArch64 上优先使用 _Float16
#if ARCH_X86_FAMILY || ARCH_ARM64
typedef _Float16 float16_t;
#define FLOAT16_NATIVE 1

/*
 * 32 位 ARM GCC 通常使用 __fp16
 * 是否可用还取决于具体目标和编译选项
 */
#elif ARCH_ARM32
typedef __fp16 float16_t;
#define FLOAT16_NATIVE 1

#else
// 其他 GCC 目标回退为 16 位原始存储类型
typedef uint16_t float16_t;
#define FLOAT16_NATIVE 0
#endif

#elif COMPILER_MSVC

/*
 * MSVC 不假定存在通用的原生 IEEE-754 binary16 C 类型
 * 使用 uint16_t 保存 binary16 的原始位模式
 */
typedef uint16_t float16_t;
#define FLOAT16_NATIVE 0

#else

/*
 * 未识别的编译器采用保守策略:
 * 仅保证 float16_t 占用 16 bit, 不保证支持浮点运算
 */
typedef uint16_t float16_t;
#define FLOAT16_NATIVE 0

#endif

#define FLOAT16_T_DEFINED 1

#endif

#ifdef __cplusplus
}
#endif

#endif
