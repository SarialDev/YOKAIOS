// /proc/net/tcp si /proc/net/udp (T9.5): citeste listele de PCB-uri lwIP in thread-ul tcpip (lwIP nu e thread-safe) si le scrie in
// formatul Linux, ca netstat din userland sa le poata analiza.
#include "net/netproc.h"
#include "net/lwip_port.h"
#include "lwip/tcpip.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/udp.h"
#include "lwip/ip4_addr.h"

namespace {

struct Out {
    char* p;
    uint64_t cap;
    uint64_t len;
};

void put(Out& o, const char* s){
    while(*s && o.len < o.cap){
        o.p[o.len++] = *s++;
    }
}

void put_dec(Out& o, uint32_t v, int width){
    char t[12];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while(v);
    for(int i = n; i < width && o.len < o.cap; i++){
        o.p[o.len++] = ' ';
    }
    while(n && o.len < o.cap){
        o.p[o.len++] = t[--n];
    }
}

void put_hex(Out& o, uint32_t v, int width){
    for(int i = width - 1; i >= 0 && o.len < o.cap; i--){
        o.p[o.len++] = "0123456789ABCDEF"[(v >> (i * 4)) & 15];
    }
}

struct Req {
    int kind;
    Out out;
};

// lwIP -> Linux (include/net/tcp_states.h)
int linux_state(enum tcp_state s){
    switch(s){
    case ESTABLISHED: return 1;
    case SYN_SENT: return 2;
    case SYN_RCVD: return 3;
    case FIN_WAIT_1: return 4;
    case FIN_WAIT_2: return 5;
    case TIME_WAIT: return 6;
    case CLOSED: return 7;
    case CLOSE_WAIT: return 8;
    case LAST_ACK: return 9;
    case LISTEN: return 10;
    case CLOSING: return 11;
    }
    return 7;
}

void line(Out& o, int sl, uint32_t la, uint16_t lp, uint32_t ra, uint16_t rp, int st, uint32_t txq, uint32_t rxq){
    put_dec(o, (uint32_t)sl, 4);
    put(o, ": ");
    put_hex(o, la, 8);
    put(o, ":");
    put_hex(o, lp, 4);
    put(o, " ");
    put_hex(o, ra, 8);
    put(o, ":");
    put_hex(o, rp, 4);
    put(o, " ");
    put_hex(o, (uint32_t)st, 2);
    put(o, " ");
    put_hex(o, txq, 8);
    put(o, ":");
    put_hex(o, rxq, 8);
    put(o, " 00:00000000 00000000     0        0 0\n");
}

void collect(void* arg){
    Req* r = (Req*)arg;
    Out& o = r->out;
    int sl = 0;
    if(r->kind == 0){
        for(struct tcp_pcb_listen* l = tcp_listen_pcbs.listen_pcbs; l; l = l->next){
            line(o, sl++, ip4_addr_get_u32(&l->local_ip), l->local_port, 0, 0, 10, 0, 0);
        }
        struct tcp_pcb* lists[2] = {tcp_active_pcbs, tcp_tw_pcbs};
        for(int k = 0; k < 2; k++){
            for(struct tcp_pcb* p = lists[k]; p; p = p->next){
                uint32_t txq = TCP_SND_BUF > p->snd_buf ? (uint32_t)(TCP_SND_BUF - p->snd_buf) : 0;
                line(o, sl++, ip4_addr_get_u32(&p->local_ip), p->local_port, ip4_addr_get_u32(&p->remote_ip),
                     p->remote_port, linux_state(p->state), txq, 0);
            }
        }
    } else {
        for(struct udp_pcb* p = udp_pcbs; p; p = p->next){
            bool conn = p->flags & UDP_FLAGS_CONNECTED;
            line(o, sl++, ip4_addr_get_u32(&p->local_ip), p->local_port, conn ? ip4_addr_get_u32(&p->remote_ip) : 0,
                 conn ? p->remote_port : 0, conn ? 1 : 7, 0, 0);
        }
    }
}

}

uint64_t netproc_sockets(int kind, char* buf, uint64_t cap){
    Req r;
    r.kind = kind;
    r.out.p = buf;
    r.out.cap = cap;
    r.out.len = 0;
    put(r.out, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    if(net_ready()){
        tcpip_callback_wait(collect, &r);
    }
    return r.out.len;
}
