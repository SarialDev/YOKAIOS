#include "tests/test_ysh.h"
#include "drivers/tty.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

const uint32_t TIMEOUT_MS = 240000;
const char* const init_argv[] = {"init", "-1", nullptr};       // -1: se opreste cand ysh iese
const char* const init_envp[] = {"PATH=/bin:/sbin", "HOME=/root", "TERM=yokai", nullptr};

}

void test_ysh_run(){
    __asm__ volatile("sti");
    tty_activate();
    // Harness-ul asteapta markerul, apoi tasteaza scriptul in ysh si asteapta prompt-ul dupa fiecare comanda
    kprintf("ysh: astept intrare\n");
    uint64_t pid = exec_spawn("/sbin/init", init_argv, init_envp);
    if(!pid){
        panic("test_ysh: /sbin/init nu a pornit");
    }
    uint64_t start = uptime_ms();
    int code = -1;
    while(!process_exit_status(pid, code)){
        if(uptime_ms() - start > TIMEOUT_MS){
            panic("test_ysh: init nu s-a terminat in %u s", (unsigned)(TIMEOUT_MS / 1000));
        }
        sleep_ms(20);
    }
    if(code != 0){
        panic("test_ysh: init a iesit cu %d", code);
    }
    kprintf("[OK] test_ysh (init + ysh)\n");
}
