#include "tests/test_user.h"
#include "exec/elf.h"
#include "syscall/syscall.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

const int KILLED_SEGV = 128 + 11;
const int MAX_PROCS = 2;

const char* const envp[] = {"YOKAI=1", nullptr};

bool cpu_has_nx(){
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    return lo & (1u << 11);
}

// Porneste n procese odata si asteapta (task 0 nu se poate bloca: yield + hlt) sa fie eliberate; codurile de iesire
void run(const char* module, const char* const* argv, int n, int* codes){
    uint64_t tasks_before = task_count();
    uint64_t pids[MAX_PROCS];
    for(int i = 0; i < n; i++){
        pids[i] = exec_spawn_module(module, argv, envp);
        if(!pids[i]){
            panic("test_user: %s nu a pornit", module);
        }
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    for(int i = 0; i < n; i++){
        if(!process_exit_status(pids[i], codes[i])){
            panic("test_user: %s (pid %lu) a disparut fara exit", module, pids[i]);
        }
    }
}

}

void test_user_run(){
    __asm__ volatile("sti");

    // Doua procese in paralel: tick-ul le intrerupe in ring 3 si comuta intre spatiile lor de adrese
    static const char* const hello_argv[] = {"hello", nullptr};
    int codes[MAX_PROCS];
    run("hello", hello_argv, MAX_PROCS, codes);
    for(int i = 0; i < MAX_PROCS; i++){
        if(codes[i] != 0){
            panic("test_user: hello #%d a iesit cu %d (verificarea %d din hello.S a esuat)", i, codes[i], codes[i]);
        }
    }

    // Dupa primele procese cache-urile slab sunt deja crescute: de aici memoria libera trebuie sa revina exact
    uint64_t free_before = pmm_free_count();

    // Fiecare exceptie din ring 3 omoara doar procesul; kernelul merge mai departe
    const char* modes = cpu_has_nx() ? "pkgwx" : "pkgw";
    for(const char* m = modes; *m; m++){
        char arg[2] = {*m, 0};
        const char* const fault_argv[] = {"fault", arg, nullptr};
        run("fault", fault_argv, 1, codes);
        if(codes[0] != KILLED_SEGV){
            panic("test_user: fault %c a iesit cu %d, asteptat %d (omorat)", *m, codes[0], KILLED_SEGV);
        }
    }

    if(pmm_free_count() != free_before){
        panic("test_user: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    __asm__ volatile("cli");
    kprintf("[OK] test_user (2 x hello exit 0, fault %s omorate, fara pagini pierdute)\n", modes);
}
