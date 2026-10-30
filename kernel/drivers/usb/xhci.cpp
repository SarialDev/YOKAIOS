#include "drivers/usb/xhci.h"
#include "drivers/usb/usb.h"
#include "drivers/pci/pci.h"
#include "arch/x86_64/paging.h"
#include "mm/pmm.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "sync/spinlock.h"
#include "sync/mutex.h"
#include "sync/semaphore.h"
#include "time/clock.h"
#include "lib/kprintf.h"
#include "lib/string.h"

namespace {

// Registre de capabilitati
const uint32_t CAP_LENGTH = 0x00;
const uint32_t CAP_HCSPARAMS1 = 0x04;
const uint32_t CAP_HCSPARAMS2 = 0x08;
const uint32_t CAP_HCCPARAMS1 = 0x10;
const uint32_t CAP_DBOFF = 0x14;
const uint32_t CAP_RTSOFF = 0x18;

const uint32_t HCC_AC64 = 1 << 0;
const uint32_t HCC_CSZ = 1 << 2;        // contexte de 64 de octeti
const uint32_t HCC_PPC = 1 << 3;        // porturile au comutator de alimentare

// Registre operationale
const uint32_t OP_USBCMD = 0x00;
const uint32_t OP_USBSTS = 0x04;
const uint32_t OP_PAGESIZE = 0x08;
const uint32_t OP_CRCR = 0x18;
const uint32_t OP_DCBAAP = 0x30;
const uint32_t OP_CONFIG = 0x38;
const uint32_t OP_PORTSC = 0x400;       // + 0x10 * (port - 1)

const uint32_t CMD_RUN = 1 << 0;
const uint32_t CMD_HCRST = 1 << 1;
const uint32_t CMD_INTE = 1 << 2;

const uint32_t STS_HCH = 1 << 0;
const uint32_t STS_EINT = 1 << 3;
const uint32_t STS_CNR = 1 << 11;

const uint32_t PORT_CCS = 1 << 0;
const uint32_t PORT_PED = 1 << 1;       // RW1C: scrierea unui 1 dezactiveaza portul
const uint32_t PORT_PR = 1 << 4;
const uint32_t PORT_PP = 1 << 9;
const uint32_t PORT_CSC = 1 << 17;
const uint32_t PORT_PRC = 1 << 21;
const uint32_t PORT_CHANGES = 0x7F << 17;   // CSC PEC WRC OCC PRC PLC CEC, toate RW1C
// Bitii care se pastreaza la scriere (alimentare, indicator, wake); restul se scriu 0
const uint32_t PORT_KEEP = PORT_PP | (3 << 14) | (7 << 25);

// Registre runtime: interrupter 0
const uint32_t RT_IR0 = 0x20;
const uint32_t IR_IMAN = 0x00;
const uint32_t IR_IMOD = 0x04;
const uint32_t IR_ERSTSZ = 0x08;
const uint32_t IR_ERSTBA = 0x10;
const uint32_t IR_ERDP = 0x18;

const uint32_t IMAN_IP = 1 << 0;
const uint32_t IMAN_IE = 1 << 1;
const uint64_t ERDP_EHB = 1 << 3;
const uint32_t IMOD_250US = 1000;       // in unitati de 250 ns

// Extended capability: USB Legacy Support (predarea controller-ului de la BIOS)
const uint8_t XCAP_LEGACY = 1;
const uint32_t LEGACY_BIOS_OWNED = 1 << 16;
const uint32_t LEGACY_OS_OWNED = 1 << 24;

// TRB
const uint32_t TRB_CYCLE = 1 << 0;
const uint32_t TRB_TC = 1 << 1;         // Link: comuta cycle
const uint32_t TRB_ISP = 1 << 2;
const uint32_t TRB_IOC = 1 << 5;
const uint32_t TRB_IDT = 1 << 6;
const uint32_t TRB_DIR_IN = 1 << 16;

enum : uint32_t {
    TRB_NORMAL = 1,
    TRB_SETUP = 2,
    TRB_DATA = 3,
    TRB_STATUS = 4,
    TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9,
    TRB_DISABLE_SLOT = 10,
    TRB_ADDRESS_DEVICE = 11,
    TRB_CONFIGURE_EP = 12,
    TRB_EVALUATE_CTX = 13,
    TRB_RESET_EP = 14,
    TRB_SET_TR_DEQ = 16,
    TRB_EV_TRANSFER = 32,
    TRB_EV_COMMAND = 33,
    TRB_EV_PORT = 34,
};

inline uint32_t trb_type(uint32_t t){ return t << 10; }

const uint8_t CC_SUCCESS = 1;
const uint8_t CC_STALL = 6;
const uint8_t CC_SHORT = 13;

// Endpoint context: tipuri
const uint32_t EP_CONTROL = 4;

const int RING_TRBS = PAGE_SIZE / sizeof(XhciTrb);
const int MAX_CONTROLLERS = 2;
const int MAX_SLOTS = 32;
const int MAX_PORTS = 32;
const int EVENT_BATCH = 16;

const uint64_t MS = 1000000;
const uint64_t RESET_TIMEOUT_NS = 1000 * MS;
const uint64_t CMD_TIMEOUT_NS = 500 * MS;
const uint64_t XFER_TIMEOUT_NS = 1000 * MS;
const uint32_t PORT_RESET_RECOVERY_MS = 10;
const uint32_t POLL_MS = 4;             // fara intreruperi, usbd verifica event ring-ul periodic

}

struct XhciController {
    PciDevice* pci = nullptr;
    volatile uint8_t* cap = nullptr;
    volatile uint8_t* op = nullptr;
    volatile uint8_t* rt = nullptr;
    volatile uint32_t* db = nullptr;
    uint8_t max_slots = 0;
    uint8_t max_ports = 0;
    uint32_t ctx_size = 32;
    int vector = -1;                    // -1 = fara intreruperi (polling)

    uint64_t* dcbaa = nullptr;
    uint64_t dcbaa_phys = 0;
    XhciRing cmd = {};
    XhciTrb* evt = nullptr;
    uint64_t evt_phys = 0;
    uint16_t evt_deq = 0;
    uint8_t evt_cycle = 1;

    Spinlock lock{"xhci"};              // ring-uri si event ring
    Mutex cmd_mutex{"xhci cmd"};        // o comanda o data
    volatile uint64_t cmd_wait = 0;
    volatile bool cmd_done = false;
    volatile uint8_t cmd_code = 0;
    volatile uint8_t cmd_slot = 0;

    Semaphore port_events{0};           // Port Status Change -> usbd
    UsbDevice devs[MAX_SLOTS + 1] = {}; // dupa slot ID
    uint8_t port_slot[MAX_PORTS + 1] = {};
};

namespace {

XhciController* controllers[MAX_CONTROLLERS];
int controller_total = 0;

inline uint32_t rd32(volatile uint8_t* base, uint32_t off){
    return *(volatile uint32_t*)(base + off);
}

inline void wr32(volatile uint8_t* base, uint32_t off, uint32_t v){
    *(volatile uint32_t*)(base + off) = v;
}

// Registrele de 64 de biti se pot scrie ca doua jumatati, intai cea de jos
inline void wr64(volatile uint8_t* base, uint32_t off, uint64_t v){
    wr32(base, off, (uint32_t)v);
    wr32(base, off + 4, (uint32_t)(v >> 32));
}

inline volatile uint32_t* portsc(XhciController* c, int port){
    return (volatile uint32_t*)(c->op + OP_PORTSC + 0x10 * (port - 1));
}

inline bool irqs_on(){
    uint64_t f;
    __asm__ volatile("pushfq; pop %0" : "=r"(f));
    return f & (1 << 9);
}

// sleep_ms cere intreruperile active (task 0 asteapta cu hlt); la boot pot fi inca oprite
void delay_ms(uint32_t ms){
    if(irqs_on()){
        sleep_ms(ms);
        return;
    }
    uint64_t end = monotonic_ns() + (uint64_t)ms * MS;
    while(monotonic_ns() < end){
        __builtin_ia32_pause();
    }
}

void* dma_page(uint64_t& phys){
    phys = pmm_alloc_page();
    if(!phys){
        return nullptr;
    }
    void* v = phys_to_virt(phys);
    memset(v, 0, PAGE_SIZE);
    return v;
}

void dma_free(uint64_t& phys){
    if(phys){
        pmm_free_page(phys);
        phys = 0;
    }
}

bool ring_init(XhciRing& r){
    r.trbs = (XhciTrb*)dma_page(r.phys);
    if(!r.trbs){
        return false;
    }
    r.enq = 0;
    r.cycle = 1;
    r.trbs[RING_TRBS - 1].param = r.phys;
    r.trbs[RING_TRBS - 1].control = trb_type(TRB_LINK) | TRB_TC;
    return true;
}

void ring_free(XhciRing& r){
    dma_free(r.phys);
    r.trbs = nullptr;
}

// Scrie un TRB (cu lock-ul controller-ului luat); intoarce adresa lui fizica, care apare in evenimente
uint64_t ring_push(XhciRing& r, uint64_t param, uint32_t status, uint32_t control){
    volatile XhciTrb& t = r.trbs[r.enq];
    t.param = param;
    t.status = status;
    __asm__ volatile("" : : : "memory");
    t.control = (control & ~TRB_CYCLE) | r.cycle;   // cycle ultimul: abia atunci TRB-ul e al controller-ului
    uint64_t phys = r.phys + r.enq * sizeof(XhciTrb);
    if(++r.enq == RING_TRBS - 1){
        volatile XhciTrb& link = r.trbs[RING_TRBS - 1];
        link.control = trb_type(TRB_LINK) | TRB_TC | r.cycle;
        r.enq = 0;
        r.cycle ^= 1;
    }
    return phys;
}

void doorbell(XhciController* c, uint8_t slot, uint8_t target){
    __sync_synchronize();
    c->db[slot] = target;
}

bool wait_reg(volatile uint8_t* base, uint32_t off, uint32_t mask, uint32_t want, uint64_t timeout_ns){
    uint64_t end = monotonic_ns() + timeout_ns;
    while((rd32(base, off) & mask) != want){
        if(monotonic_ns() > end){
            return false;
        }
        __builtin_ia32_pause();
    }
    return true;
}

UsbEndpoint* find_ep(UsbDevice* d, uint8_t dci){
    for(int i = 0; i < d->ep_count; i++){
        if(d->eps[i].dci == dci){
            return &d->eps[i];
        }
    }
    return nullptr;
}

struct Finished {
    UsbEndpoint* ep;
    int status;
    uint16_t length;
};

int cc_status(uint8_t code){
    if(code == CC_SUCCESS || code == CC_SHORT){
        return USB_OK;
    }
    return code == CC_STALL ? USB_STALL : USB_ERROR;
}

// Un eveniment, cu lock-ul luat. Transferurile pe endpoint-uri de date se strang in done[] si se raporteaza dupa unlock
void handle_event(XhciController* c, uint64_t param, uint32_t status, uint32_t control, Finished* done, int& n,
                  bool& ports){
    uint32_t type = (control >> 10) & 0x3F;
    uint8_t code = status >> 24;
    if(type == TRB_EV_COMMAND){
        if(c->cmd_wait && param == c->cmd_wait){
            c->cmd_code = code;
            c->cmd_slot = control >> 24;
            c->cmd_done = true;
        }
    } else if(type == TRB_EV_PORT){
        ports = true;
    } else if(type == TRB_EV_TRANSFER){
        uint8_t slot = control >> 24;
        uint8_t dci = (control >> 16) & 0x1F;
        if(slot == 0 || slot > c->max_slots || !c->devs[slot].present){
            return;
        }
        UsbDevice* d = &c->devs[slot];
        int st = cc_status(code);
        if(dci == 1){
            // O eroare pe Setup/Data opreste TD-ul inainte de Status: o raportam si pe ea
            if(d->ctrl_wait && (param == d->ctrl_wait || st != USB_OK)){
                d->ctrl_status = st;
                d->ctrl_done = true;
            }
            return;
        }
        UsbEndpoint* ep = find_ep(d, dci);
        if(ep && ep->busy){
            uint32_t residual = status & 0xFFFFFF;
            ep->busy = false;
            done[n++] = {ep, st, (uint16_t)(residual < ep->buf_len ? ep->buf_len - residual : 0)};
        }
    }
}

void process_events(XhciController* c){
    for(;;){
        Finished done[EVENT_BATCH];
        int n = 0;
        bool ports = false;
        {
            SpinlockGuard g(c->lock);
            while(n < EVENT_BATCH){
                volatile XhciTrb& e = c->evt[c->evt_deq];
                uint32_t control = e.control;
                if((control & TRB_CYCLE) != c->evt_cycle){
                    break;
                }
                handle_event(c, e.param, e.status, control, done, n, ports);
                if(++c->evt_deq == RING_TRBS){
                    c->evt_deq = 0;
                    c->evt_cycle ^= 1;
                }
            }
            wr64(c->rt, RT_IR0 + IR_ERDP, (c->evt_phys + c->evt_deq * sizeof(XhciTrb)) | ERDP_EHB);
        }
        for(int i = 0; i < n; i++){
            if(done[i].ep->complete){
                done[i].ep->complete(done[i].ep, done[i].status, done[i].length);
            }
        }
        if(ports){
            c->port_events.up();
        }
        if(n < EVENT_BATCH){
            return;
        }
    }
}

void xhci_irq(InterruptFrame* f){
    for(int i = 0; i < controller_total; i++){
        XhciController* c = controllers[i];
        if(c->vector != (int)f->vector){
            continue;
        }
        wr32(c->op, OP_USBSTS, STS_EINT);
        wr32(c->rt, RT_IR0 + IR_IMAN, IMAN_IE | IMAN_IP);
        process_events(c);
    }
}

// Asteapta un flag pus de handle_event. Fara intreruperi (sau cu ele oprite) citim noi event ring-ul
bool wait_flag(XhciController* c, volatile bool& flag, uint64_t timeout_ns){
    uint64_t end = monotonic_ns() + timeout_ns;
    while(!flag){
        if(c->vector < 0 || !irqs_on()){
            process_events(c);
        }
        if(monotonic_ns() > end){
            process_events(c);
            return flag;
        }
        __builtin_ia32_pause();
    }
    return true;
}

// Trimite o comanda si asteapta Command Completion; intoarce codul de completare (0 la timeout)
uint8_t command(XhciController* c, uint64_t param, uint32_t control, uint8_t* slot_out = nullptr){
    MutexGuard m(c->cmd_mutex);
    {
        SpinlockGuard g(c->lock);
        c->cmd_done = false;
        c->cmd_wait = ring_push(c->cmd, param, 0, control);
        doorbell(c, 0, 0);
    }
    bool ok = wait_flag(c, c->cmd_done, CMD_TIMEOUT_NS);
    c->cmd_wait = 0;
    if(!ok){
        kprintf("[WARN] xhci: comanda %u fara raspuns\n", (control >> 10) & 0x3F);
        return 0;
    }
    if(slot_out){
        *slot_out = c->cmd_slot;
    }
    return c->cmd_code;
}

// Contextele: in input context indexul 0 e Input Control, 1 Slot, 1 + dci endpoint-ul; in output Slot e 0
inline uint32_t* in_ctx(UsbDevice* d, int idx){
    return (uint32_t*)(d->in_ctx + idx * d->hc->ctx_size);
}

inline uint32_t* out_ctx(UsbDevice* d, int idx){
    return (uint32_t*)(d->out_ctx + idx * d->hc->ctx_size);
}

void fill_ep_ctx(uint32_t* ep, uint32_t type, uint16_t mps, uint8_t interval, uint64_t ring_phys, uint16_t avg_len){
    ep[0] = (uint32_t)interval << 16;
    ep[1] = (3 << 1) | (type << 3) | ((uint32_t)mps << 16);    // CErr = 3
    ep[2] = (uint32_t)ring_phys | 1;                            // DCS = 1
    ep[3] = (uint32_t)(ring_phys >> 32);
    ep[4] = avg_len | ((uint32_t)(type == EP_CONTROL ? 0 : mps) << 16);   // Max ESIT Payload pentru periodic
}

// Endpoint oprit dupa STALL: Reset Endpoint + Set TR Dequeue Pointer la pozitia curenta
void recover_endpoint(XhciController* c, UsbDevice* d, uint8_t dci, XhciRing& r){
    command(c, 0, trb_type(TRB_RESET_EP) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24));
    uint64_t deq;
    {
        SpinlockGuard g(c->lock);
        deq = (r.phys + r.enq * sizeof(XhciTrb)) | r.cycle;
    }
    command(c, deq, trb_type(TRB_SET_TR_DEQ) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24));
}

void free_device(XhciController* c, UsbDevice* d){
    for(int i = 0; i < d->ep_count; i++){
        ring_free(d->eps[i].ring);
        dma_free(d->eps[i].buf_phys);
    }
    ring_free(d->ep0);
    dma_free(d->ctrl_phys);
    dma_free(d->in_ctx_phys);
    dma_free(d->out_ctx_phys);
    c->dcbaa[d->slot] = 0;
    memset(d, 0, sizeof(*d));
}

// Slot nou pentru un dispozitiv pe port: contexte, EP0, Address Device, apoi enumerarea USB
bool attach_device(XhciController* c, int port, uint8_t speed){
    uint8_t slot = 0;
    uint8_t cc = command(c, 0, trb_type(TRB_ENABLE_SLOT), &slot);
    if(cc != CC_SUCCESS || slot == 0 || slot > c->max_slots){
        kprintf("[WARN] xhci: Enable Slot a esuat (cc=%u)\n", cc);
        return false;
    }
    UsbDevice* d = &c->devs[slot];
    memset(d, 0, sizeof(*d));
    d->hc = c;
    d->slot = slot;
    d->port = port;
    d->speed = speed;
    d->ep0_mps = speed == USB_SPEED_LOW ? 8 : speed == USB_SPEED_SUPER ? 512 : 64;

    d->out_ctx = (uint8_t*)dma_page(d->out_ctx_phys);
    d->in_ctx = (uint8_t*)dma_page(d->in_ctx_phys);
    d->ctrl_buf = (uint8_t*)dma_page(d->ctrl_phys);
    if(!d->out_ctx || !d->in_ctx || !d->ctrl_buf || !ring_init(d->ep0)){
        kprintf("[WARN] xhci: fara memorie pentru slot %u\n", slot);
        free_device(c, d);
        command(c, 0, trb_type(TRB_DISABLE_SLOT) | ((uint32_t)slot << 24));
        return false;
    }
    c->dcbaa[slot] = d->out_ctx_phys;

    memset(d->in_ctx, 0, PAGE_SIZE);
    in_ctx(d, 0)[1] = (1 << 0) | (1 << 1);                     // Add: Slot si EP0
    in_ctx(d, 1)[0] = ((uint32_t)speed << 20) | (1u << 27);     // Context Entries = 1
    in_ctx(d, 1)[1] = (uint32_t)port << 16;                     // Root Hub Port Number
    fill_ep_ctx(in_ctx(d, 2), EP_CONTROL, d->ep0_mps, 0, d->ep0.phys, 8);
    d->present = true;

    cc = command(c, d->in_ctx_phys, trb_type(TRB_ADDRESS_DEVICE) | ((uint32_t)slot << 24));
    if(cc != CC_SUCCESS){
        kprintf("[WARN] xhci: Address Device pe portul %d a esuat (cc=%u)\n", port, cc);
    } else {
        c->port_slot[port] = slot;
        if(usb_enumerate(d)){
            return true;
        }
        c->port_slot[port] = 0;
        usb_disconnect(d);
    }
    {
        SpinlockGuard g(c->lock);
        d->present = false;
    }
    command(c, 0, trb_type(TRB_DISABLE_SLOT) | ((uint32_t)slot << 24));
    free_device(c, d);
    return false;
}

void detach_port(XhciController* c, int port){
    uint8_t slot = c->port_slot[port];
    if(!slot){
        return;
    }
    UsbDevice* d = &c->devs[slot];
    {
        SpinlockGuard g(c->lock);
        d->present = false;
    }
    kprintf("usb: deconectat de pe portul %d (slot %u)\n", port, slot);
    usb_disconnect(d);
    command(c, 0, trb_type(TRB_DISABLE_SLOT) | ((uint32_t)slot << 24));
    c->port_slot[port] = 0;
    free_device(c, d);
}

// Dispozitiv nou pe port. USB2 are nevoie de port reset ca sa se activeze; USB3 e deja activ dupa link training
void attach_port(XhciController* c, int port){
    volatile uint32_t* sc = portsc(c, port);
    uint32_t v = *sc;
    if(!(v & PORT_CCS)){
        return;
    }
    if(!(v & PORT_PED)){
        *sc = (v & PORT_KEEP) | PORT_PR;
        uint64_t end = monotonic_ns() + RESET_TIMEOUT_NS;
        while(!(*sc & PORT_PRC) && monotonic_ns() < end){
            __builtin_ia32_pause();
        }
    }
    delay_ms(PORT_RESET_RECOVERY_MS);
    v = *sc;
    *sc = (v & PORT_KEEP) | (v & PORT_CHANGES);
    if((v & (PORT_CCS | PORT_PED)) != (PORT_CCS | PORT_PED)){
        kprintf("[WARN] xhci: portul %d nu s-a activat (portsc=%x)\n", port, v);
        return;
    }
    uint8_t speed = (v >> 10) & 0xF;
    attach_device(c, port, speed);
}

void scan_ports(XhciController* c){
    for(int p = 1; p <= c->max_ports; p++){
        volatile uint32_t* sc = portsc(c, p);
        uint32_t v = *sc;
        if(v & PORT_CHANGES){
            *sc = (v & PORT_KEEP) | (v & PORT_CHANGES);
        }
        // CSC cu dispozitiv atasat: a fost scos (si poate pus altul la loc)
        if(c->port_slot[p] && (!(v & PORT_CCS) || (v & PORT_CSC))){
            detach_port(c, p);
        }
        if((v & PORT_CCS) && !c->port_slot[p]){
            attach_port(c, p);
        }
    }
}

// Task-ul usbd: conectari/deconectari dupa boot; fara intreruperi si citeste event ring-ul
void xhci_worker(void* arg){
    XhciController* c = (XhciController*)arg;
    for(;;){
        if(c->vector < 0){
            sleep_ms(POLL_MS);
            process_events(c);
            if(!c->port_events.try_down()){
                continue;
            }
        } else {
            c->port_events.down();
        }
        while(c->port_events.try_down()){
        }
        scan_ports(c);
    }
}

// Daca BIOS-ul foloseste controller-ul (emulare PS/2 prin SMI), il cerem inapoi
void bios_handoff(XhciController* c, uint32_t hcc1){
    uint32_t off = (hcc1 >> 16) << 2;
    while(off){
        uint32_t v = rd32(c->cap, off);
        if((v & 0xFF) == XCAP_LEGACY){
            if(v & LEGACY_BIOS_OWNED){
                wr32(c->cap, off, v | LEGACY_OS_OWNED);
                if(!wait_reg(c->cap, off, LEGACY_BIOS_OWNED, 0, RESET_TIMEOUT_NS)){
                    kprintf("[WARN] xhci: BIOS-ul nu elibereaza controller-ul\n");
                }
            }
            // SMI-uri oprite, statusurile RW1C sterse
            uint32_t ctl = rd32(c->cap, off + 4);
            wr32(c->cap, off + 4, (ctl & ~0xE011u) | 0xE0000000u);
            return;
        }
        uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + (next << 2) : 0;
    }
}

bool reset_controller(XhciController* c){
    uint32_t cmd = rd32(c->op, OP_USBCMD);
    if(cmd & CMD_RUN){
        wr32(c->op, OP_USBCMD, cmd & ~CMD_RUN);
        if(!wait_reg(c->op, OP_USBSTS, STS_HCH, STS_HCH, RESET_TIMEOUT_NS)){
            return false;
        }
    }
    wr32(c->op, OP_USBCMD, CMD_HCRST);
    return wait_reg(c->op, OP_USBCMD, CMD_HCRST, 0, RESET_TIMEOUT_NS) &&
           wait_reg(c->op, OP_USBSTS, STS_CNR, 0, RESET_TIMEOUT_NS);
}

// DCBAA (+ scratchpad), command ring, event ring cu ERST de o intrare
bool setup_memory(XhciController* c, uint32_t hcs2){
    c->dcbaa = (uint64_t*)dma_page(c->dcbaa_phys);
    if(!c->dcbaa){
        return false;
    }
    uint32_t scratch = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if(scratch){
        uint64_t arr_phys;
        uint64_t* arr = (uint64_t*)dma_page(arr_phys);
        if(!arr){
            return false;
        }
        for(uint32_t i = 0; i < scratch; i++){
            if(!dma_page(arr[i])){
                return false;
            }
        }
        c->dcbaa[0] = arr_phys;
    }
    wr64(c->op, OP_DCBAAP, c->dcbaa_phys);

    if(!ring_init(c->cmd)){
        return false;
    }
    wr64(c->op, OP_CRCR, c->cmd.phys | 1);     // RCS = 1

    uint64_t erst_phys;
    uint64_t* erst = (uint64_t*)dma_page(erst_phys);
    c->evt = (XhciTrb*)dma_page(c->evt_phys);
    if(!erst || !c->evt){
        return false;
    }
    erst[0] = c->evt_phys;
    erst[1] = RING_TRBS;
    c->evt_deq = 0;
    c->evt_cycle = 1;
    wr32(c->rt, RT_IR0 + IR_ERSTSZ, 1);
    wr64(c->rt, RT_IR0 + IR_ERDP, c->evt_phys);
    wr64(c->rt, RT_IR0 + IR_ERSTBA, erst_phys);
    return true;
}

// MSI-X, altfel MSI, altfel polling din usbd
void setup_irq(XhciController* c){
    PciDevice* p = c->pci;
    if(pci_msix_enable(p)){
        c->vector = pci_msix_setup(p, 0, xhci_irq);
        if(c->vector < 0){
            pci_msix_disable(p);
        }
    }
    if(c->vector < 0){
        c->vector = pci_msi_enable(p, xhci_irq);
    }
    wr32(c->rt, RT_IR0 + IR_IMOD, IMOD_250US);
    wr32(c->rt, RT_IR0 + IR_IMAN, IMAN_IE | IMAN_IP);
}

int xhci_probe(PciDevice* p){
    if(p->progif != 0x30 || controller_total >= MAX_CONTROLLERS){
        return -1;
    }
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_MASTER);
    volatile uint8_t* base = (volatile uint8_t*)pci_map_bar(p, 0);
    if(!base){
        return -1;
    }
    XhciController* c = new XhciController;
    c->pci = p;
    c->cap = base;
    c->op = base + (rd32(base, CAP_LENGTH) & 0xFF);
    c->rt = base + (rd32(base, CAP_RTSOFF) & ~0x1Fu);
    c->db = (volatile uint32_t*)(base + (rd32(base, CAP_DBOFF) & ~0x3u));
    uint32_t hcs1 = rd32(base, CAP_HCSPARAMS1);
    uint32_t hcs2 = rd32(base, CAP_HCSPARAMS2);
    uint32_t hcc1 = rd32(base, CAP_HCCPARAMS1);
    c->max_slots = (hcs1 & 0xFF) < MAX_SLOTS ? (hcs1 & 0xFF) : MAX_SLOTS;
    c->max_ports = (hcs1 >> 24) < MAX_PORTS ? (hcs1 >> 24) : MAX_PORTS;
    c->ctx_size = (hcc1 & HCC_CSZ) ? 64 : 32;
    if(!(hcc1 & HCC_AC64)){
        kprintf("[WARN] xhci: fara adrese de 64 de biti, memoria DMA trebuie sa fie sub 4 GiB\n");
    }

    bios_handoff(c, hcc1);
    if(!reset_controller(c)){
        kprintf("[FAIL] xhci: reset esuat\n");
        return -1;
    }
    if(!(rd32(c->op, OP_PAGESIZE) & 1)){
        kprintf("[FAIL] xhci: nu suporta pagini de 4 KiB\n");
        return -1;
    }
    wr32(c->op, OP_CONFIG, c->max_slots);
    if(!setup_memory(c, hcs2)){
        kprintf("[FAIL] xhci: fara memorie\n");
        return -1;
    }
    controllers[controller_total++] = c;
    setup_irq(c);

    wr32(c->op, OP_USBCMD, CMD_RUN | CMD_INTE);
    if(!wait_reg(c->op, OP_USBSTS, STS_HCH, 0, RESET_TIMEOUT_NS)){
        kprintf("[FAIL] xhci: controller-ul nu porneste\n");
        return -1;
    }
    if(hcc1 & HCC_PPC){
        for(int port = 1; port <= c->max_ports; port++){
            volatile uint32_t* sc = portsc(c, port);
            *sc = (*sc & PORT_KEEP) | PORT_PP;
        }
        delay_ms(20);
    }
    kprintf("[OK] xhci %02x:%02x.%u: %u porturi, %u sloturi, ctx %u, %s vec %d\n", pci_bus(p), pci_slot(p),
            pci_fn(p), c->max_ports, c->max_slots, c->ctx_size,
            p->msix_table ? "MSI-X" : p->msi_vector ? "MSI" : "polling", c->vector);

    // Dispozitivele prezente la boot, sincron; restul vin prin Port Status Change in usbd
    for(int port = 1; port <= c->max_ports; port++){
        if(*portsc(c, port) & PORT_CCS){
            attach_port(c, port);
        }
    }
    Task* worker = task_create_kernel("usbd", xhci_worker, c);
    if(worker){
        sched_add(worker);
    } else {
        kprintf("[WARN] xhci: fara task usbd, conectarile dupa boot se ignora\n");
    }
    return 0;
}

const PciDriver xhci_driver = {"xhci", PCI_ANY, PCI_ANY, 0x0C, 0x03, xhci_probe};

}

void xhci_init(){
    pci_register_driver(&xhci_driver);
}

int xhci_controller_count(){
    return controller_total;
}

int xhci_control(UsbDevice* d, const UsbSetup& s, void* data, bool wait){
    XhciController* c = d->hc;
    bool in = s.request_type & USB_DIR_IN;
    if(s.length > (wait ? USB_CTRL_MAX : 64) || (!wait && in)){
        return USB_ERROR;
    }
    // Sincron in prima jumatate a paginii; asincron, rotativ, in a doua (bufferul trebuie sa traiasca pana la DMA)
    static uint32_t async_next = 0;
    {
        SpinlockGuard g(c->lock);
        if(!d->present){
            return USB_GONE;
        }
        uint32_t off = 0;
        if(!wait){
            off = USB_CTRL_MAX + (async_next++ % (USB_CTRL_MAX / 64)) * 64;
        }
        if(!in && s.length){
            memcpy(d->ctrl_buf + off, data, s.length);
        }
        uint64_t setup;
        memcpy(&setup, &s, sizeof(setup));
        uint32_t trt = s.length == 0 ? 0 : in ? 3 : 2;
        ring_push(d->ep0, setup, 8, trb_type(TRB_SETUP) | TRB_IDT | (trt << 16));
        if(s.length){
            ring_push(d->ep0, d->ctrl_phys + off, s.length, trb_type(TRB_DATA) | (in ? TRB_DIR_IN : 0));
        }
        bool status_in = !(s.length && in);
        uint64_t st = ring_push(d->ep0, 0, 0, trb_type(TRB_STATUS) | TRB_IOC | (status_in ? TRB_DIR_IN : 0));
        if(wait){
            d->ctrl_done = false;
            d->ctrl_wait = st;
        }
        doorbell(c, d->slot, 1);
    }
    if(!wait){
        return USB_OK;
    }
    bool ok = wait_flag(c, d->ctrl_done, XFER_TIMEOUT_NS);
    d->ctrl_wait = 0;
    if(!ok){
        return USB_TIMEOUT;
    }
    int st = d->ctrl_status;
    if(st == USB_STALL){
        recover_endpoint(c, d, 1, d->ep0);
    } else if(st == USB_OK && in && s.length){
        memcpy(data, d->ctrl_buf, s.length);
    }
    return st;
}

bool xhci_set_ep0_mps(UsbDevice* d, uint16_t mps){
    memset(d->in_ctx, 0, PAGE_SIZE);
    in_ctx(d, 0)[1] = 1 << 1;
    fill_ep_ctx(in_ctx(d, 2), EP_CONTROL, mps, 0, d->ep0.phys, 8);
    if(command(d->hc, d->in_ctx_phys, trb_type(TRB_EVALUATE_CTX) | ((uint32_t)d->slot << 24)) != CC_SUCCESS){
        return false;
    }
    d->ep0_mps = mps;
    return true;
}

// Intervalul xHCI e exponent: perioada = 2^interval * 125 us
static uint8_t ep_interval(const UsbDevice* d, const UsbEndpoint& ep){
    uint8_t type = ep.attributes & 3;
    if(type != USB_EP_INTERRUPT && type != USB_EP_ISOCH){
        return 0;
    }
    if(d->speed == USB_SPEED_FULL || d->speed == USB_SPEED_LOW){
        // bInterval in ms (cadre): cel mai mare 2^k <= bInterval * 8
        uint32_t uframes = (ep.interval ? ep.interval : 1) * 8;
        uint8_t k = 0;
        while((2u << k) <= uframes){
            k++;
        }
        return k < 3 ? 3 : k > 10 ? 10 : k;
    }
    uint8_t k = ep.interval ? ep.interval - 1 : 0;
    return k > 15 ? 15 : k;
}

bool xhci_configure_endpoints(UsbDevice* d){
    XhciController* c = d->hc;
    memset(d->in_ctx, 0, PAGE_SIZE);
    uint32_t add = 1;
    uint8_t max_dci = 1;
    for(int i = 0; i < d->ep_count; i++){
        UsbEndpoint& ep = d->eps[i];
        if(!ep.ring.trbs && !ring_init(ep.ring)){
            return false;
        }
        if(!ep.buf && !(ep.buf = (uint8_t*)dma_page(ep.buf_phys))){
            return false;
        }
        bool in = ep.address & USB_DIR_IN;
        uint32_t type = (ep.attributes & 3) + (in ? 4 : 0);   // 2/6 bulk, 3/7 interrupt
        fill_ep_ctx(in_ctx(d, 1 + ep.dci), type, ep.max_packet, ep_interval(d, ep), ep.ring.phys, ep.max_packet);
        add |= 1u << ep.dci;
        if(ep.dci > max_dci){
            max_dci = ep.dci;
        }
    }
    in_ctx(d, 0)[1] = add;
    memcpy(in_ctx(d, 1), out_ctx(d, 0), c->ctx_size);
    uint32_t* slot = in_ctx(d, 1);
    slot[0] = (slot[0] & ~(0x1Fu << 27)) | ((uint32_t)max_dci << 27);
    slot[3] = 0;
    uint8_t cc = command(c, d->in_ctx_phys, trb_type(TRB_CONFIGURE_EP) | ((uint32_t)d->slot << 24));
    if(cc != CC_SUCCESS){
        kprintf("[WARN] xhci: Configure Endpoint a esuat (cc=%u)\n", cc);
        return false;
    }
    return true;
}

bool xhci_submit(UsbEndpoint* ep){
    UsbDevice* d = ep->dev;
    XhciController* c = d->hc;
    SpinlockGuard g(c->lock);
    if(!d->present || ep->busy || !ep->ring.trbs || ep->buf_len > PAGE_SIZE){
        return false;
    }
    ep->busy = true;
    ring_push(ep->ring, ep->buf_phys, ep->buf_len, trb_type(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    doorbell(c, d->slot, ep->dci);
    return true;
}
