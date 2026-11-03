#include "tests/test_ypkg.h"
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
        panic("test_ypkg: %s nu a pornit", path);
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_ypkg: %s (pid %lu) a disparut fara exit", path, pid);
    }
    return code;
}

}

void test_ypkg_run(){
    __asm__ volatile("sti");
    static const char* const argv[] = {"ypkgtest", nullptr};
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    int code = run("/bin/ypkgtest", argv);
    if(code != 0){
        panic("test_ypkg: ypkgtest a iesit cu %d (vezi liniile \"ypkgtest: FAIL\")", code);
    }
    // Procesele omorate cu kill() si copiii lor si-au dat inapoi paginile si structurile
    if(process_count() != procs_before){
        panic("test_ypkg: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    // 24 = marja de la inceput + tabelele de pagini (PD/PT) create la prima incarcare a unui modul de kernel (insmod din ypkgtest)
    if(pmm_free_count() + 24 < free_before){
        panic("test_ypkg: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_ypkg (ypkg: pachete .yokai)\n");
}
