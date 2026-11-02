#include "tests/test_perm.h"
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
        panic("test_perm: %s nu a pornit", path);
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_perm: %s (pid %lu) a disparut fara exit", path, pid);
    }
    return code;
}

}

void test_perm_run(){
    __asm__ volatile("sti");
    static const char* const argv[] = {"permtest", nullptr};
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    int code = run("/bin/permtest", argv);
    if(code != 0){
        panic("test_perm: permtest a iesit cu %d (vezi liniile \"permtest: FAIL\")", code);
    }
    // Copiii (utilizatorul 1000, procesul omorat) si-au dat inapoi paginile si structurile
    if(process_count() != procs_before){
        panic("test_perm: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 8 < free_before){
        panic("test_perm: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_perm (utilizatori, rwx, sticky, setuid, umask, kill)\n");
}
