#include "tests/test_ygfx.h"
#include "drivers/fb/fb.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

const char* const envp[] = {"YOKAI=1", "PATH=/bin:/sbin", nullptr};

// Porneste programul si asteapta sa fie eliberat; codul de iesire
int run(const char* path, const char* const* argv){
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn(path, argv, envp);
    if(!pid){
        panic("test_ygfx: %s nu a pornit", path);
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_ygfx: %s (pid %lu) a disparut fara exit", path, pid);
    }
    return code;
}

}

void test_ygfx_run(){
    __asm__ volatile("sti");
    if(!fb_ready()){
        panic("test_ygfx: nu exista framebuffer (cmdline nofb sau GRUB fara mod grafic)");
    }
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();

    static const char* const test_argv[] = {"ygfxtest", nullptr};
    int code = run("/bin/ygfxtest", test_argv);
    if(code != 0){
        panic("test_ygfx: ygfxtest a iesit cu %d (vezi liniile \"ygfxtest: FAIL\")", code);
    }
    if(fb_user_owned()){
        panic("test_ygfx: dupa ygfxtest ecranul a ramas in modul grafic");
    }

    static const char* const demo_argv[] = {"gfxdemo", "-t", "cycle", "-s", "0", nullptr};
    code = run("/bin/gfxdemo", demo_argv);
    if(code != 0){
        panic("test_ygfx: gfxdemo a iesit cu %d", code);
    }
    if(fb_user_owned()){
        panic("test_ygfx: dupa gfxdemo ecranul a ramas in modul grafic");
    }
    if(process_count() != procs_before){
        panic("test_ygfx: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    // Paginile video nu sunt RAM: nimic nu trebuie sa lipseasca (doar pagini de tabele / cache-uri mici)
    if(pmm_free_count() + 16 < free_before){
        panic("test_ygfx: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_ygfx (ygfx, /dev/fb0 mmap, teme Dark/Light)\n");
}
