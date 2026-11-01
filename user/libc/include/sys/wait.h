#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include <sys/types.h>
#define WNOHANG 1
#define WIFEXITED(s) (((s) & 0xff) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0xff) != 0)
#define WTERMSIG(s) ((s) & 0x7f)
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
#endif
