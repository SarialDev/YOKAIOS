#include "drivers/ahci/ahci.h"
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

// Registre HBA
const uint32_t HBA_CAP = 0x00;
const uint32_t HBA_GHC = 0x04;
const uint32_t HBA_IS = 0x08;
const uint32_t HBA_PI = 0x0C;
const uint32_t HBA_VS = 0x10;
const uint32_t CAP_S64A = 1u << 31;
const uint32_t GHC_HR = 1u << 0;
const uint32_t GHC_IE = 1u << 1;
const uint32_t GHC_AE = 1u << 31;

// Registre port (de la 0x100 + 0x80 * n)
const uint32_t PORT_BASE = 0x100;
const uint32_t PORT_STRIDE = 0x80;
const uint32_t P_CLB = 0x00;
const uint32_t P_CLBU = 0x04;
const uint32_t P_FB = 0x08;
const uint32_t P_FBU = 0x0C;
const uint32_t P_IS = 0x10;
const uint32_t P_IE = 0x14;
const uint32_t P_CMD = 0x18;
const uint32_t P_TFD = 0x20;
const uint32_t P_SIG = 0x24;
const uint32_t P_SSTS = 0x28;
const uint32_t P_SCTL = 0x2C;
const uint32_t P_SERR = 0x30;
const uint32_t P_CI = 0x38;

const uint32_t CMD_ST = 1u << 0;
const uint32_t CMD_SUD = 1u << 1;
const uint32_t CMD_POD = 1u << 2;
const uint32_t CMD_FRE = 1u << 4;
const uint32_t CMD_FR = 1u << 14;
const uint32_t CMD_CR = 1u << 15;

const uint32_t TFD_ERR = 1u << 0;
const uint32_t TFD_DRQ = 1u << 3;
const uint32_t TFD_DF = 1u << 5;
const uint32_t TFD_BSY = 1u << 7;

// PxIS / PxIE: D2H Register FIS, apoi erori (task file, host bus fatal/data, interface fatal, overflow)
const uint32_t IS_DHRS = 1u << 0;
const uint32_t IS_UFS = 1u << 4;
const uint32_t IS_OFS = 1u << 24;
const uint32_t IS_IFS = 1u << 27;
const uint32_t IS_HBDS = 1u << 28;
const uint32_t IS_HBFS = 1u << 29;
const uint32_t IS_TFES = 1u << 30;
const uint32_t IS_ERRORS = IS_OFS | IS_IFS | IS_HBDS | IS_HBFS | IS_TFES;
const uint32_t IE_MASK = IS_DHRS | IS_UFS | IS_ERRORS;

const uint32_t SIG_ATA = 0x00000101;

const uint8_t ATA_READ_DMA_EXT = 0x25;
const uint8_t ATA_WRITE_DMA_EXT = 0x35;
const uint8_t ATA_FLUSH_EXT = 0xEA;
const uint8_t ATA_IDENTIFY = 0xEC;

const int MAX_HBA = 2;
const int MAX_DISKS = 8;
const uint32_t MAX_XFER = 128 * 1024;                 // un request = cel mult 128 KiB
const int MAX_SEGS = (int)(MAX_XFER / PAGE_SIZE) + 2; // pagini atinse de un buffer nealiniat + 1
const uint32_t TABLE_OFF = 1280;                      // in pagina portului: lista 1 KiB, FIS 256 B, tabela (aliniata la 128)
const uint32_t FIS_OFF = 1024;

struct CommandHeader {
    uint32_t flags;             // CFL [4:0], W bit 6, PRDTL [31:16]
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
};

struct PrdtEntry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;               // octeti - 1 (bit 0 = 1), bit 31 = intrerupere la sfarsit
};

struct AhciHba;

struct AhciReq {
    BlockRequest base;          // primul membru: start() primeste BlockRequest*
    uint8_t ata_cmd;
    bool write;
    int nseg;
    uint64_t seg_pa[MAX_SEGS];
    uint32_t seg_len[MAX_SEGS];
    AhciReq(BlockOp op, uint64_t lba, uint32_t count, void* buf, uint8_t cmd)
        : base(op, lba, count, buf), ata_cmd(cmd), write(op == BlockOp::Write), nseg(0), seg_pa(), seg_len(){}
};

inline uint32_t rd32(volatile uint8_t* base, uint32_t off){
    return *(volatile uint32_t*)(base + off);
}

inline void wr32(volatile uint8_t* base, uint32_t off, uint32_t v){
    *(volatile uint32_t*)(base + off) = v;
}

bool wait_bits(volatile uint8_t* base, uint32_t off, uint32_t mask, uint32_t want, uint64_t ms){
    uint64_t end = monotonic_ns() + ms * 1000000;
    while((rd32(base, off) & mask) != want){
        if(monotonic_ns() > end){
            return false;
        }
        __builtin_ia32_pause();
    }
    return true;
}

class AhciDisk final : public BlockDevice {
public:
    AhciDisk() : regs(nullptr), hba(nullptr), port(0), bs(0), blocks(0), lba48(false), dead(false), name{}, model{},
                 page_pa(0), page(nullptr), q(){}
    uint32_t block_size() const override { return bs; }
    uint64_t block_count() const override { return blocks; }
    int read(uint64_t lba, uint32_t count, void* buf) override { return rw(false, lba, count, buf); }
    int write(uint64_t lba, uint32_t count, const void* buf) override { return rw(true, lba, count, (void*)buf); }
    int flush() override;

    // Cu lock-ul cozii tinut
    void service();
    void recover();
    int start(AhciReq* r);
    bool setup(AhciHba* h, int port_no, int disk_idx);
    bool identify();
    bool start_port();
    bool stop_port();

    volatile uint8_t* regs;
    AhciHba* hba;
    int port;
    uint32_t bs;
    uint64_t blocks;
    bool lba48;
    bool dead;
    char name[4];
    char model[41];
    uint64_t page_pa;
    uint8_t* page;              // lista de comenzi, FIS primit, tabela de comanda
    BlockQueue q;
private:
    int rw(bool write, uint64_t lba, uint32_t count, void* buf);
    bool map_buf(AhciReq& r, const void* buf, uint64_t bytes);
};

struct AhciHba {
    PciDevice* pci;
    volatile uint8_t* abar;
    uint32_t cap;
    uint32_t pi;
    int vector;
    AhciDisk* ports[32];
    int ndisks;
};

AhciHba* hbas[MAX_HBA];
int hba_count;
AhciDisk* disks[MAX_DISKS];
int disk_total;

volatile uint32_t irq_total;

void hba_irq(AhciHba* h){
    irq_total = irq_total + 1;
    uint32_t is = rd32(h->abar, HBA_IS);
    for(int pass = 0; pass < 2; pass++){
        for(int i = 0; i < 32; i++){
            if(!(is & (1u << i))){
                continue;
            }
            AhciDisk* d = h->ports[i];
            if(d){
                SpinlockGuard g(d->q.lock);
                d->service();
            } else {
                volatile uint8_t* p = h->abar + PORT_BASE + PORT_STRIDE * i;
                wr32(p, P_IS, rd32(p, P_IS));
            }
        }
        if(pass == 0){
            // Bitul din IS se sterge dupa PxIS; a doua trecere prinde un eveniment aparut intre cele doua stergeri
            wr32(h->abar, HBA_IS, is);
        }
    }
}

void irq0(InterruptFrame*){ if(hbas[0]) hba_irq(hbas[0]); }
void irq1(InterruptFrame*){ if(hbas[1]) hba_irq(hbas[1]); }
const InterruptHandler irq_fns[MAX_HBA] = {irq0, irq1};

int q_start(void* ctx, BlockRequest* r){
    return static_cast<AhciDisk*>(ctx)->start(reinterpret_cast<AhciReq*>(r));
}

void q_poll(void* ctx){
    static_cast<AhciDisk*>(ctx)->service();
}

void q_abort(void* ctx){
    static_cast<AhciDisk*>(ctx)->recover();
}

const BlockQueueOps queue_ops = {q_start, q_poll, q_abort};

bool AhciDisk::stop_port(){
    uint32_t cmd = rd32(regs, P_CMD);
    wr32(regs, P_CMD, cmd & ~CMD_ST);
    if(!wait_bits(regs, P_CMD, CMD_CR, 0, 500)){
        return false;
    }
    cmd = rd32(regs, P_CMD);
    if(cmd & (CMD_FRE | CMD_FR)){
        wr32(regs, P_CMD, cmd & ~CMD_FRE);
        return wait_bits(regs, P_CMD, CMD_FR, 0, 500);
    }
    return true;
}

bool AhciDisk::start_port(){
    wr32(regs, P_SERR, 0xFFFFFFFF);
    wr32(regs, P_IS, 0xFFFFFFFF);
    wr32(regs, P_CMD, rd32(regs, P_CMD) | CMD_FRE);
    if(!wait_bits(regs, P_TFD, TFD_BSY | TFD_DRQ, 0, 1000)){
        return false;
    }
    wr32(regs, P_CMD, rd32(regs, P_CMD) | CMD_ST);
    return true;
}

// Dupa o eroare: opreste/porneste portul; daca dispozitivul ramane ocupat, face COMRESET; altfel il marcheaza mort
void AhciDisk::recover(){
    stop_port();
    if(!start_port()){
        wr32(regs, P_SCTL, (rd32(regs, P_SCTL) & ~0xFu) | 1);
        uint64_t end = monotonic_ns() + 2000000;
        while(monotonic_ns() < end){
            __builtin_ia32_pause();
        }
        wr32(regs, P_SCTL, rd32(regs, P_SCTL) & ~0xFu);
        wait_bits(regs, P_SSTS, 0xF, 3, 200);
        stop_port();
        if(!start_port()){
            dead = true;
            kprintf("ahci: %s nu raspunde, dezactivat\n", name);
        }
    }
}

// Dupa un request pornit: terminat (CI clar) sau eroare. Citeste si sterge PxIS
void AhciDisk::service(){
    uint32_t is = rd32(regs, P_IS);
    if(is){
        wr32(regs, P_IS, is);
    }
    if(!q.current_locked()){
        return;
    }
    uint32_t ci = rd32(regs, P_CI);
    uint32_t tfd = rd32(regs, P_TFD);
    if(is & IS_ERRORS){
        kprintf("ahci: %s eroare PxIS=%x TFD=%x\n", name, is, tfd);
        recover();
        q.finish_locked(-EIO);
    } else if(!(ci & 1)){
        q.finish_locked((tfd & (TFD_ERR | TFD_DF)) ? -EIO : 0);
    }
}

// Un request pe slotul 0. Lock-ul cozii tinut, deci fara alocari si fara vmm
int AhciDisk::start(AhciReq* r){
    if(dead){
        return -EIO;
    }
    CommandHeader* h = (CommandHeader*)page;
    uint8_t* table = page + TABLE_OFF;
    memset(h, 0, sizeof(*h));
    memset(table, 0, 128);
    uint8_t* fis = table;
    fis[0] = 0x27;                              // Register H2D
    fis[1] = 0x80;                              // C = comanda
    fis[2] = r->ata_cmd;
    uint64_t lba = r->base.lba;
    if(r->ata_cmd != ATA_IDENTIFY){
        fis[7] = 0x40;                          // LBA
        fis[4] = (uint8_t)lba;
        fis[5] = (uint8_t)(lba >> 8);
        fis[6] = (uint8_t)(lba >> 16);
        fis[8] = (uint8_t)(lba >> 24);
        fis[9] = (uint8_t)(lba >> 32);
        fis[10] = (uint8_t)(lba >> 40);
        fis[12] = (uint8_t)r->base.count;
        fis[13] = (uint8_t)(r->base.count >> 8);
    }
    PrdtEntry* prdt = (PrdtEntry*)(table + 128);
    for(int i = 0; i < r->nseg; i++){
        prdt[i].dba = (uint32_t)r->seg_pa[i];
        prdt[i].dbau = (uint32_t)(r->seg_pa[i] >> 32);
        prdt[i].reserved = 0;
        prdt[i].dbc = (r->seg_len[i] - 1) | (i == r->nseg - 1 ? 1u << 31 : 0);
    }
    uint64_t table_pa = page_pa + TABLE_OFF;
    h->flags = 5 | (r->write ? 1u << 6 : 0) | ((uint32_t)r->nseg << 16);
    h->ctba = (uint32_t)table_pa;
    h->ctbau = (uint32_t)(table_pa >> 32);
    __sync_synchronize();
    wr32(regs, P_IS, 0xFFFFFFFF);
    wr32(regs, P_CI, 1);
    return 0;
}

// Scatter/gather peste paginile buffer-ului. Se cheama din task (vmm_translate ia un lock), nu din IRQ
bool AhciDisk::map_buf(AhciReq& r, const void* buf, uint64_t bytes){
    uint64_t v = (uint64_t)(uintptr_t)buf;
    if((v & 1) || (bytes & 1)){
        return false;
    }
    while(bytes){
        uint64_t chunk = PAGE_SIZE - (v & (PAGE_SIZE - 1));
        if(chunk > bytes){
            chunk = bytes;
        }
        uint64_t pa = vmm_translate(kernel_as, v);
        if(!pa || (!(hba->cap & CAP_S64A) && pa + chunk > (1UL << 32))){
            return false;
        }
        if(r.nseg && r.seg_pa[r.nseg - 1] + r.seg_len[r.nseg - 1] == pa){
            r.seg_len[r.nseg - 1] += (uint32_t)chunk;
        } else {
            if(r.nseg == MAX_SEGS){
                return false;
            }
            r.seg_pa[r.nseg] = pa;
            r.seg_len[r.nseg] = (uint32_t)chunk;
            r.nseg++;
        }
        v += chunk;
        bytes -= chunk;
    }
    return true;
}

int AhciDisk::rw(bool is_write, uint64_t lba, uint32_t count, void* buf){
    if(count == 0){
        return 0;
    }
    if(lba >= blocks || count > blocks - lba){
        return -EINVAL;
    }
    uint8_t* p = (uint8_t*)buf;
    uint32_t max_n = MAX_XFER / bs;
    while(count){
        uint32_t n = count < max_n ? count : max_n;
        AhciReq r(is_write ? BlockOp::Write : BlockOp::Read, lba, n, p, is_write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT);
        if(!map_buf(r, p, (uint64_t)n * bs)){
            return -EINVAL;
        }
        int err = q.submit_wait(&r.base);
        if(err){
            return err;
        }
        lba += n;
        count -= n;
        p += (uint64_t)n * bs;
    }
    return 0;
}

int AhciDisk::flush(){
    AhciReq r(BlockOp::Flush, 0, 0, nullptr, ATA_FLUSH_EXT);
    return q.submit_wait(&r.base);
}

bool AhciDisk::identify(){
    uint64_t pa = pmm_alloc_page();
    if(!pa){
        return false;
    }
    uint16_t* w = (uint16_t*)phys_to_virt(pa);
    memset(w, 0, 512);
    AhciReq r(BlockOp::Read, 0, 1, w, ATA_IDENTIFY);
    r.nseg = 1;
    r.seg_pa[0] = pa;
    r.seg_len[0] = 512;
    bool ok = q.submit_wait(&r.base, 2000) == 0 && !(w[0] & 0x8000);
    if(ok){
        lba48 = w[83] & (1 << 10);
        blocks = (uint64_t)w[100] | ((uint64_t)w[101] << 16) | ((uint64_t)w[102] << 32) | ((uint64_t)w[103] << 48);
        bs = 512;
        if((w[106] & 0xC000) == 0x4000 && (w[106] & (1 << 12))){
            bs = ((uint32_t)w[117] | ((uint32_t)w[118] << 16)) * 2;
        }
        for(int i = 0; i < 20; i++){
            model[2 * i] = (char)(w[27 + i] >> 8);
            model[2 * i + 1] = (char)(w[27 + i] & 0xFF);
        }
        model[40] = 0;
        for(int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--){
            model[i] = 0;
        }
    }
    pmm_free_page(pa);
    return ok;
}

// Initializeaza portul n; nullptr daca nu are un disc ATA utilizabil
bool AhciDisk::setup(AhciHba* h, int port_no, int disk_idx){
    hba = h;
    port = port_no;
    regs = h->abar + PORT_BASE + PORT_STRIDE * port_no;
    name[0] = 's';
    name[1] = 'd';
    name[2] = (char)('a' + disk_idx);
    name[3] = 0;

    uint32_t ssts = rd32(regs, P_SSTS);
    if((ssts & 0xF) == 1){
        wait_bits(regs, P_SSTS, 0xF, 3, 100);
        ssts = rd32(regs, P_SSTS);
    }
    if((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1){
        return false;
    }
    if(!stop_port()){
        return false;
    }
    page_pa = pmm_alloc_page();
    if(!page_pa || (!(h->cap & CAP_S64A) && page_pa >= (1UL << 32))){
        return false;
    }
    page = (uint8_t*)phys_to_virt(page_pa);
    memset(page, 0, PAGE_SIZE);
    wr32(regs, P_CLB, (uint32_t)page_pa);
    wr32(regs, P_CLBU, (uint32_t)(page_pa >> 32));
    wr32(regs, P_FB, (uint32_t)(page_pa + FIS_OFF));
    wr32(regs, P_FBU, (uint32_t)((page_pa + FIS_OFF) >> 32));
    wr32(regs, P_CMD, rd32(regs, P_CMD) | CMD_POD | CMD_SUD);
    wr32(regs, P_SERR, 0xFFFFFFFF);
    wr32(regs, P_IS, 0xFFFFFFFF);
    wr32(regs, P_CMD, rd32(regs, P_CMD) | CMD_FRE);
    if(!wait_bits(regs, P_TFD, TFD_BSY | TFD_DRQ, 0, 1000)){
        kprintf("ahci: porta %d ocupata, ignorata\n", port_no);
        return false;
    }
    uint32_t sig = rd32(regs, P_SIG);
    if(sig != SIG_ATA){
        kprintf("ahci: porta %d semnatura %x (ATAPI/alt tip), ignorata\n", port_no, sig);
        wr32(regs, P_CMD, rd32(regs, P_CMD) & ~CMD_FRE);
        return false;
    }
    wr32(regs, P_CMD, rd32(regs, P_CMD) | CMD_ST);
    q.init(queue_ops, this, h->vector >= 0);
    wr32(regs, P_IE, IE_MASK);
    if(!identify()){
        kprintf("ahci: IDENTIFY a esuat pe portul %d\n", port_no);
        stop_port();
        wr32(regs, P_IE, 0);
        return false;
    }
    if(!lba48 || !blocks || bs < 512 || bs > PAGE_SIZE || (bs & (bs - 1))){
        kprintf("ahci: portul %d: fara LBA48 sau sector neobisnuit (%u), ignorat\n", port_no, bs);
        stop_port();
        wr32(regs, P_IE, 0);
        return false;
    }
    return true;
}

int ahci_probe(PciDevice* p){
    if(p->progif != 0x01 || hba_count >= MAX_HBA){
        return -1;
    }
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_MASTER);
    volatile uint8_t* abar = (volatile uint8_t*)pci_map_bar(p, 5);
    if(!abar){
        return -1;
    }
    AhciHba* h = new AhciHba;
    memset(h, 0, sizeof(*h));
    h->pci = p;
    h->abar = abar;
    h->vector = -1;

    wr32(abar, HBA_GHC, GHC_AE);
    wr32(abar, HBA_GHC, GHC_AE | GHC_HR);
    if(!wait_bits(abar, HBA_GHC, GHC_HR, 0, 1000)){
        kprintf("ahci: reset HBA a esuat\n");
        return -1;
    }
    wr32(abar, HBA_GHC, GHC_AE);
    h->cap = rd32(abar, HBA_CAP);
    h->pi = rd32(abar, HBA_PI);
    wr32(abar, HBA_IS, 0xFFFFFFFF);

    int idx = hba_count;
    hbas[idx] = h;
    h->vector = pci_msi_enable(p, irq_fns[idx]);
    if(h->vector >= 0){
        wr32(abar, HBA_GHC, rd32(abar, HBA_GHC) | GHC_IE);
    }

    int found = 0;
    for(int n = 0; n < 32; n++){
        if(!(h->pi & (1u << n)) || disk_total >= MAX_DISKS){
            continue;
        }
        AhciDisk* d = new AhciDisk;
        if(!d->setup(h, n, disk_total)){
            continue;
        }
        h->ports[n] = d;
        disks[disk_total++] = d;
        found++;
        hal_register(HalKind::Block, d, d->name);
        kprintf("ahci: %s \"%s\" %lu sectoare x %u B (%lu MiB)\n", d->name, d->model, d->blocks, d->bs,
                d->blocks * d->bs >> 20);
        partition_register(d, d->name);
    }
    hba_count++;
    kprintf("ahci: HBA %02x:%02x.%x v%x.%02x, %u porturi, %d disc(uri), %s\n", pci_bus(p), pci_slot(p), pci_fn(p),
            rd32(abar, HBA_VS) >> 16, (rd32(abar, HBA_VS) >> 4 & 0xF) * 10 + (rd32(abar, HBA_VS) & 0xF),
            (h->cap & 0x1F) + 1, found, h->vector >= 0 ? "MSI" : "interogare");
    return 0;
}

const PciDriver ahci_driver = {"ahci", PCI_ANY, PCI_ANY, 0x01, 0x06, ahci_probe};

}

void ahci_init(){
    pci_register_driver(&ahci_driver);
}

int ahci_controller_count(){
    return hba_count;
}

int ahci_disk_count(){
    return disk_total;
}

BlockDevice* ahci_disk(int idx){
    return idx >= 0 && idx < disk_total ? disks[idx] : nullptr;
}

uint32_t ahci_irq_count(){
    return irq_total;
}
