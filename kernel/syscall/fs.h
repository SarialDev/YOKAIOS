#pragma once
#include "types.h"

// Syscall-urile de fisiere (fs.cpp), pe semnatura SyscallFn; legate in table.cpp
int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, uint64_t, uint64_t, uint64_t);
int64_t sys_stat(uint64_t path, uint64_t st, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_fstat(uint64_t fd, uint64_t st, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_lstat(uint64_t path, uint64_t st, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_lseek(uint64_t fd, uint64_t off, uint64_t whence, uint64_t, uint64_t, uint64_t);
int64_t sys_getcwd(uint64_t buf, uint64_t size, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_chdir(uint64_t path, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_rename(uint64_t oldpath, uint64_t newpath, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_rmdir(uint64_t path, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_unlink(uint64_t path, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_symlink(uint64_t target, uint64_t linkpath, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_readlink(uint64_t path, uint64_t buf, uint64_t size, uint64_t, uint64_t, uint64_t);
int64_t sys_mount(uint64_t source, uint64_t target, uint64_t fstype, uint64_t flags, uint64_t data, uint64_t);
int64_t sys_umount2(uint64_t target, uint64_t flags, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t, uint64_t, uint64_t);
int64_t sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t, uint64_t, uint64_t);
int64_t sys_chmod(uint64_t path, uint64_t mode, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_chown(uint64_t path, uint64_t uid, uint64_t gid, uint64_t, uint64_t, uint64_t);
int64_t sys_lchown(uint64_t path, uint64_t uid, uint64_t gid, uint64_t, uint64_t, uint64_t);
int64_t sys_fchown(uint64_t fd, uint64_t uid, uint64_t gid, uint64_t, uint64_t, uint64_t);
int64_t sys_access(uint64_t path, uint64_t mode, uint64_t, uint64_t, uint64_t, uint64_t);
