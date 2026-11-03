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

// Registre (BAR0, 128 KiB)
const uint32_t R_CTRL = 0x0000;
const uint32_t R_STATUS = 0x0008;
const uint32_t R_EERD = 0x0014;
const uint32_t R_ICR = 0x00C0;
const uint32_t R_IMS = 0x00D0;
const uint32_t R_IMC = 0x00D8;
const uint32_t R_RCTL = 0x0100;
const uint32_t R_TCTL = 0x0400;
const uint32_t R_TIPG = 0x0410;
const uint32_t R_RDBAL = 0x2800;
const uint32_t R_RDBAH = 0x2804;
const uint32_t R_RDLEN = 0x2808;
const uint32_t R_RDH = 0x2810;
const uint32_t R_RDT = 0x2818;
const uint32_t R_TDBAL = 0x3800;
const uint32_t R_TDBAH = 0x3804;
const uint32_t R_TDLEN = 0x3808;
const uint32_t R_TDH = 0x3810;
const uint32_t R_TDT = 0x3818;
const uint32_t R_MPC = 0x4010;              // pachete pierdute (citire = stergere)
const uint32_t R_MTA = 0x5200;              // 128 registre
const uint32_t R_RAL0 = 0x5400;
const uint32_t R_RAH0 = 0x5404;

const uint32_t CTRL_ASDE = 1u << 5;
const uint32_t CTRL_SLU = 1u << 6;
const uint32_t CTRL_RST = 1u << 26;
const uint32_t CTRL_PHY_RST = 1u << 31;
const uint32_t CTRL_LRST = 1u << 3;
const uint32_t CTRL_ILOS = 1u << 7;
const uint32_t STATUS_LU = 1u << 1;
const uint32_t EERD_START = 1u << 0;
const uint32_t EERD_DONE = 1u << 4;
const uint32_t RCTL_EN = 1u << 1;
const uint32_t RCTL_BAM = 1u << 15;         // broadcast acceptat; BSIZE = 0 -> 2048 B
const uint32_t RCTL_SECRC = 1u << 26;       // CRC scos din lungime
const uint32_t TCTL_EN = 1u << 1;
const uint32_t TCTL_PSP = 1u << 3;
const uint32_t RAH_AV = 1u << 31;
const uint32_t INT_LSC = 1u << 2;
const uint32_t INT_RXDMT0 = 1u << 4;
const uint32_t INT_RXO = 1u << 6;
const uint32_t INT_RXT0 = 1u << 7;

const uint8_t RXD_DD = 1u << 0;
const uint8_t RXD_EOP = 1u << 1;
const uint8_t TXD_DD = 1u << 0;
const uint8_t TXC_EOP = 1u << 0;
const uint8_t TXC_IFCS = 1u << 1;
const uint8_t TXC_RS = 1u << 3;

const int MAX_NIC = 2;
const uint32_t N_RX = 256;
const uint32_t N_TX = 256;
const uint32_t BUF_SIZE = 2048;
const uint32_t RING_PAGES = 1;                              // 256 x 16 B
const uint32_t BUF_PAGES = N_RX * BUF_SIZE / PAGE_SIZE;     // 128 pagini = 512 KiB pe inel
const uint32_t ETH_HDR = 14;
const uint32_t ETH_MIN = 60;                                // fara FCS
const uint64_t FOUR_GIB = 1ull << 32;

struct RxDesc {
    uint64_t addr;
    uint16_t len;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
};
static_assert(sizeof(RxDesc) == 16, "RxDesc");

struct TxDesc {
    uint64_t addr;
    uint16_t len;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
};
static_assert(sizeof(TxDesc) == 16, "TxDesc");

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

class E1000 final : public NetDevice {
public:
    E1000(){}
    bool init(PciDevice* p, int index);

    void mac(uint8_t out[6]) const override { memcpy(out, mac_addr, 6); }
    uint32_t mtu() const override { return 1500; }
    bool link_up() override { return (rd(R_STATUS) & STATUS_LU) != 0; }
    int send(const void* frame, size_t len) override;
    int recv(void* buf, size_t max) override;
    void set_rx_handler(NetRxHandler h, void* ctx) override;
    void stats(NetStats& out) override;

    void reap();
    void irq();
    int irq_mode() const { return mode; }
    const char* mac_source() const { return mac_src; }

    uint32_t rd(uint32_t off) const { return *(volatile uint32_t*)(regs + off); }
    void wr(uint32_t off, uint32_t v){ *(volatile uint32_t*)(regs + off) = v; }

private:
    int eeprom_read(uint32_t word);
    bool read_mac();
    void release_locked();
    void reclaim_tx_locked();
    void note_link_locked();

    PciDevice* pci = nullptr;
    volatile uint8_t* regs = nullptr;
    uint8_t mac_addr[6] = {};
    int mode = 0;                               // 0 interogare, 1 MSI
    const char* mac_src = "";

    Spinlock lock{"e1000"};
    NetStats st = {};
    bool reaping = false;
    bool last_link = false;
    bool poller = false;
    NetRxHandler handler = nullptr;
    void* handler_ctx = nullptr;

    volatile RxDesc* rx = nullptr;
    uint8_t* rx_buf = nullptr;
    uint64_t rx_ring_pa = 0, rx_buf_pa = 0;
    uint16_t rx_len[N_RX] = {};
    uint32_t rx_scan = 0;                       // primul descriptor necitit de driver
    uint32_t rx_next = 0;                       // primul descriptor neeliberat (cadre numarate dar neconsumate: [rx_next, rx_scan))

    volatile TxDesc* tx = nullptr;
    uint8_t* tx_buf = nullptr;
    uint64_t tx_ring_pa = 0, tx_buf_pa = 0;
    uint32_t tx_tail = 0;
    uint32_t tx_clean = 0;
};

E1000* nics[MAX_NIC];
int nic_count;
const char* const NIC_NAMES[MAX_NIC] = {"eth0", "eth1"};

// ---- EEPROM / MAC ----

int E1000::eeprom_read(uint32_t word){
    wr(R_EERD, (word << 8) | EERD_START);
    uint64_t end = monotonic_ns() + 20000000;
    for(;;){
        uint32_t v = rd(R_EERD);
        if(v & EERD_DONE){
            return (int)(v >> 16);
        }
        if(monotonic_ns() > end){
            return -1;
        }
        __builtin_ia32_pause();
    }
}

bool mac_valid(const uint8_t* m){
    uint8_t any = 0, all = 0xFF;
    for(int i = 0; i < 6; i++){
        any |= m[i];
        all &= m[i];
    }
    return any != 0 && all != 0xFF && !(m[0] & 1);
}

bool E1000::read_mac(){
    int w[3];
    for(int i = 0; i < 3; i++){
        w[i] = eeprom_read((uint32_t)i);
    }
    if(w[0] >= 0 && w[1] >= 0 && w[2] >= 0){
        for(int i = 0; i < 3; i++){
            mac_addr[2 * i] = (uint8_t)w[i];
            mac_addr[2 * i + 1] = (uint8_t)(w[i] >> 8);
        }
        if(mac_valid(mac_addr)){
            mac_src = "EEPROM";
            return true;
        }
    }
    uint32_t lo = rd(R_RAL0), hi = rd(R_RAH0);
    for(int i = 0; i < 4; i++){
        mac_addr[i] = (uint8_t)(lo >> (8 * i));
    }
    mac_addr[4] = (uint8_t)hi;
    mac_addr[5] = (uint8_t)(hi >> 8);
    if(mac_valid(mac_addr)){
        mac_src = "RAL0";
        return true;
    }
    return false;
}

// ---- initializare ----

bool E1000::init(PciDevice* p, int index){
    pci = p;
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_MASTER);
    regs = (volatile uint8_t*)pci_map_bar(p, 0);
    if(!regs){
        kprintf("e1000: BAR0 lipseste\n");
        return false;
    }

    // Reset complet: opreste intreruperile si DMA, apoi CTRL.RST (se sterge singur)
    wr(R_IMC, 0xFFFFFFFF);
    wr(R_RCTL, 0);
    wr(R_TCTL, 0);
    wr(R_CTRL, rd(R_CTRL) | CTRL_RST);
    uint64_t end = monotonic_ns() + 200000000;
    while(rd(R_CTRL) & CTRL_RST){
        if(monotonic_ns() > end){
            kprintf("e1000: reset-ul nu se termina\n");
            return false;
        }
        delay_us(10);
    }
    delay_us(20000);                            // 10 ms dupa reset inainte de alte accese (hardware real)
    wr(R_IMC, 0xFFFFFFFF);
    (void)rd(R_ICR);

    // Link: set link up + autonegociere; fara ILOS / PHY_RST / LRST
    uint32_t ctrl = rd(R_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_PHY_RST | CTRL_LRST | CTRL_ILOS);
    wr(R_CTRL, ctrl);

    if(!read_mac()){
        kprintf("e1000: MAC invalid\n");
        return false;
    }
    uint32_t lo = mac_addr[0] | (uint32_t)mac_addr[1] << 8 | (uint32_t)mac_addr[2] << 16 | (uint32_t)mac_addr[3] << 24;
    uint32_t hi = mac_addr[4] | (uint32_t)mac_addr[5] << 8 | RAH_AV;
    wr(R_RAL0, lo);
    wr(R_RAH0, hi);
    for(uint32_t i = 0; i < 128; i++){
        wr(R_MTA + 4 * i, 0);
    }

    // Inele si buffere (DMA sub 4 GiB)
    uint8_t* va = nullptr;
    if(!dma_alloc(RING_PAGES, rx_ring_pa, va)){
        kprintf("e1000: fara memorie DMA (inel RX)\n");
        return false;
    }
    rx = (volatile RxDesc*)va;
    if(!dma_alloc(BUF_PAGES, rx_buf_pa, rx_buf)){
        kprintf("e1000: fara memorie DMA (buffere RX)\n");
        return false;
    }
    if(!dma_alloc(RING_PAGES, tx_ring_pa, va)){
        kprintf("e1000: fara memorie DMA (inel TX)\n");
        return false;
    }
    tx = (volatile TxDesc*)va;
    if(!dma_alloc(BUF_PAGES, tx_buf_pa, tx_buf)){
        kprintf("e1000: fara memorie DMA (buffere TX)\n");
        return false;
    }
    for(uint32_t i = 0; i < N_RX; i++){
        rx[i].addr = rx_buf_pa + (uint64_t)i * BUF_SIZE;
    }
    for(uint32_t i = 0; i < N_TX; i++){
        tx[i].addr = tx_buf_pa + (uint64_t)i * BUF_SIZE;
    }

    wr(R_RDBAL, (uint32_t)rx_ring_pa);
    wr(R_RDBAH, (uint32_t)(rx_ring_pa >> 32));
    wr(R_RDLEN, N_RX * sizeof(RxDesc));
    wr(R_RDH, 0);
    wr(R_RDT, N_RX - 1);                        // hardware-ul detine 0..N-2
    wr(R_TDBAL, (uint32_t)tx_ring_pa);
    wr(R_TDBAH, (uint32_t)(tx_ring_pa >> 32));
    wr(R_TDLEN, N_TX * sizeof(TxDesc));
    wr(R_TDH, 0);
    wr(R_TDT, 0);
    wr(R_TIPG, 10 | 8u << 10 | 6u << 20);       // valorile din manualul 8254x pentru full duplex

    // Intreruperi: MSI daca exista, altfel nimic mascat (interogare)
    mode = 0;
    nics[index] = this;
    static const InterruptHandler fns[MAX_NIC] = {
        [](InterruptFrame*){ if(nics[0]) nics[0]->irq(); },
        [](InterruptFrame*){ if(nics[1]) nics[1]->irq(); },
    };
    if(pci_msi_enable(p, fns[index]) >= 0){
        mode = 1;
    }

    wr(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    wr(R_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));
    last_link = link_up();
    if(mode == 1){
        wr(R_IMS, INT_RXT0 | INT_RXO | INT_RXDMT0 | INT_LSC);
    }
    return true;
}

// ---- recepție ----

// Cu lock-ul tinut: elibereaza descriptorul rx_next si il da inapoi hardware-ului
void E1000::release_locked(){
    uint32_t i = rx_next;
    rx[i].status = 0;
    rx[i].errors = 0;
    rx_len[i] = 0;
    rx_next = (rx_next + 1) % N_RX;
    __sync_synchronize();
    wr(R_RDT, i);
}

void E1000::note_link_locked(){
    bool up = (rd(R_STATUS) & STATUS_LU) != 0;
    if(up != last_link){
        last_link = up;
        st.link_changes++;
    }
}

// Numara cadrele terminate de hardware. Cu handler le da handlerului (in afara lock-ului) si le elibereaza; fara handler
// le lasa in inel pentru recv(). Un singur "reaper" odata (indicatorul reaping)
void E1000::reap(){
    {
        uint64_t fl = lock.lock_irqsave();
        if(reaping){
            lock.unlock_irqrestore(fl);
            return;
        }
        reaping = true;
        note_link_locked();
        st.rx_dropped += rd(R_MPC);
        lock.unlock_irqrestore(fl);
    }
    for(;;){
        uint64_t fl = lock.lock_irqsave();
        uint32_t i = rx_scan;
        uint8_t status = rx[i].status;
        if(!(status & RXD_DD)){
            reaping = false;
            lock.unlock_irqrestore(fl);
            return;
        }
        uint16_t len = rx[i].len;
        uint8_t errors = rx[i].errors;
        rx_scan = (rx_scan + 1) % N_RX;
        uint8_t* fr = rx_buf + (size_t)i * BUF_SIZE;
        bool good = (status & RXD_EOP) && !errors && len >= ETH_HDR && len <= BUF_SIZE;
        NetRxHandler h = handler;
        void* hc = handler_ctx;
        if(good){
            st.rx_packets++;
            st.rx_bytes += len;
            if(fr[12] == 0x08 && fr[13] == 0x06){
                st.rx_arp++;
            }
            rx_len[i] = len;
        } else {
            st.rx_errors++;
            rx_len[i] = 0;
        }
        if(good && h){
            lock.unlock_irqrestore(fl);
            h(this, fr, len, hc);
            fl = lock.lock_irqsave();
        }
        if((h || !good) && rx_next == i){
            release_locked();
        }
        lock.unlock_irqrestore(fl);
    }
}

void E1000::irq(){
    uint32_t icr = rd(R_ICR);                   // citirea sterge cauzele
    uint64_t fl = lock.lock_irqsave();
    st.irqs++;
    lock.unlock_irqrestore(fl);
    (void)icr;                                  // link-ul si inelul RX se verifica oricum in reap()
    reap();
}

int E1000::recv(void* buf, size_t max){
    if(!buf){
        return -EINVAL;
    }
    reap();
    uint64_t fl = lock.lock_irqsave();
    while(rx_next != rx_scan){
        uint32_t i = rx_next;
        uint32_t n = rx_len[i];
        if(n == 0){
            release_locked();                   // cadru cu eroare, sarit
            continue;
        }
        if(n > max){
            n = (uint32_t)max;
        }
        memcpy(buf, rx_buf + (size_t)i * BUF_SIZE, n);
        release_locked();
        lock.unlock_irqrestore(fl);
        return (int)n;
    }
    lock.unlock_irqrestore(fl);
    return 0;
}

void E1000::set_rx_handler(NetRxHandler h, void* ctx){
    for(;;){
        uint64_t fl = lock.lock_irqsave();
        if(!reaping){
            handler = h;
            handler_ctx = ctx;
            if(h){
                // Cadrele deja numarate si neconsumate se arunca
                while(rx_next != rx_scan){
                    release_locked();
                }
            }
            bool spawn = h && mode == 0 && !poller;
            if(spawn){
                poller = true;
            }
            lock.unlock_irqrestore(fl);
            if(spawn){
                Task* t = task_create_kernel("e1000p", [](void* a){
                    E1000* d = (E1000*)a;
                    for(;;){
                        sleep_ms(1);
                        d->reap();
                    }
                }, this);
                if(t){
                    sched_add(t);
                } else {
                    kprintf("e1000: fara task de interogare\n");
                }
            }
            return;
        }
        lock.unlock_irqrestore(fl);
        __builtin_ia32_pause();
    }
}

// ---- transmisie ----

void E1000::reclaim_tx_locked(){
    while(tx_clean != tx_tail && (tx[tx_clean].status & TXD_DD)){
        tx_clean = (tx_clean + 1) % N_TX;
    }
}

int E1000::send(const void* frame, size_t len){
    if(!frame || len < ETH_HDR || len > ETH_HDR + 1500){
        uint64_t fl = lock.lock_irqsave();
        st.tx_errors++;
        lock.unlock_irqrestore(fl);
        return -EINVAL;
    }
    uint64_t fl = lock.lock_irqsave();
    reclaim_tx_locked();
    uint32_t i = tx_tail;
    uint32_t next = (i + 1) % N_TX;
    if(next == tx_clean){
        st.tx_dropped++;
        lock.unlock_irqrestore(fl);
        return -EAGAIN;
    }
    uint8_t* b = tx_buf + (size_t)i * BUF_SIZE;
    memcpy(b, frame, len);
    size_t n = len;
    if(n < ETH_MIN){
        memset(b + n, 0, ETH_MIN - n);
        n = ETH_MIN;
    }
    tx[i].len = (uint16_t)n;
    tx[i].cso = 0;
    tx[i].css = 0;
    tx[i].special = 0;
    tx[i].status = 0;
    tx[i].cmd = TXC_EOP | TXC_IFCS | TXC_RS;
    tx_tail = next;
    st.tx_packets++;
    st.tx_bytes += len;
    __sync_synchronize();
    wr(R_TDT, next);
    lock.unlock_irqrestore(fl);
    return 0;
}

void E1000::stats(NetStats& out){
    reap();
    uint64_t fl = lock.lock_irqsave();
    out = st;
    lock.unlock_irqrestore(fl);
}

// ---- PCI ----

int e1000_probe(PciDevice* p){
    if(nic_count >= MAX_NIC){
        return -1;
    }
    E1000* d = new E1000();
    int idx = nic_count;
    if(!d->init(p, idx)){
        nics[idx] = nullptr;
        return -1;
    }
    nic_count++;
    hal_register(HalKind::Net, d, NIC_NAMES[idx]);
    uint8_t m[6];
    d->mac(m);
    uint32_t status = d->rd(R_STATUS);
    uint32_t sp = (status >> 6) & 3;
    kprintf("e1000: %s %02x:%02x:%02x:%02x:%02x:%02x (%s), link %s %s, %s\n", NIC_NAMES[idx], m[0], m[1], m[2], m[3],
            m[4], m[5], d->mac_source(), (status & STATUS_LU) ? "up" : "down",
            (status & STATUS_LU) ? (sp == 0 ? "10 Mb/s" : sp == 1 ? "100 Mb/s" : "1000 Mb/s") : "",
            d->irq_mode() == 1 ? "MSI" : "interogare");
    return 0;
}

const PciDriver e1000_driver = {"e1000", 0x8086, 0x100E, PCI_ANY, PCI_ANY, e1000_probe};

}

void e1000_init(){
    pci_register_driver(&e1000_driver);
}

int e1000_count(){
    return nic_count;
}

NetDevice* e1000_dev(int idx){
    return idx >= 0 && idx < nic_count ? nics[idx] : nullptr;
}

int e1000_irq_mode(int idx){
    return idx >= 0 && idx < nic_count ? nics[idx]->irq_mode() : 0;
}

void e1000_poll(int idx){
    if(idx >= 0 && idx < nic_count){
        nics[idx]->reap();
    }
}
