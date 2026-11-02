#pragma once
#include "types.h"

// dup, dup2, fcntl, nanosleep, ceasuri (misc.cpp), legate in table.cpp
int64_t sys_dup(uint64_t fd, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t, uint64_t, uint64_t);
int64_t sys_nanosleep(uint64_t req, uint64_t rem, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_clock_gettime(uint64_t clk, uint64_t ts, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_gettimeofday(uint64_t tv, uint64_t tz, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_uname(uint64_t buf, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_time(uint64_t tloc, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
