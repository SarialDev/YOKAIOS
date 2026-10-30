#include "fs/pseudofs.h"
#include "fs/vfs/vfs.h"
#include "arch/x86_64/paging.h"
#include "arch/x86_64/percpu.h"
#include "exec/elf.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "sched/task.h"
#include "time/clock.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace {

// Continutul se genereaza la fiecare read intr-un buffer de kernel (trunchiat la GEN_MAX); inode-urile se creeaza la
// lookup/readdir si dispar la ultima referinta (sb-ul are SB_NOCACHE, deci nici dentry-urile nu raman: un proces
// terminat dispare din /proc). Un inode din /proc/<pid> tine doar numarul; la citire se cauta task-ul din nou
enum class Kind : uint8_t {
    Root, Meminfo, Uptime, Cpuinfo, Mounts, Self, PidDir, PidStatus, PidCmdline, PidMaps
};

const uint64_t GEN_MAX = 8192;
const uint64_t KB_PER_PAGE = PAGE_SIZE / 1024;
const uint32_t MODE_DIR = S_IFDIR | 0555;
const uint32_t MODE_FILE = S_IFREG | 0444;
const uint32_t MODE_LINK = S_IFLNK | 0777;

struct Static {
    const char* name;
    Kind kind;
};

// Intrarile fixe din /proc, in ordinea din readdir (dupa ele vine "self", apoi procesele)
const Static statics[] = {
    {"meminfo", Kind::Meminfo}, {"uptime", Kind::Uptime}, {"cpuinfo", Kind::Cpuinfo}, {"mounts", Kind::Mounts},
};
const uint64_t NSTATIC = sizeof(statics) / sizeof(statics[0]);

const Static pid_files[] = {
    {"status", Kind::PidStatus}, {"cmdline", Kind::PidCmdline}, {"maps", Kind::PidMaps},
};
const uint64_t NPID_FILES = sizeof(pid_files) / sizeof(pid_files[0]);

extern const InodeOps root_ops;
extern const InodeOps pid_ops;
extern const InodeOps file_ops;
extern const InodeOps link_ops;

// ---- buffer de iesire ----

struct Buf {
    char* p;
    uint64_t cap;
    uint64_t len;
};

void put_n(Buf& b, const char* s, uint64_t n){
    if(n > b.cap - b.len){
        n = b.cap - b.len;
    }
    memcpy(b.p + b.len, s, n);
    b.len += n;
}

void put(Buf& b, const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    put_n(b, s, n);
}

void put_c(Buf& b, char c){
    put_n(b, &c, 1);
}

void put_dec(Buf& b, uint64_t v, int width = 0, char fill = ' '){
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while(v);
    for(int i = n; i < width; i++){
        put_c(b, fill);
    }
    while(n){
        put_c(b, tmp[--n]);
    }
}

void put_hex(Buf& b, uint64_t v, int width){
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while(v);
    for(int i = n; i < width; i++){
        put_c(b, '0');
    }
    while(n){
        put_c(b, tmp[--n]);
    }
}

// Spatii pana cand linia curenta (de la start) are col caractere
void pad_to(Buf& b, uint64_t start, uint64_t col){
    while(b.len - start < col && b.len < b.cap){
        put_c(b, ' ');
    }
}

// "Nume:" completat cu spatii pana la 15, un spatiu si valoarea pe 8 coloane, " kB" (ca /proc/meminfo)
void put_kb(Buf& b, const char* name, uint64_t kb){
    uint64_t start = b.len;
    put(b, name);
    put_c(b, ':');
    pad_to(b, start, 15);
    put_c(b, ' ');
    put_dec(b, kb, 8);
    put(b, " kB\n");
}

// ---- /proc/meminfo, /proc/uptime ----

void gen_meminfo(Buf& b){
    uint64_t total = pmm_total_pages() * KB_PER_PAGE;
    uint64_t free_kb = pmm_free_count() * KB_PER_PAGE;
    put_kb(b, "MemTotal", total);
    put_kb(b, "MemFree", free_kb);
    put_kb(b, "MemAvailable", free_kb);
    put_kb(b, "Buffers", 0);
    put_kb(b, "Cached", 0);
    put_kb(b, "SwapCached", 0);
    put_kb(b, "SwapTotal", 0);
    put_kb(b, "SwapFree", 0);
}

// "<secunde de la boot> <timp idle>"; timpul idle nu se contorizeaza (0.00)
void gen_uptime(Buf& b){
    uint64_t ms = uptime_ms();
    put_dec(b, ms / 1000);
    put_c(b, '.');
    put_dec(b, ms % 1000 / 10, 2, '0');
    put(b, " 0.00\n");
}

// ---- /proc/cpuinfo ----

void cpuid(uint32_t leaf, uint32_t& a, uint32_t& bx, uint32_t& c, uint32_t& d){
    __asm__ volatile("cpuid" : "=a"(a), "=b"(bx), "=c"(c), "=d"(d) : "a"(leaf), "c"(0));
}

struct Flag {
    uint32_t bit;
    const char* name;
};

void put_flags(Buf& b, uint32_t reg, const Flag* flags, uint64_t n){
    for(uint64_t i = 0; i < n; i++){
        if(reg & (1u << flags[i].bit)){
            put_c(b, ' ');
            put(b, flags[i].name);
        }
    }
}

void gen_cpuinfo(Buf& b){
    uint32_t a, bx, c, d;
    cpuid(0, a, bx, c, d);
    uint32_t max_leaf = a;
    char vendor[13];
    memcpy(vendor, &bx, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;

    uint32_t sig = 0, ecx1 = 0, edx1 = 0;
    if(max_leaf >= 1){
        cpuid(1, sig, bx, ecx1, edx1);
    }
    uint32_t family = (sig >> 8) & 0xF;
    uint32_t model = (sig >> 4) & 0xF;
    if(family == 0xF){
        family += (sig >> 20) & 0xFF;
    }
    if(family == 0x6 || family >= 0xF){
        model |= ((sig >> 16) & 0xF) << 4;
    }

    cpuid(0x80000000, a, bx, c, d);
    uint32_t max_ext = a;
    char brand[49] = {};
    if(max_ext >= 0x80000004){
        for(uint32_t i = 0; i < 3; i++){
            uint32_t r[4];
            cpuid(0x80000002 + i, r[0], r[1], r[2], r[3]);
            memcpy(brand + i * 16, r, 16);
        }
    }
    const char* name = brand;
    while(*name == ' '){
        name++;
    }
    uint32_t edx_ext = 0;
    if(max_ext >= 0x80000001){
        cpuid(0x80000001, a, bx, c, edx_ext);
    }

    static const Flag edx_flags[] = {
        {0, "fpu"}, {3, "pse"}, {4, "tsc"}, {5, "msr"}, {6, "pae"}, {8, "cx8"}, {9, "apic"}, {11, "sep"},
        {13, "pge"}, {15, "cmov"}, {16, "pat"}, {23, "mmx"}, {24, "fxsr"}, {25, "sse"}, {26, "sse2"},
    };
    static const Flag ecx_flags[] = {
        {0, "pni"}, {9, "ssse3"}, {19, "sse4_1"}, {20, "sse4_2"}, {21, "x2apic"}, {23, "popcnt"},
        {30, "rdrand"}, {31, "hypervisor"},
    };
    static const Flag ext_flags[] = {{20, "nx"}, {29, "lm"}};

    uint64_t hz = clock_tsc_hz();
    uint32_t ncpu = smp_cpu_count();
    for(uint32_t i = 0; i < ncpu; i++){
        put(b, "processor\t: ");
        put_dec(b, i);
        put(b, "\nvendor_id\t: ");
        put(b, vendor);
        put(b, "\ncpu family\t: ");
        put_dec(b, family);
        put(b, "\nmodel\t\t: ");
        put_dec(b, model);
        put(b, "\nmodel name\t: ");
        put(b, *name ? name : vendor);
        put(b, "\nstepping\t: ");
        put_dec(b, sig & 0xF);
        put(b, "\ncpu MHz\t\t: ");
        put_dec(b, hz / 1000000);
        put_c(b, '.');
        put_dec(b, hz / 1000 % 1000, 3, '0');
        put(b, "\nflags\t\t:");
        put_flags(b, edx1, edx_flags, sizeof(edx_flags) / sizeof(edx_flags[0]));
        put_flags(b, ecx1, ecx_flags, sizeof(ecx_flags) / sizeof(ecx_flags[0]));
        put_flags(b, edx_ext, ext_flags, sizeof(ext_flags) / sizeof(ext_flags[0]));
        put(b, "\n\n");
    }
}

// ---- /proc/mounts ----

void gen_mounts(Buf& b){
    MountInfo* list = (MountInfo*)kmalloc(sizeof(MountInfo) * MOUNT_SNAPSHOT_MAX);
    if(!list){
        return;
    }
    uint64_t n;
    {
        MutexGuard g(vfs_mutex());
        n = mount_snapshot(list, MOUNT_SNAPSHOT_MAX);
    }
    for(uint64_t i = 0; i < n; i++){
        put(b, list[i].dev_name[0] ? list[i].dev_name : "none");
        put_c(b, ' ');
        put(b, list[i].path);
        put_c(b, ' ');
        put(b, list[i].fstype);
        put(b, (list[i].flags & MS_RDONLY) ? " ro" : " rw");
        put(b, " 0 0\n");
    }
    kfree(list);
}

// ---- spatiul user al unui proces: regiuni citite din tabelele de pagini ----

struct Region {
    uint64_t start;
    uint64_t end;
    bool w;
    bool x;
};

const uint64_t MAX_REGIONS = 128;

struct Maps {
    Region r[MAX_REGIONS];
    uint64_t count;
    uint64_t pages;                 // toate paginile user prezente, si cele din regiuni pierdute prin trunchiere
};

void add_range(Maps& m, uint64_t start, uint64_t len, uint64_t entry){
    bool w = entry & PTE_RW;
    bool x = !(entry & PTE_NX);
    m.pages += len / PAGE_SIZE;
    if(m.count && m.r[m.count - 1].end == start && m.r[m.count - 1].w == w && m.r[m.count - 1].x == x){
        m.r[m.count - 1].end = start + len;
    } else if(m.count < MAX_REGIONS){
        m.r[m.count++] = Region{start, start + len, w, x};
    }
}

uint64_t* table_at(uint64_t entry){
    uint64_t phys = entry & PTE_ADDR_MASK;
    return phys < direct_map_limit ? (uint64_t*)phys_to_virt(phys) : nullptr;
}

// Parcurge jumatatea user a lui pml4 in ordinea adreselor. Nu se blocheaza spatiul: daca procesul isi inlocuieste
// imaginea (execve) sau iese exact acum, tabelele pot fi eliberate si rezultatul e gunoi (dar nu crapa: se citesc
// doar pagini din direct map)
void walk_user(uint64_t pml4_phys, Maps& m){
    m.count = 0;
    m.pages = 0;
    if(!pml4_phys || pml4_phys >= direct_map_limit){
        return;
    }
    const uint64_t* pml4 = (const uint64_t*)phys_to_virt(pml4_phys);
    for(uint64_t i4 = 0; i4 < 256; i4++){
        if(!(pml4[i4] & PTE_PRESENT)){
            continue;
        }
        const uint64_t* pdpt = table_at(pml4[i4]);
        for(uint64_t i3 = 0; pdpt && i3 < 512; i3++){
            uint64_t e3 = pdpt[i3];
            uint64_t base3 = (i4 << 39) | (i3 << 30);
            if(!(e3 & PTE_PRESENT) || !(e3 & PTE_USER)){
                continue;
            }
            if(e3 & PTE_HUGE){
                add_range(m, base3, PAGE_1G, e3);
                continue;
            }
            const uint64_t* pd = table_at(e3);
            for(uint64_t i2 = 0; pd && i2 < 512; i2++){
                uint64_t e2 = pd[i2];
                uint64_t base2 = base3 | (i2 << 21);
                if(!(e2 & PTE_PRESENT) || !(e2 & PTE_USER)){
                    continue;
                }
                if(e2 & PTE_HUGE){
                    add_range(m, base2, PAGE_2M, e2);
                    continue;
                }
                const uint64_t* pt = table_at(e2);
                for(uint64_t i1 = 0; pt && i1 < 512; i1++){
                    uint64_t e1 = pt[i1];
                    if((e1 & PTE_PRESENT) && (e1 & PTE_USER)){
                        add_range(m, base2 | (i1 << 12), PAGE_SIZE, e1);
                    }
                }
            }
        }
    }
}

// ---- /proc/<pid>/... ----

const char* state_name(const TaskInfo& t){
    if(t.zombie){
        return "Z (zombie)";
    }
    return t.state == TaskState::BLOCKED ? "S (sleeping)" : "R (running)";
}

void gen_status(Buf& b, const TaskInfo& t, const Maps* m){
    put(b, "Name:\t");
    put(b, t.name);
    put(b, "\nState:\t");
    put(b, state_name(t));
    put(b, "\nTgid:\t");
    put_dec(b, t.pid);
    put(b, "\nPid:\t");
    put_dec(b, t.pid);
    put(b, "\nPPid:\t");
    put_dec(b, t.ppid);
    put(b, "\nThreads:\t1\n");
    if(m){
        put(b, "VmSize:\t");
        put_dec(b, m->pages * KB_PER_PAGE, 8);
        put(b, " kB\nVmRSS:\t");
        put_dec(b, m->pages * KB_PER_PAGE, 8);
        put(b, " kB\n");
    }
    put(b, "Cpu:\t");
    put_dec(b, t.cpu);
    put(b, "\nSchedClass:\t");
    put(b, t.cls == TaskClass::RT ? "rt" : "normal");
    put(b, "\nPrio:\t");
    put_dec(b, t.prio);
    put_c(b, '\n');
}

void gen_maps(Buf& b, const Maps& m){
    const uint64_t stack_lo = USER_STACK_TOP - USER_STACK_SIZE;
    for(uint64_t i = 0; i < m.count; i++){
        const Region& r = m.r[i];
        uint64_t start = b.len;
        put_hex(b, r.start, 8);
        put_c(b, '-');
        put_hex(b, r.end, 8);
        put(b, " r");
        put_c(b, r.w ? 'w' : '-');
        put_c(b, r.x ? 'x' : '-');
        put(b, "p 00000000 00:00 0");
        if(r.start >= stack_lo && r.end <= USER_STACK_TOP){
            pad_to(b, start, 73);
            put(b, "[stack]");
        }
        put_c(b, '\n');
    }
}

// pid-ul procesului curent (tid-ul la thread-urile de kernel)
uint64_t current_pid(){
    Task* t = task_current();
    return t->pid ? t->pid : t->tid;
}

int generate(Kind kind, uint64_t pid, Buf& b){
    switch(kind){
    case Kind::Meminfo:
        gen_meminfo(b);
        return 0;
    case Kind::Uptime:
        gen_uptime(b);
        return 0;
    case Kind::Cpuinfo:
        gen_cpuinfo(b);
        return 0;
    case Kind::Mounts:
        gen_mounts(b);
        return 0;
    case Kind::PidStatus:
    case Kind::PidCmdline:
    case Kind::PidMaps: {
        TaskInfo* t = (TaskInfo*)kmalloc(sizeof(TaskInfo));
        Maps* m = nullptr;
        if(!t){
            return -ENOMEM;
        }
        if(!task_info(pid, *t)){
            kfree(t);
            return -ENOENT;
        }
        if(t->user && kind != Kind::PidCmdline){
            m = (Maps*)kmalloc(sizeof(Maps));
            if(!m){
                kfree(t);
                return -ENOMEM;
            }
            walk_user(t->pml4_phys, *m);
        }
        if(kind == Kind::PidStatus){
            gen_status(b, *t, m);
        } else if(kind == Kind::PidCmdline){
            put_n(b, t->cmdline, t->cmdline_len);
        } else if(m){
            gen_maps(b, *m);
        }
        kfree(m);
        kfree(t);
        return 0;
    }
    default:
        return -EINVAL;
    }
}

// ---- inode-uri ----

void* pack(Kind kind, uint64_t pid){
    return (void*)((uint64_t)kind << 56 | pid);
}

Kind kind_of(const Inode* i){
    return (Kind)((uint64_t)i->priv >> 56);
}

uint64_t pid_of(const Inode* i){
    return (uint64_t)i->priv & ((1UL << 56) - 1);
}

Inode* make_inode(SuperBlock* sb, Kind kind, uint64_t pid){
    uint32_t mode = MODE_FILE;
    const InodeOps* ops = &file_ops;
    switch(kind){
    case Kind::Root:
        mode = MODE_DIR;
        ops = &root_ops;
        break;
    case Kind::PidDir:
        mode = MODE_DIR;
        ops = &pid_ops;
        break;
    case Kind::Self:
        mode = MODE_LINK;
        ops = &link_ops;
        break;
    default:
        break;
    }
    Inode* i = inode_alloc(sb, ((pid << 4) | (uint64_t)kind) + 1, mode, ops);
    if(i){
        i->priv = pack(kind, pid);
        if(S_ISDIR(mode)){
            i->nlink = 2;
        }
    }
    return i;
}

bool parse_pid(const char* name, uint32_t len, uint64_t& pid){
    if(len == 0 || len > 12){
        return false;
    }
    pid = 0;
    for(uint32_t k = 0; k < len; k++){
        if(name[k] < '0' || name[k] > '9'){
            return false;
        }
        pid = pid * 10 + (uint64_t)(name[k] - '0');
    }
    return len == 1 || name[0] != '0';
}

bool name_is(const char* want, const char* name, uint32_t len){
    uint32_t n = 0;
    while(want[n]){
        n++;
    }
    return n == len && memcmp(want, name, len) == 0;
}

void fill_entry(DirEntry& out, uint64_t ino, uint8_t type, const char* name){
    uint32_t len = 0;
    while(name[len]){
        len++;
    }
    out.ino = ino;
    out.type = type;
    out.name_len = (uint8_t)len;
    memcpy(out.name, name, len + 1);
}

int root_lookup(Inode* dir, const char* name, uint32_t len, Inode*& out){
    Kind kind = Kind::Root;
    uint64_t pid = 0;
    for(uint64_t k = 0; k < NSTATIC; k++){
        if(name_is(statics[k].name, name, len)){
            kind = statics[k].kind;
        }
    }
    if(name_is("self", name, len)){
        kind = Kind::Self;
    } else if(kind == Kind::Root && parse_pid(name, len, pid)){
        TaskInfo* t = (TaskInfo*)kmalloc(sizeof(TaskInfo));
        if(!t){
            return -ENOMEM;
        }
        bool found = task_info(pid, *t);
        kfree(t);
        if(!found){
            return -ENOENT;
        }
        kind = Kind::PidDir;
    }
    if(kind == Kind::Root){
        return -ENOENT;
    }
    out = make_inode(dir->sb, kind, pid);
    return out ? 0 : -ENOMEM;
}

int root_readdir(Inode* dir, uint64_t index, DirEntry& out){
    if(index < NSTATIC){
        fill_entry(out, ((uint64_t)statics[index].kind) + 1, DT_REG, statics[index].name);
        return 1;
    }
    if(index == NSTATIC){
        fill_entry(out, ((uint64_t)Kind::Self) + 1, DT_LNK, "self");
        return 1;
    }
    TaskInfo* t = (TaskInfo*)kmalloc(sizeof(TaskInfo));
    if(!t){
        return -ENOMEM;
    }
    int r = 0;
    if(task_nth(index - NSTATIC - 1, *t)){
        char num[24];
        int n = 0;
        uint64_t v = t->pid;
        do {
            num[n++] = (char)('0' + v % 10);
            v /= 10;
        } while(v);
        char name[24];
        for(int k = 0; k < n; k++){
            name[k] = num[n - 1 - k];
        }
        name[n] = 0;
        fill_entry(out, ((t->pid << 4) | (uint64_t)Kind::PidDir) + 1, DT_DIR, name);
        r = 1;
    }
    kfree(t);
    (void)dir;
    return r;
}

int pid_lookup(Inode* dir, const char* name, uint32_t len, Inode*& out){
    for(uint64_t k = 0; k < NPID_FILES; k++){
        if(name_is(pid_files[k].name, name, len)){
            out = make_inode(dir->sb, pid_files[k].kind, pid_of(dir));
            return out ? 0 : -ENOMEM;
        }
    }
    return -ENOENT;
}

int pid_readdir(Inode* dir, uint64_t index, DirEntry& out){
    if(index >= NPID_FILES){
        return 0;
    }
    fill_entry(out, ((pid_of(dir) << 4) | (uint64_t)pid_files[index].kind) + 1, DT_REG, pid_files[index].name);
    return 1;
}

int64_t proc_read(Inode* i, uint64_t pos, void* buf, uint64_t len){
    char* tmp = (char*)kmalloc(GEN_MAX);
    if(!tmp){
        return -ENOMEM;
    }
    Buf b = {tmp, GEN_MAX, 0};
    int err = generate(kind_of(i), pid_of(i), b);
    int64_t n = err;
    if(!err && pos < b.len){
        n = (int64_t)(b.len - pos < len ? b.len - pos : len);
        memcpy(buf, tmp + pos, (uint64_t)n);
    } else if(!err){
        n = 0;
    }
    kfree(tmp);
    return n;
}

// /proc/self -> pid-ul celui care citeste
int64_t proc_readlink(Inode*, char* buf, uint64_t len){
    char num[24];
    int n = 0;
    uint64_t v = current_pid();
    do {
        num[n++] = (char)('0' + v % 10);
        v /= 10;
    } while(v);
    if(len > (uint64_t)n){
        len = (uint64_t)n;
    }
    for(uint64_t k = 0; k < len; k++){
        buf[k] = num[n - 1 - (int)k];
    }
    return (int64_t)len;
}

const InodeOps root_ops = {root_lookup, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           root_readdir, nullptr, nullptr};
const InodeOps pid_ops = {pid_lookup, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                          pid_readdir, nullptr, nullptr};
const InodeOps file_ops = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, proc_read, nullptr, nullptr,
                           nullptr, nullptr, nullptr};
const InodeOps link_ops = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, proc_readlink, nullptr};

// Fara cache: inode-urile dispar cu dentry-urile care le tin (radacina e eliberata de kill)
void proc_release(Inode* i){
    if(i->sb->root_inode != i){
        inode_free(i);
    }
}

void proc_kill(SuperBlock* sb){
    inode_free(sb->root_inode);
}

const SuperBlockOps proc_sb_ops = {proc_release, proc_kill};

int proc_mount(FileSystemType*, SuperBlock* sb, const char*, const char*){
    Inode* root = make_inode(sb, Kind::Root, 0);
    if(!root){
        return -ENOMEM;
    }
    sb->ops = &proc_sb_ops;
    sb->flags |= SB_NOCACHE | MS_RDONLY;
    sb->root_inode = root;
    return 0;
}

FileSystemType procfs_type = {"proc", proc_mount, nullptr};

}

void procfs_init(){
    fs_register(&procfs_type);
}
