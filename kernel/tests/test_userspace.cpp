#include "tests/test_userspace.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/kprintf.h"

void test_userspace_run(){
    __asm__ volatile("sti");
    static const char* const argv[] = {"init", "-t", nullptr};
    static const char* const envp[] = {"PATH=/bin:/sbin", "HOME=/root", "TERM=yokai", nullptr};
    uint64_t procs_before = process_count();
    uint64_t tasks_before = task_count();
    // init -t monteaza ce lipseste (/var si /opt ca tmpfs), porneste /bin/selftest si iese cu codul lui
    uint64_t pid = exec_spawn("/sbin/init", argv, envp);
    if(!pid){
        panic("test_userspace: /sbin/init nu a pornit");
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_userspace: init (pid %lu) a disparut fara exit", pid);
    }
    if(code != 0){
        panic("test_userspace: init -t a iesit cu %d (vezi liniile \"selftest: FAIL\")", code);
    }
    if(process_count() != procs_before){
        panic("test_userspace: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    kprintf("[OK] test_userspace (init -t -> /bin/selftest)\n");
}
