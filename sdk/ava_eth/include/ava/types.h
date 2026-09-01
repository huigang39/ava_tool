#pragma once
#ifdef _KERNEL_MODE
#include <ntddk.h>
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned long long uint64_t;
#else
#include <stdint.h>
#endif
