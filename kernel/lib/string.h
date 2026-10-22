#pragma once
#include "types.h"

// GCC poate genera singur apeluri la aceste functii, deci trebuie sa existe cu numele din C
extern "C" {
void* memset(void* dst, int value, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);
}
