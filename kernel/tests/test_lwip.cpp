#include "tests/test_lwip.h"
#include "net/lwip_port.h"
#include "net/netif.h"
#include "boot/multiboot2.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4_addr.h"

namespace {

#define CHECK(cond) do { if(!(cond)){ panic("test_lwip:%d: %s", __LINE__, #cond); } } while(0)

const uint32_t TCP_TOTAL = 262144;
const uint16_t ECHO_PORT = 7;

volatile bool task_done;
bool host_ping_mode;

// Aceeasi formula in tests/lwip_host.py
uint8_t pat(uint32_t i){
    return (uint8_t)(i * 131 + (i >> 8) * 7 + 5);
}

// La boot IF e oprit: il pornim cat dureaza asteptarea (ca in test_e1000)
template <typename F>
bool wait_for(uint32_t ms, F pred){
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; sti" : "=r"(flags) : : "memory");
    uint64_t t0 = jiffies;
    bool ok = pred();
    while(!ok && jiffies - t0 < ms){
        yield();
        __asm__ volatile("hlt");
        ok = pred();
    }
    if(!(flags & (1UL << 9))){
        __asm__ volatile("cli" : : : "memory");
    }
    return ok;
}

void set_rcv_timeout(int s, int ms){
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    CHECK(lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
}

ip4_addr_t cmd_addr(const char* key){
    ip4_addr_t a;
    ip4_addr_set_zero(&a);
    const char* v = cmdline_get(key);
    char buf[20];
    size_t n = 0;
    while(v && v[n] && v[n] != ' ' && n < sizeof(buf) - 1){
        buf[n] = v[n];
        n++;
    }
    buf[n] = 0;
    ip4addr_aton(buf, &a);
    return a;
}

void check_netif(){
    struct netif* n = net_netif();
    CHECK(n);
    CHECK(netif_is_up(n) && netif_is_link_up(n));
    CHECK(netif_default == n);
    ip4_addr_t want = cmd_addr("ip=");
    CHECK(ip4_addr_cmp(netif_ip4_addr(n), &want));
    ip4_addr_t wmask = cmd_addr("mask=");
    if(cmdline_get("mask=")){
        CHECK(ip4_addr_cmp(netif_ip4_netmask(n), &wmask));
    }
    CHECK(n->hwaddr_len == 6 && n->hwaddr[0] == 0x52 && n->hwaddr[1] == 0x54);
    CHECK(n->mtu == 1500);
    kprintf("net: %s mac %02x:%02x:%02x:%02x:%02x:%02x\n", ip4addr_ntoa(netif_ip4_addr(n)), n->hwaddr[0], n->hwaddr[1],
            n->hwaddr[2], n->hwaddr[3], n->hwaddr[4], n->hwaddr[5]);
    kprintf("[OK] test_lwip netif\n");
}

// ICMP echo request: type 8, code 0, id, seq, 32 octeti de date
size_t build_echo(uint8_t* b, uint16_t id, uint16_t seq){
    const size_t len = 8 + 32;
    memset(b, 0, len);
    b[0] = 8;
    b[4] = (uint8_t)(id >> 8);
    b[5] = (uint8_t)id;
    b[6] = (uint8_t)(seq >> 8);
    b[7] = (uint8_t)seq;
    for(int i = 0; i < 32; i++){
        b[8 + i] = (uint8_t)(0x41 + i);
    }
    u16_t c = inet_chksum(b, (u16_t)len);
    memcpy(b + 2, &c, 2);
    return len;
}

// Raspunsul unui raw socket ICMP vine cu antetul IP in fata
const uint8_t* icmp_of(const uint8_t* pkt, int n, int* icmp_len){
    if(n < 20 || (pkt[0] >> 4) != 4){
        return nullptr;
    }
    int ihl = (pkt[0] & 15) * 4;
    if(ihl < 20 || n < ihl + 8){
        return nullptr;
    }
    *icmp_len = n - ihl;
    return pkt + ihl;
}

void check_ping_gateway(){
    ip4_addr_t gw = cmd_addr("gw=");
    CHECK(!ip4_addr_isany_val(gw));
    int s = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    CHECK(s >= 0);
    set_rcv_timeout(s, 1500);
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = gw.addr;
    bool ok = false;
    uint64_t t0 = monotonic_ns();
    // Prima incercare poate pierde cererea ARP (lwIP tine un singur pachet in asteptare): reincercam
    for(uint16_t seq = 1; seq <= 5 && !ok; seq++){
        uint8_t out[64];
        size_t n = build_echo(out, 0x4B41, seq);
        CHECK(lwip_sendto(s, out, n, 0, (struct sockaddr*)&to, sizeof(to)) == (ssize_t)n);
        for(int tries = 0; tries < 4 && !ok; tries++){
            uint8_t in[256];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int r = lwip_recvfrom(s, in, sizeof(in), 0, (struct sockaddr*)&from, &fl);
            if(r < 0){
                break;                              // timeout: urmatoarea cerere
            }
            int il = 0;
            const uint8_t* ic = icmp_of(in, r, &il);
            if(ic && ic[0] == 0 && ic[1] == 0 && ic[4] == 0x4B && ic[5] == 0x41 && ic[7] == seq && il == (int)n &&
               from.sin_addr.s_addr == gw.addr && memcmp(ic + 8, out + 8, 32) == 0 && inet_chksum(ic, (u16_t)il) == 0){
                ok = true;
            }
        }
    }
    CHECK(ok);
    kprintf("lwip: echo reply de la %s in %lu ms\n", ip4addr_ntoa(&gw), (monotonic_ns() - t0) / 1000000UL);
    CHECK(lwip_close(s) == 0);
    kprintf("[OK] test_lwip ping gateway\n");
}

// Citeste exact n octeti sau pana la EOF; intoarce cati a citit, negativ la eroare/timeout
void check_tcp_echo(int ls){
    set_rcv_timeout(ls, 30000);
    struct sockaddr_in peer;
    socklen_t pl = sizeof(peer);
    int c = lwip_accept(ls, (struct sockaddr*)&peer, &pl);
    CHECK(c >= 0);
    set_rcv_timeout(c, 20000);
    kprintf("lwip: tcp accept de la %s\n", ip4addr_ntoa((const ip4_addr_t*)&peer.sin_addr));
    static uint8_t buf[4096];
    uint32_t total = 0;
    uint64_t t0 = monotonic_ns();
    for(;;){
        int r = lwip_recv(c, buf, sizeof(buf), 0);
        CHECK(r >= 0);
        if(r == 0){
            break;                                  // gazda si-a inchis sensul de scriere
        }
        for(int i = 0; i < r; i++){
            CHECK(buf[i] == pat(total + (uint32_t)i));
        }
        total += (uint32_t)r;
        int off = 0;
        while(off < r){
            int w = lwip_send(c, buf + off, (size_t)(r - off), 0);
            CHECK(w > 0);
            off += w;
        }
    }
    CHECK(total == TCP_TOTAL);
    CHECK(lwip_close(c) == 0);
    kprintf("lwip: tcp echo %u octeti in %lu ms\n", total, (monotonic_ns() - t0) / 1000000UL);
    kprintf("[OK] test_lwip tcp echo\n");
}

void check_udp_echo(int us){
    set_rcv_timeout(us, 20000);
    static const uint32_t expect_len[3] = {9, 1000, 1400};
    for(int k = 0; k < 3; k++){
        static uint8_t buf[2048];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int r = lwip_recvfrom(us, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
        CHECK(r == (int)expect_len[k]);
        for(int i = 0; i < r; i++){
            CHECK(buf[i] == pat((uint32_t)(i + k * 17)));
        }
        CHECK(lwip_sendto(us, buf, (size_t)r, 0, (struct sockaddr*)&from, fl) == r);
    }
    kprintf("[OK] test_lwip udp echo\n");
}

void server_tests(){
    int ls = lwip_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(ls >= 0);
    int one = 1;
    lwip_setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = lwip_htons(ECHO_PORT);
    CHECK(lwip_bind(ls, (struct sockaddr*)&sa, sizeof(sa)) == 0);
    CHECK(lwip_listen(ls, 2) == 0);
    int us = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(us >= 0);
    CHECK(lwip_bind(us, (struct sockaddr*)&sa, sizeof(sa)) == 0);
    kprintf("lwip: astept host (tcp+udp pe portul %d)\n", ECHO_PORT);
    check_tcp_echo(ls);
    check_udp_echo(us);
    CHECK(lwip_close(us) == 0);
    CHECK(lwip_close(ls) == 0);
}

// Numara cererile ICMP echo venite de la peer; raspunsul il da lwIP (icmp_input), dupa raw_input
void host_ping_tests(){
    int s = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    CHECK(s >= 0);
    set_rcv_timeout(s, 30000);
    kprintf("lwip: astept host (ping)\n");
    int seen = 0;
    uint16_t last_seq = 0;
    while(seen < 3){
        uint8_t in[2048];
        int r = lwip_recv(s, in, sizeof(in), 0);
        if(r <= 0){
            kprintf("lwip: recv -> %d errno %d\n", r, errno);
        }
        CHECK(r > 0);
        int il = 0;
        const uint8_t* ic = icmp_of(in, r, &il);
        if(ic && ic[0] == 8 && ic[1] == 0 && inet_chksum(ic, (u16_t)il) == 0){
            uint16_t seq = (uint16_t)((ic[6] << 8) | ic[7]);
            CHECK(seen == 0 || seq == (uint16_t)(last_seq + 1));
            last_seq = seq;
            seen++;
            kprintf("lwip: echo request %d (id %x seq %u, %d octeti)\n", seen, (ic[4] << 8) | ic[5], seq, il - 8);
        }
    }
    // Lasam raspunsul ultimului sa plece inainte sa inchidem totul
    sys_msleep(300);
    CHECK(lwip_close(s) == 0);
    kprintf("[OK] test_lwip host ping\n");
}

void test_task(void*){
    check_netif();
    if(host_ping_mode){
        host_ping_tests();
    } else {
        check_ping_gateway();
        server_tests();
    }
    YkNetifStats st;
    yk_netif_stats(st);
    kprintf("lwip: rx_frames=%lu rx_nomem=%lu tx_frames=%lu tx_errors=%lu\n", st.rx_frames, st.rx_nomem, st.tx_frames,
            st.tx_errors);
    CHECK(st.rx_frames > 0 && st.tx_frames > 0 && st.rx_nomem == 0 && st.tx_errors == 0);
    kprintf("[OK] test_lwip stats\n");
    task_done = true;
}

}

void test_lwip_run(){
    const char* mode = cmdline_get("lwipmode=");
    host_ping_mode = mode && mode[0] == 'h';
    CHECK(wait_for(5000, []{ return net_ready(); }));
    task_done = false;
    sys_thread_t t = sys_thread_new("lwipt", test_task, nullptr, 0, 0);
    CHECK(t);
    CHECK(wait_for(120000, []{ return task_done; }));
    kprintf("[OK] test_lwip (lwip)\n");
}
