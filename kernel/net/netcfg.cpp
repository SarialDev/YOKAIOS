// Configurarea retelei (T9.4): clientul DHCP al lwIP, serverele DNS si /run/resolv.conf.
// Callback-ul de stare ruleaza in thread-ul tcpip si doar noteaza noua configuratie (fara VFS acolo); un task de kernel "netcfg"
// scrie fisierul de indata ce /run e montat si il rescrie cand se schimba serverele (reinnoire de lease).
#include "net/netcfg.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "net/netcfg_fs.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/spinlock.h"
#include "time/clock.h"
#include "lib/string.h"
#include "lib/kprintf.h"

namespace {

Spinlock lock;
NetCfgInfo info;
uint32_t written_dns[2];        // ce s-a scris ultima data in resolv.conf
volatile bool task_started;
volatile bool dirty;            // configuratie noua de scris

void ntoa(uint32_t a, char* out){
    ip4_addr_t x;
    x.addr = a;
    // ip4addr_ntoa foloseste un buffer static (apelat doar din thread-ul tcpip si din netcfg; copiem imediat)
    const char* t = ip4addr_ntoa(&x);
    int i = 0;
    for(; t && t[i] && i < 15; i++){
        out[i] = t[i];
    }
    out[i] = 0;
}

void snapshot_from(struct netif* nif){
    NetCfgInfo n = {};
    uint64_t fl = lock.lock_irqsave();
    n.dhcp = info.dhcp;
    lock.unlock_irqrestore(fl);
    n.addr = ip4_addr_get_u32(netif_ip4_addr(nif));
    n.mask = ip4_addr_get_u32(netif_ip4_netmask(nif));
    n.gw = ip4_addr_get_u32(netif_ip4_gw(nif));
    n.bound = n.addr != 0;
    for(int i = 0; i < 2; i++){
        const ip_addr_t* d = dns_getserver((u8_t)i);
        n.dns[i] = d ? ip4_addr_get_u32(d) : 0;
    }
    fl = lock.lock_irqsave();
    bool changed = n.addr != info.addr || n.dns[0] != info.dns[0] || n.dns[1] != info.dns[1];
    n.resolv_written = info.resolv_written && !changed;
    info = n;
    if(changed){
        dirty = true;
    }
    lock.unlock_irqrestore(fl);
    if(changed && n.bound){
        char a[16], m[16], g[16], d0[16], d1[16];
        ntoa(n.addr, a);
        ntoa(n.mask, m);
        ntoa(n.gw, g);
        ntoa(n.dns[0], d0);
        ntoa(n.dns[1], d1);
        kprintf("net: eth0 %s mask %s gw %s%s\n", a, m, g, n.dhcp ? " (dhcp)" : "");
        if(n.dns[0]){
            kprintf("net: dns %s%s%s\n", d0, n.dns[1] ? " " : "", n.dns[1] ? d1 : "");
        }
    }
}

void status_cb(struct netif* nif){
    snapshot_from(nif);
}

int do_write(){
    NetCfgInfo n;
    netcfg_info(n);
    char buf[160];
    int len = 0;
    const char* hdr = "# YokaiOS: generat de netcfg\n";
    for(const char* p = hdr; *p; p++){
        buf[len++] = *p;
    }
    for(int i = 0; i < 2; i++){
        if(!n.dns[i]){
            continue;
        }
        char a[16];
        ntoa(n.dns[i], a);
        const char* k = "nameserver ";
        for(; *k; k++){
            buf[len++] = *k;
        }
        for(char* p = a; *p; p++){
            buf[len++] = *p;
        }
        buf[len++] = '\n';
    }
    int err = netcfg_fs_write("/run/resolv.conf", buf, (uint64_t)len);
    if(!err){
        uint64_t fl = lock.lock_irqsave();
        written_dns[0] = n.dns[0];
        written_dns[1] = n.dns[1];
        info.resolv_written = info.dns[0] == n.dns[0] && info.dns[1] == n.dns[1];
        lock.unlock_irqrestore(fl);
    }
    return err;
}

void netcfg_task(void*){
    for(;;){
        sleep_ms(20);
        if(!dirty){
            continue;
        }
        NetCfgInfo n;
        netcfg_info(n);
        if(!n.dns[0] && !n.dns[1]){
            dirty = false;
            continue;
        }
        // /run poate sa nu fie montat inca (DHCP e mai rapid decat montarile pseudo): incercam din nou
        if(!netcfg_fs_dir_exists("/run")){
            continue;
        }
        dirty = false;
        int err = do_write();
        if(err){
            kprintf("netcfg: /run/resolv.conf: eroare %d\n", err);
        } else {
            kprintf("netcfg: /run/resolv.conf scris\n");
        }
    }
}

}

void netcfg_attach(struct netif* nif, bool dhcp, uint32_t dns){
    info.dhcp = dhcp;
    if(!task_started){
        task_started = true;
        Task* t = task_create_kernel("netcfg", netcfg_task, nullptr);
        if(t){
            sched_add(t);
        } else {
            kprintf("netcfg: fara task\n");
        }
    }
    netif_set_status_callback(nif, status_cb);
    if(dhcp){
        err_t e = dhcp_start(nif);
        if(e != ERR_OK){
            kprintf("net: dhcp_start a esuat (%d)\n", (int)e);
        }
    } else {
        if(dns){
            ip_addr_t d;
            ip_addr_set_ip4_u32_val(d, dns);
            dns_setserver(0, &d);
        }
        snapshot_from(nif);
    }
}

void netcfg_info(NetCfgInfo& out){
    uint64_t fl = lock.lock_irqsave();
    out = info;
    lock.unlock_irqrestore(fl);
}

bool netcfg_bound(){
    NetCfgInfo n;
    netcfg_info(n);
    return n.bound;
}

int netcfg_write_resolv(){
    return do_write();
}
