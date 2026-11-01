#include "tests/test_libc.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

const char* const envp[] = {"YOKAI=1", nullptr};

// Porneste programul si asteapta sa fie eliberat; codul de iesire
int run(const char* path, const char* const* argv){
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn(path, argv, envp);
    if(!pid){
        panic("test_libc: %s nu a pornit", path);
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_libc: %s (pid %lu) a disparut fara exit", path, pid);
    }
    return code;
}

}

void test_libc_run(){
    __asm__ volatile("sti");
    static const char* const hello_argv[] = {"hello", "unu", "doi", nullptr};
    int code = run("/bin/hello", hello_argv);
    if(code != 0){
        panic("test_libc: hello a iesit cu %d", code);
    }
    static const char* const t_argv[] = {"libctest", nullptr};
    uint64_t free_before = pmm_free_count();
    code = run("/bin/libctest", t_argv);
    if(code != 0){
        panic("test_libc: libctest a iesit cu %d (vezi liniile \"libctest: FAIL\")", code);
    }
    // Procesul a dat inapoi tot (brk, mmap, stiva): fara pagini pierdute; dir-urile/fisierele tmpfs au fost sterse
    if(pmm_free_count() + 8 < free_before){
        panic("test_libc: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_libc (hello 3 argumente, libctest)\n");
}
