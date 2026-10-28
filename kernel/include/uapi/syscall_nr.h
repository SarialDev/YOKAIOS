#pragma once
/* Numerele de syscall, comune pentru kernel si programele user (C si asm).
 * ABI: rax = numar, argumente in rdi rsi rdx r10 r8 r9; rezultat in rax, erori ca -errno.
 * Numerele sunt cele de pe Linux x86-64. */

#define SYS_WRITE       1
#define SYS_YIELD       24
#define SYS_GETPID      39
#define SYS_EXIT        60

#define SYS_COUNT       64

/* Coduri de eroare (intoarse negativ) */
#define EBADF           9
#define EFAULT          14
#define EINVAL          22
#define ENOSYS          38
