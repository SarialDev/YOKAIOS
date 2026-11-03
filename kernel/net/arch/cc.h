#pragma once
// Portul lwIP pe YokaiOS x86-64 (freestanding): tipuri din <stdint.h>/<stddef.h> ale compilatorului, restul din kernel
#define LWIP_NO_INTTYPES_H  1
#define LWIP_NO_UNISTD_H    1
#define LWIP_NO_CTYPE_H     1

#define BYTE_ORDER          LITTLE_ENDIAN

#include <limits.h>
#ifndef SSIZE_MAX
#define SSIZE_MAX           LONG_MAX
#endif
typedef long ssize_t;

#define X8_F  "02x"
#define U16_F "hu"
#define S16_F "hd"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

#define PACK_STRUCT_FIELD(x)    x
#define PACK_STRUCT_STRUCT      __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

#ifdef __cplusplus
extern "C" {
#endif
void lwip_port_diag(const char* fmt, ...);
[[noreturn]] void lwip_port_assert(const char* msg, const char* file, int line);
unsigned int lwip_port_rand(void);
// errno de kernel pentru API-ul de socket-uri: o celula pe task (indexata dupa tid), ca threadurile sa nu se calce
int* lwip_port_errno(void);
#ifdef __cplusplus
}
#endif

#define LWIP_PLATFORM_DIAG(x)   do { lwip_port_diag x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) lwip_port_assert(x, __FILE__, __LINE__)
#define LWIP_RAND()             ((u32_t)lwip_port_rand())
#define errno                   (*lwip_port_errno())
