#pragma once

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long               int64_t;
typedef uint64_t           uintptr_t;
typedef uint64_t           size_t;

static_assert(sizeof(uint64_t) == 8, "x86-64 only");

const uintptr_t KERNEL_VMA = 0xFFFFFFFF80000000UL;
