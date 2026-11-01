#pragma once
/* Numerele de syscall, comune pentru kernel si programele user (C si asm).
 * ABI: rax = numar, argumente in rdi rsi rdx r10 r8 r9; rezultat in rax, erori ca -errno.
 * Numerele sunt cele de pe Linux x86-64. */

#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_STAT        4
#define SYS_FSTAT       5
#define SYS_LSTAT       6
#define SYS_LSEEK       8
#define SYS_MMAP        9
#define SYS_MUNMAP      11
#define SYS_BRK         12
#define SYS_PIPE        22
#define SYS_YIELD       24
#define SYS_DUP         32
#define SYS_DUP2        33
#define SYS_NANOSLEEP   35
#define SYS_GETPID      39
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_EXIT        60
#define SYS_WAIT4       61
#define SYS_GETTIMEOFDAY 96
#define SYS_GETPPID     110
#define SYS_FCNTL       72
#define SYS_GETCWD      79
#define SYS_CHDIR       80
#define SYS_RENAME      82
#define SYS_MKDIR       83
#define SYS_RMDIR       84
#define SYS_UNLINK      87
#define SYS_SYMLINK     88
#define SYS_READLINK    89
#define SYS_MOUNT       165
#define SYS_UMOUNT2     166
#define SYS_GETDENTS64  217
#define SYS_TIME        201
#define SYS_CLOCK_GETTIME 228

#define SYS_COUNT       256

/* Coduri de eroare (intoarse negativ) */
#define EPERM           1
#define ENOENT          2
#define EIO             5
#define ENOEXEC         8
#define E2BIG           7
#define EBADF           9
#define EINTR           4
#define ENXIO           6
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define EMFILE          24
#define EFBIG           27
#define ENOSPC          28
#define ENOTTY          25
#define ESPIPE          29
#define EROFS           30
#define EPIPE           32
#define EDOM            33
#define ERANGE          34
#define ENAMETOOLONG    36
#define ENOSYS          38
#define ENOTEMPTY       39
#define EMLINK          31
#define EOPNOTSUPP      95
#define ELOOP           40

/* wait4: optiuni si codificarea starii (ca pe Linux) */
#define WNOHANG         1
/* status = (cod & 0xff) << 8 la exit normal, numarul semnalului daca procesul a fost omorat */
#define W_EXITCODE(code) (((code) & 0xff) << 8)

/* mmap (doar anonim), fcntl, ceasuri */
#define PROT_READ       1
#define PROT_WRITE      2
#define PROT_EXEC       4
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
