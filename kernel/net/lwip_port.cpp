// Portul lwIP: functii de platforma (diagnostic, assert, rand, mini-libc pentru ce cere lwIP), cmdline ip=/gw=/mask=
// si pornirea thread-ului tcpip.
#include "net/lwip_port.h"
#include "net/netif.h"
#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "boot/multiboot2.h"
#include "hal/net.h"
#include "core/panic.h"
#include "sched/task.h"
#include "lib/entropy.h"
#include "lib/kprintf.h"
#include "lib/string.h"

extern "C" {

void lwip_port_diag(const char* fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void lwip_port_assert(const char* msg, const char* file, int line){
    panic("lwIP assert: %s (%s:%d)", msg, file, line);
}

unsigned int lwip_port_rand(void){
    return (unsigned int)entropy_u64();
}

int* lwip_port_errno(void){
    static int cells[64];
    return &cells[task_current()->tid & 63];
}

int atoi(const char* s){
    int sign = 1, v = 0;
    while(*s == ' ' || *s == '\t'){
        s++;
    }
    if(*s == '-' || *s == '+'){
        sign = *s++ == '-' ? -1 : 1;
    }
    while(*s >= '0' && *s <= '9'){
        v = v * 10 + (*s++ - '0');
    }
    return sign * v;
}

size_t strlen(const char* s){
    size_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

int strcmp(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n){
    while(n && *a && *a == *b){
        a++;
        b++;
        n--;
    }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char* strncpy(char* d, const char* s, size_t n){
    size_t i = 0;
    for(; i < n && s[i]; i++){
        d[i] = s[i];
    }
    for(; i < n; i++){
        d[i] = 0;
    }
    return d;
}

char* strchr(const char* s, int c){
    for(;; s++){
        if(*s == (char)c){
            return (char*)s;
        }
        if(!*s){
            return nullptr;
        }
    }
}

}

namespace {

volatile bool ready;
struct netif* the_netif;

struct Cfg {
    ip4_addr_t addr, mask, gw;
    NetDevice* dev;
};
Cfg cfg;

bool parse_ip(const char* key, ip4_addr_t& out, bool required){
    const char* v = cmdline_get(key);
    if(!v){
        return !required;
    }
    char buf[20];
    size_t n = 0;
    while(v[n] && v[n] != ' ' && n < sizeof(buf) - 1){
        buf[n] = v[n];
        n++;
    }
    buf[n] = 0;
    if(!ip4addr_aton(buf, &out)){
        kprintf("net: %s%s invalid\n", key, buf);
        return false;
    }
    return true;
}

// Ruleaza in thread-ul tcpip, inainte de bucla de mesaje
void tcpip_ready(void*){
    the_netif = yk_netif_create(cfg.dev, cfg.addr.addr, cfg.mask.addr, cfg.gw.addr);
    if(!the_netif){
        kprintf("net: netif_add a esuat\n");
        return;
    }
    ready = true;
    // ip4addr_ntoa foloseste un buffer static: un singur kprintf cu trei adrese ar suprascrie buffer-ul, deci copiem
    char a[16], m[16], g[16];
    strncpy(a, ip4addr_ntoa(&cfg.addr), sizeof(a));
    strncpy(m, ip4addr_ntoa(&cfg.mask), sizeof(m));
    strncpy(g, ip4addr_ntoa(&cfg.gw), sizeof(g));
    a[15] = m[15] = g[15] = 0;
    kprintf("net: eth0 %s mask %s gw %s\n", a, m, g);
}

}

bool net_init(){
    if(!cmdline_get("ip=")){
        return false;
    }
    if(hal_count(HalKind::Net) == 0){
        kprintf("net: ip= dat, dar nu exista placa de retea\n");
        return false;
    }
    IP4_ADDR(&cfg.mask, 255, 255, 255, 0);
    ip4_addr_set_zero(&cfg.gw);
    if(!parse_ip("ip=", cfg.addr, true) || !parse_ip("mask=", cfg.mask, false) || !parse_ip("gw=", cfg.gw, false)){
        return false;
    }
    // Prima placa fizica (eth0, eth1, ...)
    cfg.dev = nullptr;
    for(int i = 0; i < hal_count(HalKind::Net) && !cfg.dev; i++){
        const char* name = hal_name(HalKind::Net, i);
        if(name && name[0] == 'e' && name[1] == 't' && name[2] == 'h'){
            cfg.dev = hal_net(i);
        }
    }
    if(!cfg.dev){
        kprintf("net: nu exista placa fizica\n");
        return false;
    }
    tcpip_init(tcpip_ready, nullptr);
    return true;
}

bool net_ready(){
    return ready;
}

struct netif* net_netif(){
    return the_netif;
}
