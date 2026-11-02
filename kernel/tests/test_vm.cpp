#include "tests/test_vm.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "drivers/tty.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "time/clock.h"

namespace {

volatile uint64_t mm_pid;
volatile bool typed;
volatile bool stop;

// Thread separat (cu un CPU, testul ruleaza pe task-ul idle si nu ar primi CPU langa un copil care se invarte):
// cand mmtest pune un copil in prim-plan (tcsetpgrp), "tastam" Ctrl-C ca de la tastatura
void typist(void*){
    while(!stop){
        uint64_t fg = tty_get_pgrp();
        if(!typed && mm_pid && fg && fg != mm_pid){
            sched_sleep_until(monotonic_ns() + 100000000UL);
            tty_inject("\x03", 1);
            typed = true;
        }
        sched_sleep_until(monotonic_ns() + 10000000UL);
    }
}

const char* const envp[] = {"YOKAI=1", "PATH=/bin:/sbin", nullptr};

}

void test_vm_run(){
    __asm__ volatile("sti");
    static const char* const argv[] = {"mmtest", nullptr};
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    Task* ty = task_create_kernel("typist", typist, nullptr);
    if(!ty){
        panic("test_vm: nu pot crea thread-ul typist");
    }
    sched_add(ty);
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn("/bin/mmtest", argv, envp);
    if(!pid){
        panic("test_vm: /bin/mmtest nu a pornit");
    }
    mm_pid = pid;
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    stop = true;
    while(task_count() >= tasks_before){         // typist iese si e eliberat
        yield();
        __asm__ volatile("hlt");
    }
    tty_set_pgrp(0);
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_vm: mmtest (pid %lu) a disparut fara exit", pid);
    }
    if(code != 0){
        panic("test_vm: mmtest a iesit cu %d (vezi liniile \"mmtest: FAIL\")", code);
    }
    if(!typed){
        panic("test_vm: niciun copil in prim-plan pentru Ctrl-C");
    }
    if(process_count() != procs_before){
        panic("test_vm: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 8 < free_before){
        panic("test_vm: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_vm (mmap/munmap/mprotect/msync, fork CoW 64 MiB, semnale, Ctrl-C)\n");
}
