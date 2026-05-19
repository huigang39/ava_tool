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

#endif // !PLATDEF_H
