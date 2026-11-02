#include "tests/test_lua.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "drivers/tty.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

volatile uint64_t fp_pid;
volatile uint64_t last_fg;
volatile bool stop;

// Ca in test_vm: cand fputest pune un copil in prim-plan (tcsetpgrp), "tastam" Ctrl-C
void typist(void*){
    while(!stop){
        uint64_t fg = tty_get_pgrp();
        if(fp_pid && fg && fg != fp_pid && fg != last_fg){
            sched_sleep_until(monotonic_ns() + 100000000UL);
            tty_inject("\x03", 1);
            last_fg = fg;
        }
        sched_sleep_until(monotonic_ns() + 10000000UL);
    }
}

const char* const envp[] = {"YOKAI=1", "PATH=/bin:/sbin", nullptr};

// Porneste programul si asteapta sa fie eliberat; codul de iesire
int run(const char* path, const char* const* argv){
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn(path, argv, envp);
    if(!pid){
        panic("test_lua: %s nu a pornit", path);
    }
    fp_pid = pid;
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_lua: %s (pid %lu) a disparut fara exit", path, pid);
    }
    return code;
}

}

void test_lua_run(){
    __asm__ volatile("sti");
    static const char* const argv[] = {"lua", "/usr/share/lua/selftest.lua", nullptr};
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    static const char* const fargv[] = {"fputest", nullptr};
    Task* ty = task_create_kernel("typist", typist, nullptr);
    if(!ty){
        panic("test_lua: nu pot crea thread-ul typist");
    }
    sched_add(ty);
    uint64_t tasks_with_typist = task_count();
    int code = run("/bin/fputest", fargv);
    stop = true;
    while(task_count() >= tasks_with_typist){
        yield();
        __asm__ volatile("hlt");
    }
    tty_set_pgrp(0);
    if(code != 0){
        panic("test_lua: fputest a iesit cu %d (starea FPU/SSE nu e izolata corect)", code);
    }
    code = run("/bin/lua", argv);
    if(code != 0){
        panic("test_lua: lua a iesit cu %d (vezi liniile \"luatest: FAIL\")", code);
    }
    // Procesele omorate cu kill() si copiii lor si-au dat inapoi paginile si structurile
    if(process_count() != procs_before){
        panic("test_lua: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 8 < free_before){
        panic("test_lua: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_lua (Lua 5.4: tabele, string.format, io, os, popen)\n");
}
