#pragma once
/* Numerele de syscall, comune pentru kernel si programele user (C si asm).
 * ABI: rax = numar, argumente in rdi rsi rdx r10 r8 r9; rezultat in rax, erori ca -errno.
 * Numerele sunt cele de pe Linux x86-64. */

#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_CLOSE       3
#define SYS_PIPE        22
#define SYS_YIELD       24
#define SYS_GETPID      39
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_EXIT        60
#define SYS_WAIT4       61
#define SYS_GETPPID     110

#define SYS_COUNT       128

/* Coduri de eroare (intoarse negativ) */
#define ENOENT          2
#define E2BIG           7
#define EBADF           9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EFAULT          14
#define EINVAL          22
#define EMFILE          24
#define EPIPE           32
#define ENOSYS          38

/* wait4: optiuni si codificarea starii (ca pe Linux) */
#define WNOHANG         1
/* status = (cod & 0xff) << 8 la exit normal, numarul semnalului daca procesul a fost omorat */
#define W_EXITCODE(code) (((code) & 0xff) << 8)
