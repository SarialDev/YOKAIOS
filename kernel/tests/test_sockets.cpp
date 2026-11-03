#include "tests/test_sockets.h"
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

}

void test_sockets_run(){
    __asm__ volatile("sti");
    // Reteaua porneste in fundal (thread-ul tcpip): asteptam interfata
    uint64_t t0 = jiffies;
    while(!net_ready() && jiffies - t0 < 5000){
        yield();
        __asm__ volatile("hlt");
    }
    if(!net_ready()){
        panic("test_sockets: reteaua nu a pornit (lipseste ip= sau placa)");
    }
    char port[16] = "0";
    if(const char* v = cmdline_get("sockhost=")){
        unsigned i = 0;
        while(v[i] >= '0' && v[i] <= '9' && i < sizeof(port) - 1){
            port[i] = v[i];
            i++;
        }
        port[i] = 0;
    }
    const char* const argv[] = {"socktest", port, nullptr};
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn("/bin/socktest", argv, envp);
    if(!pid){
        panic("test_sockets: /bin/socktest nu a pornit");
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_sockets: socktest (pid %lu) a disparut fara exit", pid);
    }
    if(code != 0){
        panic("test_sockets: socktest a iesit cu %d (vezi liniile \"socktest: FAIL\")", code);
    }
    if(process_count() != procs_before){
        panic("test_sockets: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 8 < free_before){
        panic("test_sockets: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_sockets (socket-uri BSD: TCP/UDP, poll/select, EINTR)\n");
}
