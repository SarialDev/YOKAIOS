#include "tests/test_dhcp.h"
#include "net/lwip_port.h"
#include "net/netcfg.h"
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

void wait_for(bool (*cond)(), const char* what){
    uint64_t t0 = jiffies;
    while(!cond() && jiffies - t0 < 30000){
        yield();
        __asm__ volatile("hlt");
    }
    if(!cond()){
        panic("test_dhcp: timeout: %s", what);
    }
}

bool started(){
    return net_ready();
}

bool bound(){
    return netcfg_bound();
}

bool resolv_ready(){
    NetCfgInfo n;
    netcfg_info(n);
    return n.resolv_written;
}

}

void test_dhcp_run(){
    __asm__ volatile("sti");
    wait_for(started, "reteaua nu a pornit (net=dhcp / placa)");
    wait_for(bound, "DHCP n-a dat adresa");
    NetCfgInfo n;
    netcfg_info(n);
    // 10.0.2.15, masca 255.255.255.0, gateway 10.0.2.2, DNS 10.0.2.3 (slirp), ordinea retelei = octetul 0 cel mai mic
    if(!n.dhcp || n.addr != (10u | 0u << 8 | 2u << 16 | 15u << 24) || n.mask != (255u | 255u << 8 | 255u << 16) ||
       n.gw != (10u | 0u << 8 | 2u << 16 | 2u << 24) || n.dns[0] != (10u | 0u << 8 | 2u << 16 | 3u << 24)){
        panic("test_dhcp: configuratie neasteptata addr=%x mask=%x gw=%x dns=%x", n.addr, n.mask, n.gw, n.dns[0]);
    }
    kprintf("[OK] dhcp: 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3\n");
    wait_for(resolv_ready, "resolv.conf nu s-a scris");
    kprintf("[OK] dhcp: /run/resolv.conf\n");

    char port[16] = "0";
    if(const char* v = cmdline_get("dnshost=")){
        unsigned i = 0;
        while(v[i] >= '0' && v[i] <= '9' && i < sizeof(port) - 1){
            port[i] = v[i];
            i++;
        }
        port[i] = 0;
    }
    const char* const argv[] = {"dnstest", port, nullptr};
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn("/bin/dnstest", argv, envp);
    if(!pid){
        panic("test_dhcp: /bin/dnstest nu a pornit");
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_dhcp: dnstest (pid %lu) a disparut fara exit", pid);
    }
    if(code != 0){
        panic("test_dhcp: dnstest a iesit cu %d (vezi liniile \"dnstest: FAIL\")", code);
    }
    if(process_count() != procs_before){
        panic("test_dhcp: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 8 < free_before){
        panic("test_dhcp: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_dhcp (DHCP, DNS in libc, /proc/net/dev)\n");
}
