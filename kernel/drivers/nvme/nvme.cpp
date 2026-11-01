#include "drivers/nvme/nvme.h"
#include "block/blockdev.h"
#include "block/partition.h"
#include "hal/hal.h"
#include "drivers/pci/pci.h"
#include "arch/x86_64/paging.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "time/clock.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace {

// Registre controller
const uint32_t R_CAP = 0x00;                // 64 biti
const uint32_t R_VS = 0x08;
const uint32_t R_CC = 0x14;
const uint32_t R_CSTS = 0x1C;
const uint32_t R_AQA = 0x24;
const uint32_t R_ASQ = 0x28;                // 64 biti
const uint32_t R_ACQ = 0x30;                // 64 biti
const uint32_t R_DB = 0x1000;

const uint32_t CC_EN = 1u << 0;
const uint32_t CSTS_RDY = 1u << 0;
const uint32_t CSTS_CFS = 1u << 1;

// Comenzi admin / I/O
const uint8_t ADM_CREATE_SQ = 0x01;
const uint8_t ADM_CREATE_CQ = 0x05;
const uint8_t ADM_IDENTIFY = 0x06;
const uint8_t ADM_SET_FEATURES = 0x09;
const uint8_t IO_FLUSH = 0x00;
const uint8_t IO_WRITE = 0x01;
const uint8_t IO_READ = 0x02;

const int MAX_CTRL = 2;
const int MAX_NS = 8;                       // total pe toate controllerele (HAL are 16 de locuri pentru blocuri + partitii)
const uint32_t ADMIN_QD = 16;
const uint32_t IO_QD = 64;                  // o pagina de SQ (64 x 64 B) si una de CQ
const uint32_t MAX_XFER = 128 * 1024;
const int MAX_PAGES = (int)(MAX_XFER / PAGE_SIZE) + 1;
const uint64_t ADMIN_TIMEOUT_MS = 3000;

struct Sqe {
    uint32_t cdw0;                          // opcode | cid << 16
    uint32_t nsid;
    uint32_t rsvd[2];
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};
static_assert(sizeof(Sqe) == 64, "Sqe");

struct Cqe {
    uint32_t result;
    uint32_t rsvd;
    uint32_t sq;                            // SQ head [15:0], SQ id [31:16]
    uint32_t status;                        // cid [15:0], P [16], status [31:17]
};
static_assert(sizeof(Cqe) == 16, "Cqe");

struct NvmeCtrl;

struct NvmeReq {
    BlockRequest base;                      // primul membru: start() primeste BlockRequest*
    uint8_t opcode;
    uint32_t nsid;
    int npg;
    uint64_t pg[MAX_PAGES];                 // adresa fizica a fiecarei pagini atinse (prima poate avea offset)
    NvmeReq(BlockOp op, uint8_t opc, uint32_t ns, uint64_t lba, uint32_t count, void* buf)
        : base(op, lba, count, buf), opcode(opc), nsid(ns), npg(0), pg(){}
};

inline uint32_t rd32(volatile uint8_t* b, uint32_t off){
    return *(volatile uint32_t*)(b + off);
}

inline void wr32(volatile uint8_t* b, uint32_t off, uint32_t v){
    *(volatile uint32_t*)(b + off) = v;
}

inline uint64_t rd64(volatile uint8_t* b, uint32_t off){
    return (uint64_t)rd32(b, off) | (uint64_t)rd32(b, off + 4) << 32;
}

inline void wr64(volatile uint8_t* b, uint32_t off, uint64_t v){
    wr32(b, off, (uint32_t)v);
    wr32(b, off + 4, (uint32_t)(v >> 32));
}

bool wait_rdy(volatile uint8_t* b, uint32_t want, uint64_t ms){
    uint64_t end = monotonic_ns() + ms * 1000000;
    while((rd32(b, R_CSTS) & CSTS_RDY) != want){
        if(monotonic_ns() > end){
            return false;
        }
        __builtin_ia32_pause();
    }
    return true;
}

class NvmeNs;

struct NvmeCtrl {
    PciDevice* pci;
    volatile uint8_t* regs;
    uint64_t cap;
    uint32_t dstrd;                         // stride-ul registrelor doorbell, in octeti
    uint32_t max_xfer;
    int idx;
    int irq_mode;                           // 0 interogare, 1 MSI, 2 MSI-X
    bool dead;
    char model[41];
    char serial[21];

    // Coada admin (folosita doar la init, prin interogare)
    uint64_t asq_pa, acq_pa;
    Sqe* asq;
    Cqe* acq;
    uint32_t a_tail, a_head, a_phase;
    uint16_t a_cid;

    // Coada I/O 1
    uint64_t sq_pa, cq_pa;
    Sqe* sq;
    Cqe* cq;
    uint32_t sq_tail, cq_head, cq_phase;
    uint16_t cid_next, cur_cid;
    uint64_t list_pa;                       // lista PRP a request-ului curent (unul singur in zbor)
    uint64_t* list;

    BlockQueue q;
    NvmeNs* ns[MAX_NS];
    int nns;

    void service();                         // cu lock-ul cozii tinut
    int start(NvmeReq* r);
    void doorbell_sq(uint32_t qid, uint32_t tail){ wr32(regs, R_DB + (2 * qid) * dstrd, tail); }
    void doorbell_cq(uint32_t qid, uint32_t head){ wr32(regs, R_DB + (2 * qid + 1) * dstrd, head); }
    int admin(Sqe& c, uint32_t* result);
    int io_rw(uint32_t nsid, bool write, uint64_t lba, uint32_t count, void* buf, uint32_t bs);
    int io_flush(uint32_t nsid);
    bool map_buf(NvmeReq& r, const void* buf, uint64_t bytes);
};

class NvmeNs final : public BlockDevice {
public:
    NvmeNs() : c(nullptr), nsid(0), bs(0), blocks(0), name{}{}
    uint32_t block_size() const override { return bs; }
    uint64_t block_count() const override { return blocks; }
    int read(uint64_t lba, uint32_t count, void* buf) override { return c->io_rw(nsid, false, lba, count, buf, bs); }
    int write(uint64_t lba, uint32_t count, const void* buf) override {
        return c->io_rw(nsid, true, lba, count, (void*)buf, bs);
    }
    int flush() override { return c->io_flush(nsid); }

    NvmeCtrl* c;
    uint32_t nsid;
    uint32_t bs;
    uint64_t blocks;
    char name[12];                          // "nvme0n1"
};

NvmeCtrl* ctrls[MAX_CTRL];
int ctrl_count;
NvmeNs* all_ns[MAX_NS];
int ns_total;
volatile uint32_t irq_total;
volatile uint32_t admin_irq_total;

// ---- cozi ----

// Comanda de admin pe coada 0, cu interogare; status 0 = succes, altfel codul NVMe (SCT << 8 | SC) cu semn minus
int NvmeCtrl::admin(Sqe& c, uint32_t* result){
    uint16_t cid = ++a_cid;
    c.cdw0 = (c.cdw0 & 0xFFFF) | (uint32_t)cid << 16;
    asq[a_tail] = c;
    a_tail = (a_tail + 1) % ADMIN_QD;
    __sync_synchronize();
    doorbell_sq(0, a_tail);
    uint64_t end = monotonic_ns() + ADMIN_TIMEOUT_MS * 1000000;
    for(;;){
        volatile Cqe* e = &acq[a_head];
        uint32_t st = e->status;
        if(((st >> 16) & 1) == a_phase){
            uint32_t res = e->result;
            a_head++;
            if(a_head == ADMIN_QD){
                a_head = 0;
                a_phase ^= 1;
            }
            doorbell_cq(0, a_head);
            if((st & 0xFFFF) != cid){
                continue;
            }
            if(result){
                *result = res;
            }
            uint32_t sc = (st >> 17) & 0x7FFF;
            return sc ? -(int)(0x10000 | sc) : 0;
        }
        if(monotonic_ns() > end || (rd32(regs, R_CSTS) & CSTS_CFS)){
            return -EIO;
        }
        __builtin_ia32_pause();
    }
}

// Cu lock-ul cozii tinut: consuma completarile si le termina pe cea curenta
void NvmeCtrl::service(){
    for(;;){
        volatile Cqe* e = &cq[cq_head];
        uint32_t st = e->status;
        if(((st >> 16) & 1) != cq_phase){
            break;
        }
        cq_head++;
        if(cq_head == IO_QD){
            cq_head = 0;
            cq_phase ^= 1;
        }
        doorbell_cq(1, cq_head);
        uint16_t cid = (uint16_t)(st & 0xFFFF);
        if(q.current_locked() && cid == cur_cid){
            uint32_t sc = (st >> 17) & 0x7FFF;
            if(sc){
                kprintf("nvme%d: eroare SCT=%x SC=%x\n", idx, sc >> 8, sc & 0xFF);
            }
            q.finish_locked(sc ? -EIO : 0);
        }
    }
}

// Lock-ul cozii tinut: fara alocari si fara vmm
int NvmeCtrl::start(NvmeReq* r){
    if(dead){
        return -EIO;
    }
    Sqe c;
    memset(&c, 0, sizeof(c));
    cur_cid = ++cid_next;
    c.cdw0 = r->opcode | (uint32_t)cur_cid << 16;
    c.nsid = r->nsid;
    if(r->opcode != IO_FLUSH){
        uint64_t lba = r->base.lba;
        c.cdw10 = (uint32_t)lba;
        c.cdw11 = (uint32_t)(lba >> 32);
        c.cdw12 = r->base.count - 1;
        c.prp1 = r->pg[0];
        if(r->npg == 2){
            c.prp2 = r->pg[1];
        } else if(r->npg > 2){
            for(int i = 1; i < r->npg; i++){
                list[i - 1] = r->pg[i];
            }
            c.prp2 = list_pa;
        }
    }
    sq[sq_tail] = c;
    sq_tail = (sq_tail + 1) % IO_QD;
    __sync_synchronize();
    doorbell_sq(1, sq_tail);
    return 0;
}

int q_start(void* ctx, BlockRequest* r){
    return static_cast<NvmeCtrl*>(ctx)->start(reinterpret_cast<NvmeReq*>(r));
}

void q_poll(void* ctx){
    static_cast<NvmeCtrl*>(ctx)->service();
}

// Nu exista anulare simpla a unei comenzi: dupa timeout controllerul se considera mort
void q_abort(void* ctx){
    NvmeCtrl* c = static_cast<NvmeCtrl*>(ctx);
    c->dead = true;
    kprintf("nvme%d: timeout, controller dezactivat\n", c->idx);
}

const BlockQueueOps queue_ops = {q_start, q_poll, q_abort};

void io_irq(NvmeCtrl* c){
    irq_total = irq_total + 1;
    SpinlockGuard g(c->q.lock);
    c->service();
}

void io_irq0(InterruptFrame*){ if(ctrls[0]) io_irq(ctrls[0]); }
void io_irq1(InterruptFrame*){ if(ctrls[1]) io_irq(ctrls[1]); }
const InterruptHandler io_fns[MAX_CTRL] = {io_irq0, io_irq1};

// Completarile adminului se iau prin interogare; intreruperea doar se numara
void admin_irq0(InterruptFrame*){ admin_irq_total = admin_irq_total + 1; }
void admin_irq1(InterruptFrame*){ admin_irq_total = admin_irq_total + 1; }
const InterruptHandler admin_fns[MAX_CTRL] = {admin_irq0, admin_irq1};

// ---- transfer de date ----

// Paginile fizice ale buffer-ului. Se cheama din task (vmm_translate ia un lock), nu din IRQ
bool NvmeCtrl::map_buf(NvmeReq& r, const void* buf, uint64_t bytes){
    uint64_t v = (uint64_t)(uintptr_t)buf;
    if(v & 3){
        return false;                       // PRP-urile cer adrese aliniate la dword
    }
    while(bytes){
        uint64_t chunk = PAGE_SIZE - (v & (PAGE_SIZE - 1));
        if(chunk > bytes){
            chunk = bytes;
        }
        uint64_t pa = vmm_translate(kernel_as, v);
        if(!pa || r.npg == MAX_PAGES){
            return false;
        }
        r.pg[r.npg++] = pa;
        v += chunk;
        bytes -= chunk;
    }
    return true;
}

int NvmeCtrl::io_rw(uint32_t nsid, bool write, uint64_t lba, uint32_t count, void* buf, uint32_t bs){
    NvmeNs* n = nullptr;
    for(int i = 0; i < nns; i++){
        if(ns[i]->nsid == nsid){
            n = ns[i];
        }
    }
    if(count == 0){
        return 0;
    }
    if(!n || lba >= n->blocks || count > n->blocks - lba){
        return -EINVAL;
    }
    uint8_t* p = (uint8_t*)buf;
    uint32_t max_n = max_xfer / bs;
    while(count){
        uint32_t k = count < max_n ? count : max_n;
        NvmeReq r(write ? BlockOp::Write : BlockOp::Read, write ? IO_WRITE : IO_READ, nsid, lba, k, p);
        if(!map_buf(r, p, (uint64_t)k * bs)){
            return -EINVAL;
        }
        int err = q.submit_wait(&r.base);
        if(err){
            return err;
        }
        lba += k;
        count -= k;
        p += (uint64_t)k * bs;
    }
    return 0;
}

int NvmeCtrl::io_flush(uint32_t nsid){
    NvmeReq r(BlockOp::Flush, IO_FLUSH, nsid, 0, 0, nullptr);
    return q.submit_wait(&r.base);
}

// ---- initializare ----

uint64_t alloc_zeroed(void*& va){
    uint64_t pa = pmm_alloc_page();
    if(!pa){
        return 0;
    }
    va = phys_to_virt(pa);
    memset(va, 0, PAGE_SIZE);
    return pa;
}

void trim(char* s, int n){
    s[n] = 0;
    for(int i = n - 1; i >= 0 && (s[i] == ' ' || s[i] == 0); i--){
        s[i] = 0;
    }
}

bool create_queues(NvmeCtrl* c, int io_vector){
    Sqe cmd;
    uint32_t res = 0;
    // Number of Queues: cerem una de fiecare
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADM_SET_FEATURES;
    cmd.cdw10 = 0x07;
    cmd.cdw11 = 0;
    if(c->admin(cmd, &res)){
        kprintf("nvme%d: Set Features (Number of Queues) a esuat\n", c->idx);
        return false;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADM_CREATE_CQ;
    cmd.prp1 = c->cq_pa;
    cmd.cdw10 = (IO_QD - 1) << 16 | 1;
    cmd.cdw11 = (uint32_t)(io_vector >= 0 ? io_vector : 0) << 16 | (io_vector >= 0 ? 2u : 0u) | 1u;
    if(c->admin(cmd, nullptr)){
        kprintf("nvme%d: Create I/O CQ a esuat\n", c->idx);
        return false;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADM_CREATE_SQ;
    cmd.prp1 = c->sq_pa;
    cmd.cdw10 = (IO_QD - 1) << 16 | 1;
    cmd.cdw11 = 1u << 16 | 1u;
    if(c->admin(cmd, nullptr)){
        kprintf("nvme%d: Create I/O SQ a esuat\n", c->idx);
        return false;
    }
    return true;
}

bool identify(NvmeCtrl* c, uint32_t cns, uint32_t nsid, uint64_t pa){
    Sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADM_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = pa;
    cmd.cdw10 = cns;
    return c->admin(cmd, nullptr) == 0;
}

// Namespace-ul nsid: dimensiune si format LBA; nullptr daca nu e utilizabil
NvmeNs* setup_ns(NvmeCtrl* c, uint32_t nsid, uint8_t* data, uint64_t data_pa){
    if(!identify(c, 0, nsid, data_pa)){
        return nullptr;
    }
    uint64_t nsze = *(uint64_t*)data;
    uint8_t flbas = data[26];
    uint32_t fmt = (flbas & 0xF) | ((flbas >> 5) & 3) << 4;
    if(fmt > 63 || nsze == 0){
        return nullptr;
    }
    uint32_t lbaf = *(uint32_t*)(data + 128 + 4 * fmt);
    uint32_t ms = lbaf & 0xFFFF;
    uint32_t ds = (lbaf >> 16) & 0xFF;
    if(ms != 0 || ds < 9 || ds > 12){
        kprintf("nvme%d: namespace %u cu metadate (%u) sau sector 2^%u, ignorat\n", c->idx, nsid, ms, ds);
        return nullptr;
    }
    NvmeNs* n = new NvmeNs;
    n->c = c;
    n->nsid = nsid;
    n->bs = 1u << ds;
    n->blocks = nsze;
    char* nm = n->name;
    nm[0] = 'n'; nm[1] = 'v'; nm[2] = 'm'; nm[3] = 'e';
    nm[4] = (char)('0' + c->idx);
    nm[5] = 'n';
    int k = 6;
    char tmp[10];
    int t = 0;
    for(uint32_t v = nsid; v && t < 9; v /= 10){
        tmp[t++] = (char)('0' + v % 10);
    }
    while(t){
        nm[k++] = tmp[--t];
    }
    nm[k] = 0;
    return n;
}

int nvme_probe(PciDevice* p){
    if(p->progif != 0x02 || ctrl_count >= MAX_CTRL){
        return -1;
    }
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_MASTER);
    volatile uint8_t* regs = (volatile uint8_t*)pci_map_bar(p, 0);
    if(!regs){
        return -1;
    }
    uint64_t cap = rd64(regs, R_CAP);
    uint32_t mqes = (uint32_t)(cap & 0xFFFF) + 1;
    uint32_t to_ms = (uint32_t)((cap >> 24) & 0xFF) * 500;
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0xF);
    if(!(cap & (1ull << 37)) || mpsmin != 0){
        kprintf("nvme: set de comenzi NVM sau pagina de 4 KiB lipsa (CAP=%lx), ignorat\n", cap);
        return -1;
    }
    if(to_ms < 1000){
        to_ms = 1000;
    }
    if(mqes < 2){
        kprintf("nvme: MQES prea mic\n");
        return -1;
    }

    NvmeCtrl* c = new NvmeCtrl();               // value-init: tot zero, apoi constructorul cozii
    c->pci = p;
    c->regs = regs;
    c->cap = cap;
    c->dstrd = 4u << ((cap >> 32) & 0xF);
    c->idx = ctrl_count;
    c->max_xfer = MAX_XFER;
    c->a_phase = 1;
    c->cq_phase = 1;

    // Oprim controllerul daca firmware-ul l-a lasat pornit
    uint32_t cc = rd32(regs, R_CC);
    if(cc & CC_EN){
        wr32(regs, R_CC, cc & ~CC_EN);
    }
    if(!wait_rdy(regs, 0, to_ms)){
        kprintf("nvme: controllerul nu se opreste\n");
        return -1;
    }

    void* va = nullptr;
    uint8_t* data;
    uint64_t data_pa;
    c->asq_pa = alloc_zeroed(va);
    c->asq = (Sqe*)va;
    c->acq_pa = alloc_zeroed(va);
    c->acq = (Cqe*)va;
    c->sq_pa = alloc_zeroed(va);
    c->sq = (Sqe*)va;
    c->cq_pa = alloc_zeroed(va);
    c->cq = (Cqe*)va;
    c->list_pa = alloc_zeroed(va);
    c->list = (uint64_t*)va;
    data_pa = alloc_zeroed(va);
    data = (uint8_t*)va;
    if(!c->asq_pa || !c->acq_pa || !c->sq_pa || !c->cq_pa || !c->list_pa || !data_pa){
        kprintf("nvme: fara memorie\n");
        return -1;
    }

    wr32(regs, R_AQA, (ADMIN_QD - 1) << 16 | (ADMIN_QD - 1));
    wr64(regs, R_ASQ, c->asq_pa);
    wr64(regs, R_ACQ, c->acq_pa);
    // IOSQES = 6 (64 B), IOCQES = 4 (16 B), MPS = 0 (4 KiB), CSS = 0 (NVM), AMS = 0 (round robin)
    wr32(regs, R_CC, CC_EN | (6u << 16) | (4u << 20));
    if(!wait_rdy(regs, CSTS_RDY, to_ms) || (rd32(regs, R_CSTS) & CSTS_CFS)){
        kprintf("nvme: controllerul nu devine gata (CSTS=%x)\n", rd32(regs, R_CSTS));
        return -1;
    }

    // Intreruperi: MSI-X (admin pe intrarea 0, I/O pe 1), altfel MSI, altfel interogare
    int idx = ctrl_count;
    ctrls[idx] = c;
    int io_vec = -1;
    if(pci_msix_enable(p)){
        int av = pci_msix_setup(p, 0, admin_fns[idx]);
        io_vec = pci_msix_setup(p, 1, io_fns[idx]);
        if(av >= 0 && io_vec >= 0){
            c->irq_mode = 2;
            io_vec = 1;                     // IV = numarul intrarii din tabel
        } else {
            pci_msix_disable(p);
            io_vec = -1;
        }
    }
    if(c->irq_mode == 0){
        int v = pci_msi_enable(p, io_fns[idx]);
        if(v >= 0){
            c->irq_mode = 1;
            io_vec = 0;
        }
    }

    // IDENTIFY controller
    if(!identify(c, 1, 0, data_pa)){
        kprintf("nvme%d: IDENTIFY controller a esuat\n", idx);
        ctrls[idx] = nullptr;
        return -1;
    }
    memcpy(c->serial, data + 4, 20);
    trim(c->serial, 20);
    memcpy(c->model, data + 24, 40);
    trim(c->model, 40);
    uint8_t mdts = data[77];
    if(mdts && mdts < 16 && (4096u << mdts) < c->max_xfer){
        c->max_xfer = 4096u << mdts;
    }
    uint32_t nn = *(uint32_t*)(data + 516);

    if(!create_queues(c, c->irq_mode ? io_vec : -1)){
        ctrls[idx] = nullptr;
        return -1;
    }
    c->q.init(queue_ops, c, c->irq_mode != 0);

    // Lista de namespace-uri active (CNS 2), altfel 1..NN
    uint32_t ids[MAX_NS];
    int nids = 0;
    if(identify(c, 2, 0, data_pa)){
        for(int i = 0; i < 1024 && nids < MAX_NS; i++){
            uint32_t id = ((uint32_t*)data)[i];
            if(!id){
                break;
            }
            ids[nids++] = id;
        }
    } else {
        for(uint32_t id = 1; id <= nn && nids < MAX_NS; id++){
            ids[nids++] = id;
        }
    }
    ctrl_count++;
    int found = 0;
    for(int i = 0; i < nids; i++){
        if(ns_total >= MAX_NS || c->nns >= MAX_NS){
            break;
        }
        NvmeNs* n = setup_ns(c, ids[i], data, data_pa);
        if(!n){
            continue;
        }
        c->ns[c->nns++] = n;
        all_ns[ns_total++] = n;
        found++;
        hal_register(HalKind::Block, n, n->name);
        kprintf("nvme: %s \"%s\" %lu sectoare x %u B (%lu MiB)\n", n->name, c->model, n->blocks, n->bs,
                n->blocks * n->bs >> 20);
        partition_register(n, n->name);
    }
    pmm_free_page(data_pa);
    kprintf("nvme: controller %02x:%02x.%x v%x.%x \"%s\", %d namespace(uri), %s\n", pci_bus(p), pci_slot(p),
            pci_fn(p), rd32(regs, R_VS) >> 16, (rd32(regs, R_VS) >> 8) & 0xFF, c->model, found,
            c->irq_mode == 2 ? "MSI-X" : c->irq_mode == 1 ? "MSI" : "interogare");
    return 0;
}

const PciDriver nvme_driver = {"nvme", PCI_ANY, PCI_ANY, 0x01, 0x08, nvme_probe};

}

void nvme_init(){
    pci_register_driver(&nvme_driver);
}

int nvme_controller_count(){
    return ctrl_count;
}

int nvme_ns_count(){
    return ns_total;
}

BlockDevice* nvme_ns(int idx){
    return idx >= 0 && idx < ns_total ? all_ns[idx] : nullptr;
}

uint32_t nvme_irq_count(){
    return irq_total;
}

int nvme_irq_mode(int ctrl){
    return ctrl >= 0 && ctrl < ctrl_count ? ctrls[ctrl]->irq_mode : 0;
}
