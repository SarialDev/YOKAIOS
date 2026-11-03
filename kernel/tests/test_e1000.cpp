#include "tests/test_e1000.h"
#include "drivers/net/e1000.h"
#include "hal/net.h"
#include "core/panic.h"
#include "time/clock.h"
#include "sched/sched.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"

namespace {

#define CHECK(cond) do { if(!(cond)){ panic("test_e1000:%d: %s", __LINE__, #cond); } } while(0)

uint8_t my_mac[6];
volatile uint32_t cb_frames;
volatile uint32_t cb_arp_replies;
uint8_t cb_last[64];

// Cerere ARP "who-has 10.0.2.2 tell 10.0.2.15" (broadcast), 42 de octeti
size_t build_arp(uint8_t* f){
    memset(f, 0xFF, 6);
    memcpy(f + 6, my_mac, 6);
    f[12] = 0x08; f[13] = 0x06;
    f[14] = 0; f[15] = 1;           // Ethernet
    f[16] = 0x08; f[17] = 0;        // IPv4
    f[18] = 6; f[19] = 4;
    f[20] = 0; f[21] = 1;           // request
    memcpy(f + 22, my_mac, 6);
    f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15;
    memset(f + 32, 0, 6);
    f[38] = 10; f[39] = 0; f[40] = 2; f[41] = 2;
    return 42;
}

bool is_arp_reply_from_gw(const uint8_t* f, size_t n){
    return n >= 42 && f[12] == 0x08 && f[13] == 0x06 && f[20] == 0 && f[21] == 2 &&
           f[28] == 10 && f[29] == 0 && f[30] == 2 && f[31] == 2 &&
           memcmp(f + 32, my_mac, 6) == 0 && f[38] == 10 && f[41] == 15;
}

void on_rx(NetDevice*, const void* frame, size_t len, void*){
    const uint8_t* f = (const uint8_t*)frame;
    cb_frames = cb_frames + 1;
    if(is_arp_reply_from_gw(f, len)){
        memcpy(cb_last, f, len < sizeof(cb_last) ? len : sizeof(cb_last));
        cb_arp_replies = cb_arp_replies + 1;
    }
}

// La boot IF e oprit: il pornim cat dureaza asteptarea (ca in test_pci)
template <typename F>
bool wait_for(uint32_t ms, F pred){
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; sti" : "=r"(flags) : : "memory");
    uint64_t t0 = jiffies;
    bool ok = pred();
    while(!ok && jiffies - t0 < ms){
        yield();                    // task 0 cedeaza CPU-ul (task-ul de interogare)
        __asm__ volatile("hlt");
        ok = pred();
    }
    if(!(flags & (1UL << 9))){
        __asm__ volatile("cli" : : : "memory");
    }
    return ok;
}

NetDevice* nic;

void check_basic(){
    CHECK(e1000_count() == 1);
    nic = e1000_dev(0);
    CHECK(nic);
    bool found = false;
    for(int i = 0; i < hal_count(HalKind::Net); i++){
        if(hal_net(i) == nic){
            found = true;
        }
    }
    CHECK(found);
    nic->mac(my_mac);
    CHECK(my_mac[0] == 0x52 && my_mac[1] == 0x54 && my_mac[2] == 0x00);     // MAC implicit QEMU
    CHECK(!(my_mac[0] & 1));
    CHECK(nic->mtu() == 1500);
    CHECK(wait_for(3000, []{ return nic->link_up(); }));
    // Placa tocmai initializata: nimic nu a trecut inca prin ea (cu exceptia unor anunturi de la QEMU)
    NetStats s;
    nic->stats(s);
    CHECK(s.tx_packets == 0 && s.rx_errors == 0);
    // recv fara trafic = 0; send invalid = eroare
    uint8_t tmp[64];
    uint8_t big[1600];
    memset(big, 0, sizeof(big));
    CHECK(nic->send(big, 10) == -EINVAL);
    CHECK(nic->send(big, 1515) == -EINVAL);
    CHECK(nic->send(nullptr, 60) == -EINVAL);
    nic->stats(s);
    CHECK(s.tx_errors == 3 && s.tx_packets == 0);
    kprintf("[OK] test_e1000 basic\n");
    (void)tmp;
}

// Fara handler: raspunsul ARP al slirp apare in statistici si il scoate recv()
void check_arp_stats(){
    uint8_t f[64];
    size_t n = build_arp(f);
    NetStats s0, s1;
    nic->stats(s0);
    CHECK(nic->send(f, n) == 0);
    CHECK(wait_for(1000, [&]{ nic->stats(s1); return s1.rx_arp > s0.rx_arp; }));
    CHECK(s1.tx_packets == s0.tx_packets + 1);
    CHECK(s1.tx_bytes == s0.tx_bytes + 42);
    CHECK(s1.rx_packets >= s0.rx_packets + 1 && s1.rx_bytes >= s0.rx_bytes + 42);
    // Printre cadrele primite (QEMU poate trimite si altele) trebuie sa fie raspunsul gateway-ului
    uint8_t in[2048];
    bool got = false;
    for(int i = 0; i < 16 && !got; i++){
        int r = nic->recv(in, sizeof(in));
        if(r == 0){
            break;
        }
        CHECK(r > 0);
        got = is_arp_reply_from_gw(in, (size_t)r);
    }
    CHECK(got);
    kprintf("e1000: ARP primit: rx_arp=%lu rx_packets=%lu tx_packets=%lu\n", s1.rx_arp, s1.rx_packets, s1.tx_packets);
    kprintf("[OK] test_e1000 arp statistici\n");
}

// Cu handler: 700 de cereri (inelele de 256 se invart de cel putin doua ori), fiecare cu raspuns
void check_handler(){
    uint8_t f[64];
    size_t n = build_arp(f);
    cb_frames = 0;
    cb_arp_replies = 0;
    nic->set_rx_handler(on_rx, nullptr);
    NetStats s0, s1;
    nic->stats(s0);
    const uint32_t TOTAL = 700;
    uint32_t sent = 0;
    while(sent < TOTAL){
        uint32_t batch = TOTAL - sent < 50 ? TOTAL - sent : 50;
        for(uint32_t i = 0; i < batch; i++){
            int r;
            uint32_t tries = 0;
            while((r = nic->send(f, n)) == -EAGAIN && tries++ < 100000){
                e1000_poll(0);
            }
            CHECK(r == 0);
        }
        sent += batch;
        // Fara e1000_poll: raspunsurile trebuie sa vina de la intrerupere (MSI) sau din task-ul de interogare
        CHECK(wait_for(2000, [&]{ return cb_arp_replies >= sent; }));
    }
    nic->stats(s1);
    CHECK(cb_arp_replies == TOTAL);
    CHECK(s1.tx_packets == s0.tx_packets + TOTAL);
    CHECK(s1.rx_arp >= s0.rx_arp + TOTAL);
    CHECK(s1.rx_dropped == s0.rx_dropped && s1.rx_errors == 0);
    CHECK(is_arp_reply_from_gw(cb_last, sizeof(cb_last)));
    kprintf("e1000: %lu cereri ARP, %u raspunsuri la handler, rx_dropped=%lu, mod %s\n", (uint64_t)TOTAL, cb_arp_replies,
            s1.rx_dropped, e1000_irq_mode(0) == 1 ? "MSI" : "interogare");
    nic->set_rx_handler(nullptr, nullptr);
    kprintf("[OK] test_e1000 handler\n");
}

}

void test_e1000_run(){
    check_basic();
    check_arp_stats();
    check_handler();
    kprintf("[OK] test_e1000 (e1000)\n");
}
