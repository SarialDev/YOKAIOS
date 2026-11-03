#pragma once
// Doar pentru lwIP: kernelul nu are libc. Functiile sunt in lwip_port.cpp (mem* vin din lib/string.cpp)
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* memset(void*, int, size_t);
void* memcpy(void*, const void*, size_t);
void* memmove(void*, const void*, size_t);
int memcmp(const void*, const void*, size_t);
size_t strlen(const char*);
int strcmp(const char*, const char*);
int strncmp(const char*, const char*, size_t);
char* strncpy(char*, const char*, size_t);
char* strchr(const char*, int);
#ifdef __cplusplus
}
#endif
