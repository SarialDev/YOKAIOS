#include "drivers/net/rtl8139.h"
#include "drivers/net/e1000.h"
#include "drivers/pci/pci.h"
#include "arch/x86_64/paging.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "sync/spinlock.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace {

// Registre (BAR1, 256 B)
const uint32_t R_IDR0 = 0x00;
const uint32_t R_TSD0 = 0x10;               // 4 x 32 biti
const uint32_t R_TSAD0 = 0x20;              // 4 x 32 biti
const uint32_t R_RBSTART = 0x30;
const uint32_t R_CR = 0x37;                 // 8 biti
const uint32_t R_CAPR = 0x38;               // 16 biti, = offset de citire - 16
const uint32_t R_IMR = 0x3C;                // 16 biti
const uint32_t R_ISR = 0x3E;                // 16 biti, write-1-to-clear
const uint32_t R_TCR = 0x40;
const uint32_t R_RCR = 0x44;
const uint32_t R_MPC = 0x4C;                // pachete pierdute (scrierea o sterge)
const uint32_t R_CONFIG1 = 0x52;            // 8 biti
const uint32_t R_MSR = 0x58;                // 8 biti

const uint8_t CR_BUFE = 1u << 0;
const uint8_t CR_TE = 1u << 2;
const uint8_t CR_RE = 1u << 3;
const uint8_t CR_RST = 1u << 4;

const uint32_t RCR_APM = 1u << 1;           // adresa fizica proprie
const uint32_t RCR_AM = 1u << 2;            // multicast
const uint32_t RCR_AB = 1u << 3;            // broadcast
const uint32_t RCR_WRAP = 1u << 7;          // ultimul pachet continua liniar dupa capatul inelului
const uint32_t RCR_MXDMA_UNL = 7u << 8;
const uint32_t RCR_FTH_NONE = 7u << 13;     // fara prag de FIFO

const uint32_t TCR_IFG96 = 3u << 24;
const uint32_t TCR_MXDMA_1K = 6u << 8;

const uint16_t INT_ROK = 1u << 0;
const uint16_t INT_RER = 1u << 1;
const uint16_t INT_RXOVW = 1u << 4;
const uint16_t INT_PUN = 1u << 5;           // link schimbat
const uint16_t INT_FOVW = 1u << 6;

const uint32_t TSD_SIZE_MASK = 0x1FFF;
const uint32_t TSD_OWN = 1u << 13;
const uint32_t TSD_TUN = 1u << 14;
const uint32_t TSD_TOK = 1u << 15;
const uint32_t TSD_TABT = 1u << 30;
const uint32_t TSD_THRESH_256 = 8u << 16;   // prag de transmisie 8 x 32 B

const uint8_t MSR_LINKB = 1u << 2;          // 1 = fara link

const uint16_t RXH_ROK = 1u << 0;

const int MAX_NIC = 2;
const uint32_t N_TX = 4;
const uint32_t BUF_SIZE = 2048;
const uint32_t RX_RING = 8192;              // 8 KiB (RBLEN = 0); descriptorul de 16 B e in plus
const uint32_t RX_PAGES = 3;                // 8192 + 16 + 1536 (WRAP) = 9744 <= 12288
const uint32_t TX_PAGES = N_TX * BUF_SIZE / PAGE_SIZE;
const uint32_t ETH_HDR = 14;
const uint32_t ETH_MIN = 60;                // fara FCS
const uint32_t MAX_FRAME = 1536;            // un cadru valid fara FCS
const uint32_t QN = 32;                     // coada software pentru recv()
const uint64_t FOUR_GIB = 1ull << 32;

void delay_us(uint32_t us){
    uint64_t end = monotonic_ns() + (uint64_t)us * 1000;
    while(monotonic_ns() < end){
        __builtin_ia32_pause();
    }
}

// n pagini contigue sub 4 GiB, zero; false daca nu se poate
bool dma_alloc(uint32_t pages, uint64_t& pa, uint8_t*& va){
    uint64_t p = pmm_alloc_contig(pages);
    if(!p){
        return false;
    }
    if(p + (uint64_t)pages * PAGE_SIZE > FOUR_GIB){
        for(uint32_t i = 0; i < pages; i++){
            pmm_free_page(p + (uint64_t)i * PAGE_SIZE);
        }
        return false;
    }
    pa = p;
    va = (uint8_t*)phys_to_virt(p);
    memset(va, 0, (size_t)pages * PAGE_SIZE);
    return true;
}

class Rtl8139 final : public NetDevice {
public:
    Rtl8139(){}
    bool init(PciDevice* p, int index);

    void mac(uint8_t out[6]) const override { memcpy(out, mac_addr, 6); }
    uint32_t mtu() const override { return 1500; }
    bool link_up() override { return (rd8(R_MSR) & MSR_LINKB) == 0; }
    int send(const void* frame, size_t len) override;
    int recv(void* buf, size_t max) override;
    void set_rx_handler(NetRxHandler h, void* ctx) override;
    void stats(NetStats& out) override;

    void reap();
    void irq();
    int irq_mode() const { return mode; }

    uint8_t rd8(uint32_t off) const { return *(volatile uint8_t*)(regs + off); }
    uint16_t rd16(uint32_t off) const { return *(volatile uint16_t*)(regs + off); }
    uint32_t rd32(uint32_t off) const { return *(volatile uint32_t*)(regs + off); }
    void wr8(uint32_t off, uint8_t v){ *(volatile uint8_t*)(regs + off) = v; }
    void wr16(uint32_t off, uint16_t v){ *(volatile uint16_t*)(regs + off) = v; }
    void wr32(uint32_t off, uint32_t v){ *(volatile uint32_t*)(regs + off) = v; }

private:
    void rx_start_locked();
    void reclaim_tx_locked();
    void note_link_locked();

    PciDevice* pci = nullptr;
    volatile uint8_t* regs = nullptr;
    uint8_t mac_addr[6] = {};
    int mode = 0;                               // 0 interogare, 1 MSI

    Spinlock lock{"rtl8139"};
    NetStats st = {};
    bool reaping = false;
    bool last_link = false;
    bool poller = false;
    NetRxHandler handler = nullptr;
    void* handler_ctx = nullptr;

    uint8_t* rx_buf = nullptr;
    uint64_t rx_pa = 0;
    uint32_t rx_off = 0;                        // offsetul urmatorului pachet necitit in inel

    uint8_t* q_buf = nullptr;                   // QN x MAX_FRAME
    uint16_t q_len[QN] = {};
    uint32_t q_head = 0, q_count = 0;

    uint8_t* tx_buf = nullptr;
    uint64_t tx_pa = 0;
    uint32_t tx_head = 0;                       // urmatorul descriptor liber
    uint32_t tx_tail = 0;                       // cel mai vechi descriptor in zbor
    uint32_t tx_used = 0;
};

Rtl8139* nics[MAX_NIC];
int nic_count;
const char* const NIC_NAMES[4] = {"eth0", "eth1", "eth2", "eth3"};

// RBSTART, RCR, CR: pornirea (sau repornirea dupa eroare) a receptorului; inelul pleaca gol
void Rtl8139::rx_start_locked(){
    rx_off = 0;
    wr32(R_RBSTART, (uint32_t)rx_pa);
    wr8(R_CR, CR_RE | CR_TE);
    wr32(R_RCR, RCR_APM | RCR_AM | RCR_AB | RCR_WRAP | RCR_MXDMA_UNL | RCR_FTH_NONE);
    wr16(R_CAPR, (uint16_t)(0 - 16));
}

bool Rtl8139::init(PciDevice* p, int index){
    pci = p;
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_MASTER);
    regs = (volatile uint8_t*)pci_map_bar(p, 1);
    if(!regs){
        kprintf("rtl8139: BAR1 (memorie) lipseste\n");
        return false;
    }

    // Alimentare + reset software (CR.RST se sterge singur)
    wr8(R_CONFIG1, 0);
    wr16(R_IMR, 0);
    wr8(R_CR, CR_RST);
    uint64_t end = monotonic_ns() + 200000000;
    while(rd8(R_CR) & CR_RST){
        if(monotonic_ns() > end){
            kprintf("rtl8139: reset-ul nu se termina\n");
            return false;
        }
        delay_us(10);
    }
    for(int i = 0; i < 6; i++){
        mac_addr[i] = rd8(R_IDR0 + (uint32_t)i);
    }
    if((mac_addr[0] & 1) || !(mac_addr[0] | mac_addr[1] | mac_addr[2] | mac_addr[3] | mac_addr[4] | mac_addr[5])){
        kprintf("rtl8139: MAC invalid\n");
        return false;
    }

    uint8_t* va = nullptr;
    if(!dma_alloc(RX_PAGES, rx_pa, rx_buf)){
        kprintf("rtl8139: fara memorie DMA (inel RX)\n");
        return false;
    }
    if(!dma_alloc(TX_PAGES, tx_pa, tx_buf)){
        kprintf("rtl8139: fara memorie DMA (buffere TX)\n");
        return false;
    }
    (void)va;
    q_buf = (uint8_t*)kzalloc((size_t)QN * MAX_FRAME);
    if(!q_buf){
        kprintf("rtl8139: fara memorie (coada RX)\n");
        return false;
    }

    nics[index] = this;
    static const InterruptHandler fns[MAX_NIC] = {
        [](InterruptFrame*){ if(nics[0]) nics[0]->irq(); },
        [](InterruptFrame*){ if(nics[1]) nics[1]->irq(); },
    };
    mode = 0;
    if(pci_msi_enable(p, fns[index]) >= 0){
        mode = 1;
    }

    wr32(R_MPC, 0);
    wr32(R_TCR, TCR_IFG96 | TCR_MXDMA_1K);
    uint64_t fl = lock.lock_irqsave();
    rx_start_locked();
    last_link = (rd8(R_MSR) & MSR_LINKB) == 0;
    lock.unlock_irqrestore(fl);
    wr16(R_ISR, 0xFFFF);
    if(mode == 1){
        wr16(R_IMR, INT_ROK | INT_RER | INT_RXOVW | INT_PUN | INT_FOVW);
    }
    return true;
}

// ---- receptie ----

void Rtl8139::note_link_locked(){
    bool up = (rd8(R_MSR) & MSR_LINKB) == 0;
    if(up != last_link){
        last_link = up;
        st.link_changes++;
    }
}

// Goleste inelul hardware (CR.BUFE): cu handler fiecare cadru merge la handler direct din inel; fara handler se copiaza
// in coada software (plina = pierdut). Un singur "reaper" odata (indicatorul reaping). Elibereaza si descriptorii TX
void Rtl8139::reap(){
    uint64_t fl = lock.lock_irqsave();
    if(reaping){
        lock.unlock_irqrestore(fl);
        return;
    }
    reaping = true;
    note_link_locked();
    uint16_t isr = rd16(R_ISR);
    if(isr){
        wr16(R_ISR, isr);                       // write-1-to-clear
    }
    st.rx_dropped += rd32(R_MPC);
    wr32(R_MPC, 0);
    reclaim_tx_locked();
    for(;;){
        if(rd8(R_CR) & CR_BUFE){
            break;
        }
        const uint8_t* hdr = rx_buf + rx_off;
        uint16_t status = (uint16_t)(hdr[0] | hdr[1] << 8);
        uint32_t len = (uint32_t)(hdr[2] | hdr[3] << 8);        // include FCS de 4 octeti
        if(len == 0xFFF0){
            break;                              // DMA-ul pachetului nu s-a terminat
        }
        if(len < ETH_HDR + 4 || len > MAX_FRAME + 4){
            // Antet fara sens: inelul s-a desincronizat, receptorul se reporneste
            st.rx_errors++;
            wr8(R_CR, CR_TE);
            rx_start_locked();
            continue;
        }
        uint32_t flen = len - 4;
        const uint8_t* fr = hdr + 4;
        bool good = (status & RXH_ROK) != 0;
        NetRxHandler h = handler;
        void* hc = handler_ctx;
        if(good){
            st.rx_packets++;
            st.rx_bytes += flen;
            if(fr[12] == 0x08 && fr[13] == 0x06){
                st.rx_arp++;
            }
            if(h){
                lock.unlock_irqrestore(fl);
                h(this, fr, flen, hc);          // fr ramane valid: CAPR nu a avansat
                fl = lock.lock_irqsave();
            } else if(q_count < QN){
                uint32_t slot = (q_head + q_count) % QN;
                memcpy(q_buf + (size_t)slot * MAX_FRAME, fr, flen);
                q_len[slot] = (uint16_t)flen;
                q_count++;
            } else {
                st.rx_dropped++;
            }
        } else {
            st.rx_errors++;
        }
        rx_off = (rx_off + len + 4 + 3) & ~3u;
        if(rx_off >= RX_RING){
            rx_off -= RX_RING;
        }
        wr16(R_CAPR, (uint16_t)(rx_off - 16));
    }
    reaping = false;
    lock.unlock_irqrestore(fl);
}

void Rtl8139::irq(){
    uint64_t fl = lock.lock_irqsave();
    st.irqs++;
    lock.unlock_irqrestore(fl);
    reap();                                     // citeste si sterge ISR
}

int Rtl8139::recv(void* buf, size_t max){
    if(!buf){
        return -EINVAL;
    }
    reap();
    uint64_t fl = lock.lock_irqsave();
    if(q_count == 0){
        lock.unlock_irqrestore(fl);
        return 0;
    }
    uint32_t n = q_len[q_head];
    if(n > max){
        n = (uint32_t)max;
    }
    memcpy(buf, q_buf + (size_t)q_head * MAX_FRAME, n);
    q_head = (q_head + 1) % QN;
    q_count--;
    lock.unlock_irqrestore(fl);
    return (int)n;
}

void Rtl8139::set_rx_handler(NetRxHandler h, void* ctx){
    for(;;){
        uint64_t fl = lock.lock_irqsave();
        if(!reaping){
            handler = h;
            handler_ctx = ctx;
            if(h){
                q_count = 0;                    // cadrele deja puse in coada se arunca
            }
            bool spawn = h && mode == 0 && !poller;
            if(spawn){
                poller = true;
            }
            lock.unlock_irqrestore(fl);
            if(spawn){
                Task* t = task_create_kernel("rtl8139p", [](void* a){
                    Rtl8139* d = (Rtl8139*)a;
                    for(;;){
                        sleep_ms(1);
                        d->reap();
                    }
                }, this);
                if(t){
                    sched_add(t);
                } else {
                    kprintf("rtl8139: fara task de interogare\n");
                }
            }
            return;
        }
        lock.unlock_irqrestore(fl);
        __builtin_ia32_pause();
    }
}

// ---- transmisie ----

void Rtl8139::reclaim_tx_locked(){
    while(tx_used > 0){
        uint32_t s = rd32(R_TSD0 + 4 * tx_tail);
        if(!(s & (TSD_TOK | TSD_TUN | TSD_TABT))){
            break;
        }
        if(!(s & TSD_TOK)){
            st.tx_errors++;
        }
        tx_tail = (tx_tail + 1) % N_TX;
        tx_used--;
    }
}

int Rtl8139::send(const void* frame, size_t len){
    if(!frame || len < ETH_HDR || len > ETH_HDR + 1500){
        uint64_t fl = lock.lock_irqsave();
        st.tx_errors++;
        lock.unlock_irqrestore(fl);
        return -EINVAL;
    }
    uint64_t fl = lock.lock_irqsave();
    reclaim_tx_locked();
    if(tx_used == N_TX){
        st.tx_dropped++;
        lock.unlock_irqrestore(fl);
        return -EAGAIN;
    }
    uint32_t i = tx_head;
    uint8_t* b = tx_buf + (size_t)i * BUF_SIZE;
    memcpy(b, frame, len);
    size_t n = len;
    if(n < ETH_MIN){
        memset(b + n, 0, ETH_MIN - n);
        n = ETH_MIN;
    }
    tx_head = (tx_head + 1) % N_TX;
    tx_used++;
    st.tx_packets++;
    st.tx_bytes += len;
    __sync_synchronize();
    wr32(R_TSAD0 + 4 * i, (uint32_t)(tx_pa + (uint64_t)i * BUF_SIZE));
    wr32(R_TSD0 + 4 * i, TSD_THRESH_256 | ((uint32_t)n & TSD_SIZE_MASK));   // OWN = 0: porneste DMA-ul
    lock.unlock_irqrestore(fl);
    return 0;
}

void Rtl8139::stats(NetStats& out){
    reap();
    uint64_t fl = lock.lock_irqsave();
    out = st;
    lock.unlock_irqrestore(fl);
}

// ---- PCI ----

int rtl8139_probe(PciDevice* p){
    if(nic_count >= MAX_NIC){
        return -1;
    }
    Rtl8139* d = new Rtl8139();
    int idx = nic_count;
    if(!d->init(p, idx)){
        nics[idx] = nullptr;
        return -1;
    }
    nic_count++;
    int name_idx = e1000_count() + idx;
    if(name_idx > 3){
        name_idx = 3;
    }
    hal_register(HalKind::Net, d, NIC_NAMES[name_idx]);
    uint8_t m[6];
    d->mac(m);
    kprintf("rtl8139: %s %02x:%02x:%02x:%02x:%02x:%02x, link %s, %s\n", NIC_NAMES[name_idx], m[0], m[1], m[2], m[3],
            m[4], m[5], d->link_up() ? "up" : "down", d->irq_mode() == 1 ? "MSI" : "interogare");
    return 0;
}

const PciDriver rtl8139_driver = {"rtl8139", 0x10EC, 0x8139, PCI_ANY, PCI_ANY, rtl8139_probe};

}

void rtl8139_init(){
    pci_register_driver(&rtl8139_driver);
}

int rtl8139_count(){
    return nic_count;
}

NetDevice* rtl8139_dev(int idx){
    return idx >= 0 && idx < nic_count ? nics[idx] : nullptr;
}

int rtl8139_irq_mode(int idx){
    return idx >= 0 && idx < nic_count ? nics[idx]->irq_mode() : 0;
}

void rtl8139_poll(int idx){
    if(idx >= 0 && idx < nic_count){
        nics[idx]->reap();
    }
}
