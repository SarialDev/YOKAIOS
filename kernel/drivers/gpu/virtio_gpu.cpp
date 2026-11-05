#include "drivers/gpu/virtio_gpu.h"
#include "drivers/pci/pci.h"
#include "drivers/fb/fb.h"
#include "arch/x86_64/paging.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "sync/spinlock.h"
#include "time/clock.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"
#include "uapi/gpu.h"
#include "uapi/net.h"     // ETIMEDOUT

namespace {

// ---- transportul virtio 1.x prin PCI ----

const uint8_t PCI_CAP_VENDOR = 0x09;
const uint8_t CFG_COMMON = 1;
const uint8_t CFG_NOTIFY = 2;
const uint8_t CFG_ISR = 3;
const uint8_t CFG_DEVICE = 4;

const uint8_t ST_ACK = 1;
const uint8_t ST_DRIVER = 2;
const uint8_t ST_DRIVER_OK = 4;
const uint8_t ST_FEATURES_OK = 8;
const uint8_t ST_FAILED = 128;

const uint32_t F1_VERSION_1 = 1u << 0;      // bitul 32

struct __attribute__((packed)) CommonCfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint32_t queue_desc_lo, queue_desc_hi;
    uint32_t queue_driver_lo, queue_driver_hi;
    uint32_t queue_device_lo, queue_device_hi;
};

struct VqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
static_assert(sizeof(VqDesc) == 16, "VqDesc");
const uint16_t DESC_NEXT = 1;
const uint16_t DESC_WRITE = 2;
const uint16_t AVAIL_NO_INTERRUPT = 1;

struct VqAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct VqUsedElem {
    uint32_t id;
    uint32_t len;
};

struct VqUsed {
    uint16_t flags;
    uint16_t idx;
    VqUsedElem ring[];
};

// ---- protocolul virtio-gpu ----

const uint32_t GPU_F_VIRGL = 1u << 0;
const uint32_t GPU_F_EDID = 1u << 1;
const uint32_t GPU_F_UUID = 1u << 2;
const uint32_t GPU_F_BLOB = 1u << 3;
const uint32_t GPU_F_CTX_INIT = 1u << 4;

const uint32_t CMD_GET_DISPLAY_INFO = 0x0100;
const uint32_t CMD_RESOURCE_CREATE_2D = 0x0101;
const uint32_t CMD_RESOURCE_UNREF = 0x0102;
const uint32_t CMD_SET_SCANOUT = 0x0103;
const uint32_t CMD_RESOURCE_FLUSH = 0x0104;
const uint32_t CMD_TRANSFER_TO_HOST_2D = 0x0105;
const uint32_t CMD_RESOURCE_ATTACH_BACKING = 0x0106;
const uint32_t CMD_RESOURCE_DETACH_BACKING = 0x0107;
const uint32_t CMD_GET_CAPSET_INFO = 0x0108;
const uint32_t RESP_OK_NODATA = 0x1100;
const uint32_t RESP_OK_DISPLAY_INFO = 0x1101;
const uint32_t RESP_OK_CAPSET_INFO = 0x1102;
const uint32_t RESP_ERR_FIRST = 0x1200;

const uint32_t FORMAT_B8G8R8X8 = 2;
const uint32_t RESOURCE_ID = 1;
const int MAX_SCANOUTS = 16;

struct CtrlHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};
static_assert(sizeof(CtrlHdr) == 24, "CtrlHdr");

struct GpuRect {
    uint32_t x, y, w, h;
};

struct RespDisplayInfo {
    CtrlHdr hdr;
    struct {
        GpuRect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[MAX_SCANOUTS];
};
static_assert(sizeof(RespDisplayInfo) == 408, "RespDisplayInfo");

struct ReqCreate2d {
    CtrlHdr hdr;
    uint32_t resource_id, format, width, height;
};

struct ReqResource {               // UNREF, DETACH_BACKING
    CtrlHdr hdr;
    uint32_t resource_id, padding;
};

struct ReqSetScanout {
    CtrlHdr hdr;
    GpuRect r;
    uint32_t scanout_id, resource_id;
};

struct ReqFlush {
    CtrlHdr hdr;
    GpuRect r;
    uint32_t resource_id, padding;
};

struct ReqTransfer2d {
    CtrlHdr hdr;
    GpuRect r;
    uint64_t offset;
    uint32_t resource_id, padding;
};
static_assert(sizeof(ReqTransfer2d) == 56, "ReqTransfer2d");

struct ReqAttachBacking {
    CtrlHdr hdr;
    uint32_t resource_id, nr_entries;
};

struct MemEntry {
    uint64_t addr;
    uint32_t length, padding;
};

struct ReqCapsetInfo {
    CtrlHdr hdr;
    uint32_t capset_index, padding;
};

struct RespCapsetInfo {
    CtrlHdr hdr;
    uint32_t capset_id, capset_max_version, capset_max_size, padding;
};

struct DevCfg {                    // virtio_gpu_config
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
};

// ---- starea driverului ----

const uint16_t QSIZE_MAX = 64;
const int NSLOTS = 16;                      // un slot = un lant de 2 descriptori (cerere, raspuns)
const uint32_t REQ_SLOT = 256;
const uint32_t RESP_SLOT = 512;
const uint64_t SYNC_TIMEOUT_NS = 2000000000ull;

struct Gpu {
    bool ready;
    PciDevice* pci;
    volatile CommonCfg* common;
    volatile uint8_t* notify;               // adresa de notificare a cozii 0
    volatile DevCfg* devcfg;

    uint16_t qsize;
    VqDesc* desc;
    VqAvail* avail;
    volatile VqUsed* used;
    uint16_t avail_idx;
    uint16_t used_last;
    uint8_t* req;                           // NSLOTS x REQ_SLOT
    uint64_t req_pa;
    uint8_t* resp;                          // NSLOTS x RESP_SLOT
    uint64_t resp_pa;
    bool busy[NSLOTS];
    int inflight;
    int next_slot;

    uint32_t features;
    uint32_t caps;
    uint32_t num_scanouts;
    uint32_t num_capsets;
    yk_gpu_capset capset[YGPU_MAX_CAPSETS];
    uint32_t host_w, host_h;

    uint32_t width, height, stride;
    uint64_t map_len;
    uint64_t* pages;                        // map_len / 4K pagini fizice, cat timp exista utilizatori
    uint32_t npages;
    int users;
    bool scanout;

    uint64_t cmds, flushes, bytes, errors, waits;
    uint32_t logged_errors;
};

Gpu g;
// Lock cu intreruperile oprite (lock_irqsave): fara jiffies, ceasul poate sta pe loc, deci limita de timp e si in iteratii
Spinlock gpu_lock("virtio-gpu");

const uint64_t SPIN_MAX = 20000000;

struct Deadline {
    uint64_t end = monotonic_ns() + SYNC_TIMEOUT_NS;
    uint64_t spins = 0;
    bool expired(){
        return ++spins > SPIN_MAX || monotonic_ns() > end;
    }
};

uint32_t rd_status(){
    return g.common->device_status;
}

void barrier(){
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// Culege raspunsurile terminate; elibereaza sloturile
void reap(){
    while(true){
        uint16_t idx = __atomic_load_n(&g.used->idx, __ATOMIC_ACQUIRE);
        if(idx == g.used_last){
            break;
        }
        uint32_t id = g.used->ring[g.used_last % g.qsize].id;
        g.used_last++;
        int s = (int)(id / 2);
        if(s < 0 || s >= NSLOTS || !g.busy[s]){
            continue;
        }
        const CtrlHdr* r = (const CtrlHdr*)(g.resp + s * RESP_SLOT);
        if(r->type >= RESP_ERR_FIRST){
            g.errors++;
            if(g.logged_errors < 8){
                g.logged_errors++;
                const CtrlHdr* q = (const CtrlHdr*)(g.req + s * REQ_SLOT);
                kprintf("virtio-gpu: eroare 0x%x la comanda 0x%x\n", r->type, q->type);
            }
        }
        g.busy[s] = false;
        g.inflight--;
    }
}

// Un slot liber; asteapta (spin, cu limita) daca toate sunt in zbor. -1 la timeout
int get_slot(){
    reap();
    Deadline dl;
    bool waited = false;
    while(true){
        for(int k = 0; k < NSLOTS; k++){
            int s = (g.next_slot + k) % NSLOTS;
            if(!g.busy[s]){
                g.next_slot = (s + 1) % NSLOTS;
                g.busy[s] = true;
                ((CtrlHdr*)(g.resp + s * RESP_SLOT))->type = 0;
                return s;
            }
        }
        if(!waited){
            g.waits++;
            waited = true;
        }
        if(dl.expired()){
            return -1;
        }
        __builtin_ia32_pause();
        reap();
    }
}

// Pune lantul slotului s in inelul "avail" (fara notificare). ext_pa != 0: cererea sta in alt bufer DMA
void queue_slot(int s, uint32_t req_len, uint32_t resp_len, uint64_t ext_pa = 0){
    VqDesc& a = g.desc[2 * s];
    VqDesc& b = g.desc[2 * s + 1];
    a.addr = ext_pa ? ext_pa : g.req_pa + (uint64_t)s * REQ_SLOT;
    a.len = req_len;
    a.flags = DESC_NEXT;
    a.next = (uint16_t)(2 * s + 1);
    b.addr = g.resp_pa + (uint64_t)s * RESP_SLOT;
    b.len = resp_len;
    b.flags = DESC_WRITE;
    b.next = 0;
    g.avail->ring[g.avail_idx % g.qsize] = (uint16_t)(2 * s);
    g.avail_idx++;
    g.inflight++;
    g.cmds++;
}

void kick(){
    barrier();
    __atomic_store_n(&g.avail->idx, g.avail_idx, __ATOMIC_RELEASE);
    barrier();
    *(volatile uint16_t*)g.notify = 0;      // coada 0 (controlq)
}

bool wait_slot(int s){
    Deadline dl;
    while(true){
        reap();
        if(!g.busy[s]){
            return true;
        }
        if(dl.expired()){
            return false;
        }
        __builtin_ia32_pause();
    }
}

// Comanda sincrona: copiaza cererea, asteapta raspunsul. Intoarce tipul raspunsului sau 0 (timeout / fara slot)
uint32_t cmd_sync(const void* req, uint32_t req_len, void* resp, uint32_t resp_len, uint64_t ext_pa = 0){
    int s = get_slot();
    if(s < 0){
        return 0;
    }
    if(!ext_pa){
        memcpy(g.req + s * REQ_SLOT, req, req_len);
    }
    queue_slot(s, req_len, resp_len ? resp_len : sizeof(CtrlHdr), ext_pa);
    kick();
    if(!wait_slot(s)){
        kprintf("virtio-gpu: timeout la comanda 0x%x\n", ((const CtrlHdr*)req)->type);
        return 0;
    }
    const uint8_t* r = g.resp + s * RESP_SLOT;
    if(resp){
        memcpy(resp, r, resp_len);
    }
    return ((const CtrlHdr*)r)->type;
}

CtrlHdr hdr(uint32_t type){
    CtrlHdr h;
    memset(&h, 0, sizeof(h));
    h.type = type;
    return h;
}

bool wait_idle(){
    Deadline dl;
    while(true){
        reap();
        if(g.inflight == 0){
            return true;
        }
        if(dl.expired()){
            return false;
        }
        __builtin_ia32_pause();
    }
}

// ---- initializare ----

bool find_caps(PciDevice* d, volatile CommonCfg*& common, volatile uint8_t*& notify_base, uint32_t& notify_mult,
               volatile DevCfg*& devcfg){
    common = nullptr;
    notify_base = nullptr;
    devcfg = nullptr;
    notify_mult = 0;
    if(!(pci_cfg_read16(d->bdf, 0x06) & (1u << 4))){
        return false;                       // fara lista de capabilitati
    }
    uint8_t off = pci_cfg_read8(d->bdf, 0x34) & 0xFC;
    void* bars[PCI_BAR_COUNT] = {};
    for(int guard = 0; off && guard < 48; guard++){
        uint8_t id = pci_cfg_read8(d->bdf, off);
        uint8_t next = pci_cfg_read8(d->bdf, off + 1) & 0xFC;
        if(id == PCI_CAP_VENDOR){
            uint8_t type = pci_cfg_read8(d->bdf, off + 3);
            uint8_t bar = pci_cfg_read8(d->bdf, off + 4);
            uint32_t boff = pci_cfg_read32(d->bdf, off + 8);
            if(bar < PCI_BAR_COUNT && (type == CFG_COMMON || type == CFG_NOTIFY || type == CFG_DEVICE)){
                if(!bars[bar]){
                    bars[bar] = pci_map_bar(d, bar);
                }
                if(bars[bar]){
                    volatile uint8_t* p = (volatile uint8_t*)bars[bar] + boff;
                    if(type == CFG_COMMON && !common){
                        common = (volatile CommonCfg*)p;
                    } else if(type == CFG_NOTIFY && !notify_base){
                        notify_base = p;
                        notify_mult = pci_cfg_read32(d->bdf, off + 16);
                    } else if(type == CFG_DEVICE && !devcfg){
                        devcfg = (volatile DevCfg*)p;
                    }
                }
            }
            (void)CFG_ISR;
        }
        off = next;
    }
    return common && notify_base && devcfg;
}

bool setup_queue(uint32_t notify_mult, volatile uint8_t* notify_base){
    g.common->queue_select = 0;
    uint16_t dev_size = g.common->queue_size;
    if(!dev_size){
        return false;
    }
    g.qsize = dev_size < QSIZE_MAX ? dev_size : QSIZE_MAX;
    if(g.qsize < 2 * NSLOTS){
        return false;
    }
    // Pagina 0: descriptori + avail + used; pagina 1: cereri; paginile 2-3: raspunsuri
    uint64_t pa = pmm_alloc_contig(4);
    if(!pa){
        return false;
    }
    uint8_t* va = (uint8_t*)phys_to_virt(pa);
    memset(va, 0, 4 * PAGE_SIZE);
    uint32_t avail_off = g.qsize * sizeof(VqDesc);
    uint32_t used_off = (avail_off + 6 + 2 * g.qsize + 3) & ~3u;
    g.desc = (VqDesc*)va;
    g.avail = (VqAvail*)(va + avail_off);
    g.used = (volatile VqUsed*)(va + used_off);
    g.req = va + PAGE_SIZE;
    g.req_pa = pa + PAGE_SIZE;
    g.resp = va + 2 * PAGE_SIZE;
    g.resp_pa = pa + 2 * PAGE_SIZE;
    g.avail->flags = AVAIL_NO_INTERRUPT;    // fara intreruperi: raspunsurile se culeg prin polling

    g.common->queue_size = g.qsize;
    g.common->queue_msix_vector = 0xFFFF;
    g.common->queue_desc_lo = (uint32_t)pa;
    g.common->queue_desc_hi = (uint32_t)(pa >> 32);
    g.common->queue_driver_lo = (uint32_t)(pa + avail_off);
    g.common->queue_driver_hi = (uint32_t)((pa + avail_off) >> 32);
    g.common->queue_device_lo = (uint32_t)(pa + used_off);
    g.common->queue_device_hi = (uint32_t)((pa + used_off) >> 32);
    g.notify = notify_base + (uint64_t)g.common->queue_notify_off * notify_mult;
    g.common->queue_enable = 1;
    return true;
}

void query_host(){
    CtrlHdr h = hdr(CMD_GET_DISPLAY_INFO);
    RespDisplayInfo di;
    memset(&di, 0, sizeof(di));
    if(cmd_sync(&h, sizeof(h), &di, sizeof(di)) == RESP_OK_DISPLAY_INFO){
        for(uint32_t i = 0; i < g.num_scanouts && i < (uint32_t)MAX_SCANOUTS; i++){
            if(di.pmodes[i].enabled){
                g.host_w = di.pmodes[i].r.w;
                g.host_h = di.pmodes[i].r.h;
                break;
            }
        }
    }
    uint32_t n = g.num_capsets < YGPU_MAX_CAPSETS ? g.num_capsets : YGPU_MAX_CAPSETS;
    for(uint32_t i = 0; i < n; i++){
        ReqCapsetInfo q;
        memset(&q, 0, sizeof(q));
        q.hdr = hdr(CMD_GET_CAPSET_INFO);
        q.capset_index = i;
        RespCapsetInfo r;
        memset(&r, 0, sizeof(r));
        if(cmd_sync(&q, sizeof(q), &r, sizeof(r)) == RESP_OK_CAPSET_INFO){
            g.capset[i].id = r.capset_id;
            g.capset[i].max_version = r.capset_max_version;
            g.capset[i].max_size = r.capset_max_size;
        }
    }
}

int probe(PciDevice* d){
    if(g.ready){
        return -1;                          // un singur dispozitiv
    }
    volatile CommonCfg* common;
    volatile uint8_t* notify_base;
    uint32_t notify_mult;
    volatile DevCfg* devcfg;
    pci_enable(d, PCI_CMD_MEM | PCI_CMD_MASTER | PCI_CMD_INTX_OFF);
    if(!find_caps(d, common, notify_base, notify_mult, devcfg)){
        kprintf("virtio-gpu: lipsesc capabilitatile virtio 1.x\n");
        return -1;
    }
    g.pci = d;
    g.common = common;
    g.devcfg = devcfg;

    g.common->device_status = 0;            // reset
    for(int i = 0; i < 1000000 && rd_status() != 0; i++){
        __builtin_ia32_pause();
    }
    g.common->device_status = ST_ACK;
    g.common->device_status = ST_ACK | ST_DRIVER;
    g.common->device_feature_select = 0;
    uint32_t f0 = g.common->device_feature;
    g.common->device_feature_select = 1;
    uint32_t f1 = g.common->device_feature;
    if(!(f1 & F1_VERSION_1)){
        kprintf("virtio-gpu: dispozitivul nu are VIRTIO_F_VERSION_1\n");
        g.common->device_status = ST_FAILED;
        return -1;
    }
    // Acceptam doar VERSION_1: 2D merge fara alte functii; restul doar se raporteaza
    g.common->driver_feature_select = 0;
    g.common->driver_feature = 0;
    g.common->driver_feature_select = 1;
    g.common->driver_feature = F1_VERSION_1;
    g.common->device_status = ST_ACK | ST_DRIVER | ST_FEATURES_OK;
    if(!(rd_status() & ST_FEATURES_OK)){
        kprintf("virtio-gpu: FEATURES_OK refuzat\n");
        g.common->device_status = ST_FAILED;
        return -1;
    }
    if(!setup_queue(notify_mult, notify_base)){
        kprintf("virtio-gpu: coada de control nu se poate crea\n");
        g.common->device_status = ST_FAILED;
        return -1;
    }
    g.common->device_status = ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK;

    g.features = f0;
    g.caps = YGPU_CAP_2D;
    if(f0 & GPU_F_VIRGL){
        g.caps |= YGPU_CAP_VIRGL;
    }
    if(f0 & GPU_F_EDID){
        g.caps |= YGPU_CAP_EDID;
    }
    if(f0 & GPU_F_UUID){
        g.caps |= YGPU_CAP_UUID;
    }
    if(f0 & GPU_F_BLOB){
        g.caps |= YGPU_CAP_BLOB;
    }
    if(f0 & GPU_F_CTX_INIT){
        g.caps |= YGPU_CAP_CTX_INIT;
    }
    g.num_scanouts = g.devcfg->num_scanouts;
    g.num_capsets = g.devcfg->num_capsets;
    query_host();

    // Marimea resursei: ca framebuffer-ul de boot (aceeasi geometrie pentru compozitor), altfel modul gazdei
    if(fb_ready()){
        g.width = fb_width();
        g.height = fb_height();
    } else if(g.host_w && g.host_h){
        g.width = g.host_w;
        g.height = g.host_h;
    } else {
        g.width = 1024;
        g.height = 768;
    }
    g.stride = g.width * 4;
    g.map_len = ((uint64_t)g.stride * g.height + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    g.ready = true;

    char line[96];
    pci_format(d, line, sizeof(line));
    kprintf("virtio-gpu: %s\n", line);
    kprintf("virtio-gpu: 2D %ux%u, scanout-uri %u, gazda %ux%u, 3D virgl: %s, blob: %s, context_init: %s, capset-uri %u\n",
            g.width, g.height, g.num_scanouts, g.host_w, g.host_h, (g.caps & YGPU_CAP_VIRGL) ? "da" : "nu",
            (g.caps & YGPU_CAP_BLOB) ? "da" : "nu", (g.caps & YGPU_CAP_CTX_INIT) ? "da" : "nu", g.num_capsets);
    for(uint32_t i = 0; i < g.num_capsets && i < YGPU_MAX_CAPSETS; i++){
        kprintf("virtio-gpu: capset %u v%u (%u o.)\n", g.capset[i].id, g.capset[i].max_version, g.capset[i].max_size);
    }
    return 0;
}

const PciDriver driver = {"virtio-gpu", 0x1AF4, 0x1050, PCI_ANY, PCI_ANY, probe};

// ---- resursa 2D (sub gpu_lock) ----

void free_pages(){
    for(uint32_t i = 0; i < g.npages; i++){
        if(g.pages[i]){
            pmm_free_page(g.pages[i]);
        }
    }
    kfree(g.pages);
    g.pages = nullptr;
    g.npages = 0;
}

bool resource_up(){
    g.npages = (uint32_t)(g.map_len / PAGE_SIZE);
    g.pages = (uint64_t*)kzalloc(sizeof(uint64_t) * g.npages);
    if(!g.pages){
        g.npages = 0;
        return false;
    }
    uint32_t runs = 0;
    for(uint32_t i = 0; i < g.npages; i++){
        uint64_t p = pmm_alloc_page();
        if(!p){
            free_pages();
            return false;
        }
        memset(phys_to_virt(p), 0, PAGE_SIZE);
        g.pages[i] = p;
        if(!i || g.pages[i - 1] + PAGE_SIZE != p){
            runs++;
        }
    }
    // Lista de pagini a resursei: alergari de pagini consecutive
    uint64_t list_len = sizeof(ReqAttachBacking) + (uint64_t)runs * sizeof(MemEntry);
    uint64_t list_pages = (list_len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t list_pa = pmm_alloc_contig(list_pages);
    if(!list_pa){
        free_pages();
        return false;
    }
    ReqAttachBacking* ab = (ReqAttachBacking*)phys_to_virt(list_pa);
    memset(ab, 0, list_len);
    ab->hdr = hdr(CMD_RESOURCE_ATTACH_BACKING);
    ab->resource_id = RESOURCE_ID;
    ab->nr_entries = runs;
    MemEntry* e = (MemEntry*)(ab + 1);
    int k = -1;
    for(uint32_t i = 0; i < g.npages; i++){
        if(!i || g.pages[i - 1] + PAGE_SIZE != g.pages[i]){
            k++;
            e[k].addr = g.pages[i];
            e[k].length = 0;
        }
        e[k].length += PAGE_SIZE;
    }

    ReqCreate2d c;
    memset(&c, 0, sizeof(c));
    c.hdr = hdr(CMD_RESOURCE_CREATE_2D);
    c.resource_id = RESOURCE_ID;
    c.format = FORMAT_B8G8R8X8;
    c.width = g.width;
    c.height = g.height;
    bool ok = cmd_sync(&c, sizeof(c), nullptr, 0) == RESP_OK_NODATA;
    ok = ok && cmd_sync(ab, (uint32_t)list_len, nullptr, 0, list_pa) == RESP_OK_NODATA;
    for(uint64_t i = 0; i < list_pages; i++){
        pmm_free_page(list_pa + i * PAGE_SIZE);
    }
    if(ok){
        ReqSetScanout s;
        memset(&s, 0, sizeof(s));
        s.hdr = hdr(CMD_SET_SCANOUT);
        s.r = {0, 0, g.width, g.height};
        s.scanout_id = 0;
        s.resource_id = RESOURCE_ID;
        ok = cmd_sync(&s, sizeof(s), nullptr, 0) == RESP_OK_NODATA;
    }
    if(!ok){
        kprintf("virtio-gpu: resursa 2D nu s-a putut crea\n");
        ReqResource u;
        memset(&u, 0, sizeof(u));
        u.hdr = hdr(CMD_RESOURCE_UNREF);
        u.resource_id = RESOURCE_ID;
        cmd_sync(&u, sizeof(u), nullptr, 0);
        free_pages();
        return false;
    }
    g.scanout = true;
    return true;
}

void resource_down(){
    wait_idle();
    ReqSetScanout s;
    memset(&s, 0, sizeof(s));
    s.hdr = hdr(CMD_SET_SCANOUT);           // resource_id 0 = scanout oprit (virtio-vga revine la VGA)
    cmd_sync(&s, sizeof(s), nullptr, 0);
    ReqResource u;
    memset(&u, 0, sizeof(u));
    u.hdr = hdr(CMD_RESOURCE_DETACH_BACKING);
    u.resource_id = RESOURCE_ID;
    cmd_sync(&u, sizeof(u), nullptr, 0);
    u.hdr = hdr(CMD_RESOURCE_UNREF);
    cmd_sync(&u, sizeof(u), nullptr, 0);
    if(!wait_idle()){
        // Gazda inca poate citi paginile: mai bine le pierdem decat sa le refolosim
        kprintf("virtio-gpu: comenzi neterminate, paginile resursei raman alocate\n");
        kfree(g.pages);
        g.pages = nullptr;
        g.npages = 0;
    } else {
        free_pages();
    }
    g.scanout = false;
}

}

void virtio_gpu_init(){
    pci_register_driver(&driver);
}

bool virtio_gpu_ready(){
    return g.ready;
}

bool virtio_gpu_info(yk_gpu_info& out){
    if(!g.ready){
        return false;
    }
    SpinlockGuard lk(gpu_lock);
    reap();
    memset(&out, 0, sizeof(out));
    out.abi = YGPU_ABI;
    out.width = g.width;
    out.height = g.height;
    out.stride = g.stride;
    out.format = YGPU_FMT_XRGB8888;
    out.map_len = (uint32_t)g.map_len;
    out.caps = g.caps | (g.scanout ? YGPU_CAP_SCANOUT : 0);
    out.features = g.features;
    out.num_scanouts = g.num_scanouts;
    out.num_capsets = g.num_capsets;
    out.host_w = g.host_w;
    out.host_h = g.host_h;
    memcpy(out.capset, g.capset, sizeof(out.capset));
    out.cmds = g.cmds;
    out.flushes = g.flushes;
    out.bytes = g.bytes;
    out.errors = g.errors;
    out.waits = g.waits;
    return true;
}

void virtio_gpu_user_get(){
    if(!g.ready){
        return;
    }
    SpinlockGuard lk(gpu_lock);
    if(g.users++ == 0 && !resource_up()){
        // Maparea ramane fara pagini: accesul da SIGBUS (map_phys -> -ENXIO)
        kprintf("virtio-gpu: fara resursa pentru mapare\n");
    }
}

void virtio_gpu_user_put(){
    if(!g.ready){
        return;
    }
    SpinlockGuard lk(gpu_lock);
    if(g.users > 0 && --g.users == 0 && g.scanout){
        resource_down();
    }
}

bool virtio_gpu_user_owned(){
    return g.ready && __atomic_load_n(&g.users, __ATOMIC_ACQUIRE) != 0;
}

uint64_t virtio_gpu_map_len(){
    return g.ready ? g.map_len : 0;
}

int64_t virtio_gpu_map_phys(uint64_t off){
    if(!g.ready || off >= g.map_len || (off & (PAGE_SIZE - 1))){
        return -ENXIO;
    }
    SpinlockGuard lk(gpu_lock);
    if(!g.pages){
        return -ENXIO;
    }
    return (int64_t)g.pages[off / PAGE_SIZE];
}

int virtio_gpu_flush(const yk_gpu_flush& f){
    if(!g.ready){
        return -ENODEV;
    }
    if(f.n < 1 || f.n > YGPU_MAX_RECTS){
        return -EINVAL;
    }
    SpinlockGuard lk(gpu_lock);
    if(!g.scanout){
        return -ENXIO;
    }
    g.flushes++;
    uint32_t x0 = g.width, y0 = g.height, x1 = 0, y1 = 0;
    int queued = 0;
    for(uint32_t i = 0; i < f.n; i++){
        const yk_gpu_rect& r = f.r[i];
        if(r.x >= g.width || r.y >= g.height || !r.w || !r.h){
            continue;
        }
        uint32_t w = r.w > g.width - r.x ? g.width - r.x : r.w;
        uint32_t h = r.h > g.height - r.y ? g.height - r.y : r.h;
        int s = get_slot();
        if(s < 0){
            break;
        }
        ReqTransfer2d* t = (ReqTransfer2d*)(g.req + s * REQ_SLOT);
        memset(t, 0, sizeof(*t));
        t->hdr = hdr(CMD_TRANSFER_TO_HOST_2D);
        t->r = {r.x, r.y, w, h};
        t->offset = (uint64_t)r.y * g.stride + (uint64_t)r.x * 4;
        t->resource_id = RESOURCE_ID;
        queue_slot(s, sizeof(*t), sizeof(CtrlHdr));
        queued++;
        g.bytes += (uint64_t)w * h * 4;
        x0 = r.x < x0 ? r.x : x0;
        y0 = r.y < y0 ? r.y : y0;
        x1 = r.x + w > x1 ? r.x + w : x1;
        y1 = r.y + h > y1 ? r.y + h : y1;
    }
    if(!queued){
        return 0;
    }
    int s = get_slot();
    if(s >= 0){
        ReqFlush* q = (ReqFlush*)(g.req + s * REQ_SLOT);
        memset(q, 0, sizeof(*q));
        q->hdr = hdr(CMD_RESOURCE_FLUSH);
        q->r = {x0, y0, x1 - x0, y1 - y0};
        q->resource_id = RESOURCE_ID;
        queue_slot(s, sizeof(*q), sizeof(CtrlHdr));
    }
    kick();
    return s >= 0 ? 0 : -ETIMEDOUT;
}

int virtio_gpu_sync(){
    if(!g.ready){
        return -ENODEV;
    }
    SpinlockGuard lk(gpu_lock);
    return wait_idle() ? 0 : -ETIMEDOUT;
}

uint64_t virtio_gpu_cmd_count(){
    return g.cmds;
}
