#ifndef PLATDEF_H
#define PLATDEF_H

/* 操作系统识别 */
#if defined(_WIN32) || defined(_WIN64)
#define OS_WIN 1
#elif defined(__APPLE__) && defined(__MACH__)
#define OS_MAC 1
#elif defined(__linux__)
#define OS_LINUX 1
#else
#define OS_MCU 1
#endif

#if defined(OS_LINUX) || defined(OS_MAC)
#define OS_POSIX 1
#endif

#if defined(OS_LINUX) || defined(OS_MAC) || defined(OS_WIN)
#define OS_HOSTED 1
#endif

/* CPU 架构识别 */
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#define ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
#define ARCH_ARM64 1
#elif defined(__arm__) || defined(_M_ARM) || defined(__ARM_ARCH)
#define ARCH_ARM32 1
#elif defined(__i386__) || defined(_M_IX86)
#define ARCH_X86 1
#else
#define ARCH_UNKNOWN 1
#endif

#if defined(ARCH_X86_64) || defined(ARCH_X86)
#define ARCH_X86_FAMILY 1
#endif

#if defined(ARCH_ARM64) || defined(ARCH_ARM32)
#define ARCH_ARM_FAMILY 1
#endif

/* SIMD 能力探测 */
#if defined(__AVX2__)
#define HAS_AVX2 1
#endif
#if defined(__AVX__)
#define HAS_AVX 1
#endif
#if defined(__SSE__) || defined(ARCH_X86_64)
#define HAS_SSE 1
#endif
#if defined(__ARM_NEON) || defined(ARCH_ARM64)
#define HAS_NEON 1
#endif

#endif // !PLATDEF_H
