#include "tests/test_netutils.h"
#include "net/lwip_port.h"
#include "boot/multiboot2.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

const char* const envp[] = {"YOKAI=1", "PATH=/bin:/sbin", nullptr};

// Valoarea numerica dupa cheia key= din cmdline, ca text ("0" daca lipseste)
void get_port(const char* key, char* out, unsigned cap){
    out[0] = '0';
    out[1] = 0;
    if(const char* v = cmdline_get(key)){
        unsigned i = 0;
        while(v[i] >= '0' && v[i] <= '9' && i < cap - 1){
            out[i] = v[i];
            i++;
        }
        out[i] = 0;
    }
}

}

void test_netutils_run(){
    __asm__ volatile("sti");
    uint64_t t0 = jiffies;
    while(!net_ready() && jiffies - t0 < 5000){
        yield();
        __asm__ volatile("hlt");
    }
    if(!net_ready()){
        panic("test_netutils: reteaua nu a pornit (lipseste ip= sau placa)");
    }
    char http[16], echo[16];
    get_port("nethttp=", http, sizeof(http));
    get_port("netecho=", echo, sizeof(echo));
    const char* const argv[] = {"netutiltest", http, echo, nullptr};
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn("/bin/netutiltest", argv, envp);
    if(!pid){
        panic("test_netutils: /bin/netutiltest nu a pornit");
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_netutils: nettest (pid %lu) a disparut fara exit", pid);
    }
    if(code != 0){
        panic("test_netutils: nettest a iesit cu %d (vezi liniile \"nettest: FAIL\")", code);
    }
    if(process_count() != procs_before){
        panic("test_netutils: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 8 < free_before){
        panic("test_netutils: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_netutils (ping, ifconfig, netstat, wget, nc)\n");
}
