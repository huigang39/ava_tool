#ifndef PLATDEF_H
#define PLATDEF_H

/* 操作系统 */
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
#define OS_NONE   0
#else
#define OS_HOSTED 0
#define OS_NONE   1
#endif

/* CPU 架构 */
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

/* 编译器 */
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

/* SIMD 能力 */
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

#define OS(FEATURE)       OS_##FEATURE
#define ARCH(FEATURE)     ARCH_##FEATURE
#define COMPILER(FEATURE) COMPILER_##FEATURE
#define HAS(FEATURE)      HAS_##FEATURE

/* ---------------------------------- 浮点类型 ---------------------------------- */

/*
 * float32_t / float64_t
 *
 * C/C++ 预处理器无法判断某个 typedef 是否已经存在,
 * 因此使用 *_DEFINED 宏避免与其他头文件中的同名类型重复定义.
 *
 * 如果外部已经定义了对应类型, 应同时定义:
 *
 *   FLOAT32_T_DEFINED
 *   FLOAT64_T_DEFINED
 *
 * 例如:
 *
 *   typedef float float32_t;
 *   #define FLOAT32_T_DEFINED 1
 */

#ifndef FLOAT32_T_DEFINED
typedef float float32_t;
#define FLOAT32_T_DEFINED 1
#endif

#ifndef FLOAT64_T_DEFINED
typedef double float64_t;
#define FLOAT64_T_DEFINED 1
#endif

/*
 * float16_t
 *
 * 表示 16 位浮点或 16 位浮点原始存储类型.
 *
 * C/C++ 预处理器同样无法判断 float16_t 是否已经被其他头文件 typedef,
 * 因此使用 FLOAT16_T_DEFINED 避免重复定义.
 *
 * 如果外部已经定义了 float16_t, 应同时定义:
 *
 *   FLOAT16_T_DEFINED
 *
 * FLOAT16_NATIVE 表示 float16_t 是否为编译器原生可参与浮点运算的类型:
 *
 *   1 : float16_t 为原生 16 位浮点类型, 例如 _Float16 或 __fp16
 *   0 : float16_t 实际为 uint16_t, 仅用于保存 IEEE-754 binary16 位模式,
 *       不能直接作为浮点数参与运算
 */

#ifndef FLOAT16_T_DEFINED

#if COMPILER_CLANG

/*
 * Clang 在主流 ARM、AArch64、x86 和 x86_64 目标上支持 _Float16
 */
#if ARCH_X86_FAMILY || ARCH_ARM_FAMILY
typedef _Float16 float16_t;
#define FLOAT16_NATIVE 1
#else
/*
 * 未知架构使用 uint16_t 保存 binary16 原始位模式
 */
typedef uint16_t float16_t;
#define FLOAT16_NATIVE 0
#endif

#elif COMPILER_GCC || COMPILER_MINGW

/*
 * GCC 在 x86/x86_64 和 AArch64 上优先使用 _Float16
 */
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
/*
 * 其他 GCC 目标回退为 16 位原始存储类型
 */
typedef uint16_t float16_t;
#define FLOAT16_NATIVE 0
#endif

#elif COMPILER_MSVC

/*
 * MSVC 的 C23 前端将 float16_t 作为保留的浮点类型标识符，但当前
 * 版本仍不允许把它用作 typedef 声明符。module 的半精度转换 API
 * 本身使用 uint16_t 传递 binary16 位模式，因此这里只声明能力标志，
 * 不再重复定义该保留名称。
 */
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

#endif // !PLATDEF_H
