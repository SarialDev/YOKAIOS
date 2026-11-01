#include "syscall/misc.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "uapi/fs.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "drivers/rtc.h"

namespace {

File* get(uint64_t fd){
    Process* p = process_current();
    return p ? fd_get(p->fds, (int64_t)fd) : nullptr;
}

// Cel mai mic fd >= min liber primeste o referinta noua la f
int64_t dup_from(File* f, uint64_t min){
    Process* p = process_current();
    if(min >= (uint64_t)PROC_MAX_FD){
        return -EINVAL;
    }
    for(int i = (int)min; i < PROC_MAX_FD; i++){
        if(!p->fds.fd[i]){
            file_get(f);
            p->fds.fd[i] = f;
            return i;
        }
    }
    return -EMFILE;
}

}

int64_t sys_dup(uint64_t fd, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    File* f = get(fd);
    return f ? dup_from(f, 0) : -EBADF;
}

int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t, uint64_t, uint64_t, uint64_t){
    File* f = get(oldfd);
    if(!f){
        return -EBADF;
    }
    if(newfd >= (uint64_t)PROC_MAX_FD){
        return -EBADF;
    }
    if(oldfd == newfd){
        return (int64_t)newfd;
    }
    Process* p = process_current();
    file_get(f);
    File* old = p->fds.fd[newfd];
    p->fds.fd[newfd] = f;
    if(old){
        file_put(old);
    }
    return (int64_t)newfd;
}

int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t, uint64_t, uint64_t){
    File* f = get(fd);
    if(!f){
        return -EBADF;
    }
    switch(cmd){
        case F_DUPFD: return dup_from(f, arg);
        case F_GETFD: return 0;                 // fara close-on-exec
        case F_SETFD: return 0;
        case F_GETFL: return (int64_t)f->flags;
        case F_SETFL:
            if(f != console_file()){
                const uint32_t mask = O_APPEND | O_NONBLOCK;
                f->flags = (f->flags & ~mask) | ((uint32_t)arg & mask);
            }
            return 0;
        default: return -EINVAL;
    }
}

int64_t sys_nanosleep(uint64_t req, uint64_t rem, uint64_t, uint64_t, uint64_t, uint64_t){
    Timespec ts;
    if(!copy_from_user(&ts, req, sizeof(ts))){
        return -EFAULT;
    }
    if(ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000){
        return -EINVAL;
    }
    uint64_t ns = (uint64_t)ts.sec * 1000000000UL + (uint64_t)ts.nsec;
    if(ns && !sched_sleep_until(monotonic_ns() + ns)){
        return -EINVAL;
    }
    if(rem){
        Timespec zero = {0, 0};
        copy_to_user(rem, &zero, sizeof(zero));
    }
    return 0;
}

int64_t sys_clock_gettime(uint64_t clk, uint64_t ts_user, uint64_t, uint64_t, uint64_t, uint64_t){
    Timespec ts;
    if(clk == CLOCK_MONOTONIC){
        uint64_t ns = monotonic_ns();
        ts.sec = (int64_t)(ns / 1000000000UL);
        ts.nsec = (int64_t)(ns % 1000000000UL);
    } else if(clk == CLOCK_REALTIME){
        uint64_t ms = wall_time_ms();
        ts.sec = (int64_t)(ms / 1000);
        ts.nsec = (int64_t)(ms % 1000) * 1000000;
    } else {
        return -EINVAL;
    }
    return copy_to_user(ts_user, &ts, sizeof(ts)) ? 0 : -EFAULT;
}

int64_t sys_gettimeofday(uint64_t tv_user, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    if(!tv_user){
        return 0;
    }
    uint64_t ms = wall_time_ms();
    int64_t tv[2] = {(int64_t)(ms / 1000), (int64_t)(ms % 1000) * 1000};
    return copy_to_user(tv_user, tv, sizeof(tv)) ? 0 : -EFAULT;
}

int64_t sys_time(uint64_t tloc, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    int64_t t = (int64_t)wall_time();
    if(tloc && !copy_to_user(tloc, &t, sizeof(t))){
        return -EFAULT;
    }
    return t;
}
