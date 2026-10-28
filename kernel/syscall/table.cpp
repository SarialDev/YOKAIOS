#include "syscall/table.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "sched/sched.h"
#include "lib/kprintf.h"

namespace {

const uint64_t WRITE_CHUNK = 256;

int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len, uint64_t, uint64_t, uint64_t){
    if(fd != 1){
        return -EBADF;
    }
    if(!user_access_ok(buf, len, false)){
        return -EFAULT;
    }
    char chunk[WRITE_CHUNK];
    for(uint64_t done = 0; done < len;){
        uint64_t n = len - done < WRITE_CHUNK ? len - done : WRITE_CHUNK;
        if(!copy_from_user(chunk, buf + done, n)){
            return done ? (int64_t)done : -EFAULT;
        }
        for(uint64_t i = 0; i < n; i++){
            kputc(chunk[i]);
        }
        done += n;
    }
    return (int64_t)len;
}

int64_t sys_yield(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    yield();
    return 0;
}

int64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    return (int64_t)task_current()->pid;
}

int64_t sys_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    process_exit((int)code);
}

struct Table {
    SyscallFn fn[SYS_COUNT];
};

constexpr Table make_table(){
    Table t{};
    t.fn[SYS_WRITE] = sys_write;
    t.fn[SYS_YIELD] = sys_yield;
    t.fn[SYS_GETPID] = sys_getpid;
    t.fn[SYS_EXIT] = sys_exit;
    return t;
}

constexpr Table table = make_table();

}

SyscallFn syscall_lookup(uint64_t nr){
    return nr < SYS_COUNT ? table.fn[nr] : nullptr;
}
