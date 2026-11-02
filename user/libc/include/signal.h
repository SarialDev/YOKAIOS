#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/types.h>
/* Fara handlere: kill() doar termina procesul tinta (iesire 128 + semnal) */
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGUSR1 10
#define SIGUSR2 12
#define NSIG 32
int kill(pid_t pid, int sig);
#endif
