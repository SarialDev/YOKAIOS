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
#define SYS_MPROTECT    10
#define SYS_MUNMAP      11
#define SYS_BRK         12
#define SYS_RT_SIGACTION 13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN 15
#define SYS_IOCTL       16
#define SYS_ACCESS      21
#define SYS_PIPE        22
#define SYS_YIELD       24
#define SYS_MSYNC       26
#define SYS_DUP         32
#define SYS_DUP2        33
#define SYS_PAUSE       34
#define SYS_NANOSLEEP   35
#define SYS_GETPID      39
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_EXIT        60
#define SYS_WAIT4       61
#define SYS_KILL        62
#define SYS_UNAME       63
#define SYS_CHMOD       90
#define SYS_FCHMOD      91
#define SYS_CHOWN       92
#define SYS_FCHOWN      93
#define SYS_LCHOWN      94
#define SYS_UMASK       95
#define SYS_GETTIMEOFDAY 96
#define SYS_GETUID      102
#define SYS_GETGID      104
#define SYS_SETUID      105
#define SYS_SETGID      106
#define SYS_GETEUID     107
#define SYS_GETEGID     108
#define SYS_SETPGID     109
#define SYS_GETPPID     110
#define SYS_GETPGRP     111
#define SYS_SETREUID    113
#define SYS_SETREGID    114
#define SYS_GETGROUPS   115
#define SYS_SETGROUPS   116
#define SYS_GETPGID     121
#define SYS_CAPGET      125
#define SYS_CAPSET      126
#define SYS_RT_SIGPENDING 127
#define SYS_RT_SIGSUSPEND 130
#define SYS_SCHED_SETSCHEDULER 144
#define SYS_SCHED_GETSCHEDULER 145
#define SYS_IOPERM      173
#define SYS_INIT_MODULE 175     /* init_module(imagine, lungime): modul de kernel semnat; cere CAP_SYS_ADMIN */
#define SYS_DELETE_MODULE 176   /* delete_module(nume) */
#define SYS_SIGVERIFY   250     /* sigverify(op, date, lungime, semnatura): vezi kernel/core/verify.h */
#define SYS_CRYPTCTL    251     /* cryptctl(op, arg): volume criptate; vezi kernel/block/crypt.h si uapi/crypt.h */
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
#define ESRCH           3
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
#define ENOKEY          126     /* nesemnat */
#define EKEYREJECTED    129     /* semnatura nu corespunde */

/* wait4: optiuni si codificarea starii (ca pe Linux) */
#define WNOHANG         1
/* status = (cod & 0xff) << 8 la exit normal, numarul semnalului daca procesul a fost omorat */
#define W_EXITCODE(code) (((code) & 0xff) << 8)

/* Semnale (numerele de pe Linux x86-64; 1..31, multimea de semnale = un uint64_t cu bitul n-1) */
#define SIGHUP          1
#define SIGINT          2
#define SIGQUIT         3
#define SIGILL          4
#define SIGTRAP         5
#define SIGABRT         6
#define SIGBUS          7
#define SIGFPE          8
#define SIGKILL         9
#define SIGUSR1         10
#define SIGSEGV         11
#define SIGUSR2         12
#define SIGPIPE         13
#define SIGALRM         14
#define SIGTERM         15
#define SIGCHLD         17
#define SIGCONT         18
#define SIGSTOP         19
#define SIGTSTP         20
#define SIGTTIN         21
#define SIGTTOU         22
#define SIGURG          23
#define SIGWINCH        28
#define SIGSYS          31
#define NSIG            32

/* sa_flags */
#define SA_NOCLDSTOP    0x00000001
#define SA_NOCLDWAIT    0x00000002
#define SA_SIGINFO      0x00000004
#define SA_RESTORER     0x04000000
#define SA_ONSTACK      0x08000000
#define SA_RESTART      0x10000000
#define SA_NODEFER      0x40000000
#define SA_RESETHAND    0x80000000
#define SIG_DFL         0
#define SIG_IGN         1
/* sigprocmask: how */
#define SIG_BLOCK       0
#define SIG_UNBLOCK     1
#define SIG_SETMASK     2
/* si_code */
#define SI_USER         0
#define SEGV_MAPERR     1
#define SEGV_ACCERR     2
#define FPE_INTDIV      1
#define ILL_ILLOPC      1
#define CLD_EXITED      1
#define CLD_KILLED      2

/* uname: structura ca pe Linux, sase campuri de 65 de octeti (struct utsname) */
#define UTS_FIELD       65

/* mmap, fcntl, ceasuri */
#define PROT_NONE       0
#define PROT_READ       1
#define PROT_WRITE      2
#define PROT_EXEC       4
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_NORESERVE   0x4000
#define MAP_POPULATE    0x8000
#define MS_ASYNC        1
#define MS_INVALIDATE   2
#define MS_SYNC         4
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
