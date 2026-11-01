#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
#include <sys/types.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define _SC_PAGESIZE 30
#define _SC_CLK_TCK 2
extern char **environ;
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
int pipe(int fds[2]);
int dup(int fd);
int dup2(int old, int new_);
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
void _exit(int code) __attribute__((noreturn));
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int rmdir(const char *path);
int unlink(const char *path);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t n);
int access(const char *path, int mode);
int isatty(int fd);
unsigned sleep(unsigned sec);
int usleep(unsigned usec);
int sched_yield(void);
void *sbrk(long inc);
int brk(void *addr);
long sysconf(int name);
#endif
