#include "tests/test_procfs.h"
#include "uapi/syscall_nr.h"
#include "fs/vfs/vfs.h"
#include "arch/x86_64/percpu.h"
#include "exec/elf.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/string.h"
#include "lib/kprintf.h"

namespace {

#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_procfs:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_procfs:%d: %s", __LINE__, #cond); } } while(0)

const Path none = {};
char text[8192];

uint64_t slen(const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

bool starts(const char* s, const char* prefix){
    return memcmp(s, prefix, slen(prefix)) == 0;
}

bool contains(const char* hay, const char* needle){
    uint64_t n = slen(needle);
    for(; *hay; hay++){
        if(memcmp(hay, needle, n) == 0){
            return true;
        }
    }
    return false;
}

uint64_t count_of(const char* hay, const char* needle){
    uint64_t n = slen(needle), c = 0;
    for(; *hay; hay++){
        if(memcmp(hay, needle, n) == 0){
            c++;
        }
    }
    return c;
}

// Numarul de dupa prefix (prima aparitie)
uint64_t number_after(const char* hay, const char* prefix){
    uint64_t n = slen(prefix);
    for(; *hay; hay++){
        if(memcmp(hay, prefix, n) == 0){
            const char* p = hay + n;
            while(*p == ' ' || *p == '\t'){
                p++;
            }
            uint64_t v = 0;
            while(*p >= '0' && *p <= '9'){
                v = v * 10 + (uint64_t)(*p++ - '0');
            }
            return v;
        }
    }
    panic("test_procfs: lipseste \"%s\"", prefix);
}

// Tot fisierul in text (cu terminator); numarul de bytes
int64_t slurp(const char* path){
    File* f;
    EQ(vfs_open(none, path, O_RDONLY, 0, f), 0);
    int64_t total = 0;
    for(;;){
        int64_t n = f->ops->read(f, text + total, sizeof(text) - 1 - (uint64_t)total, false);
        CHECK(n >= 0);
        if(n == 0){
            break;
        }
        total += n;
    }
    text[total] = 0;
    file_put(f);
    return total;
}

bool listed(const char* dir, const char* name, int expect_type){
    File* f;
    EQ(vfs_open(none, dir, O_RDONLY | O_DIRECTORY, 0, f), 0);
    static char buf[4096];
    bool found = false;
    int64_t got;
    while((got = f->ops->getdents(f, buf, sizeof(buf))) > 0){
        for(int64_t off = 0; off < got;){
            const Dirent64* d = (const Dirent64*)(buf + off);
            off += d->d_reclen;
            if(memcmp(d->d_name, name, slen(name) + 1) == 0){
                EQ(d->d_type, expect_type);
                found = true;
            }
        }
    }
    EQ(got, 0);
    file_put(f);
    return found;
}

void check_files(){
    Stat st;
    EQ(vfs_stat(none, "/proc", true, st), 0);
    CHECK(S_ISDIR(st.st_mode));
    CHECK(listed("/proc", "meminfo", DT_REG));
    CHECK(listed("/proc", "uptime", DT_REG));
    CHECK(listed("/proc", "cpuinfo", DT_REG));
    CHECK(listed("/proc", "mounts", DT_REG));
    CHECK(listed("/proc", "self", DT_LNK));
    EQ(vfs_stat(none, "/proc/meminfo", true, st), 0);
    CHECK(S_ISREG(st.st_mode));
    EQ(vfs_stat(none, "/proc/nu-exista", true, st), -ENOENT);
    EQ(vfs_stat(none, "/proc/4000000/status", true, st), -ENOENT);
    EQ(vfs_stat(none, "/proc/meminfo/x", true, st), -ENOTDIR);
    // Doar citire
    File* f;
    EQ(vfs_open(none, "/proc/meminfo", O_WRONLY, 0, f), -EROFS);
    int err = vfs_mkdir(none, "/proc/d", 0755);
    CHECK(err == -EROFS || err == -EPERM);
    // Citire cu pozitie: a doua jumatate a continutului
    int64_t whole = slurp("/proc/meminfo");
    static char half[64];
    EQ(vfs_open(none, "/proc/meminfo", O_RDONLY, 0, f), 0);
    EQ(f->ops->lseek(f, 10, SEEK_SET), 10);
    int64_t n = f->ops->read(f, half, sizeof(half) - 1, false);
    CHECK(n > 0 && n <= whole - 10);
    CHECK(memcmp(half, text + 10, 8) == 0);
    file_put(f);
    kprintf("[OK] test_procfs files\n");
}

void check_meminfo(){
    int64_t n = slurp("/proc/meminfo");
    CHECK(n > 0);
    CHECK(starts(text, "MemTotal:"));
    // Formatul Linux: "MemTotal:" completat pana la 15, spatiu, valoare pe 8 coloane, " kB"
    CHECK(memcmp(text, "MemTotal:       ", 16) == 0);
    CHECK(contains(text, " kB\nMemFree:"));
    uint64_t total = number_after(text, "MemTotal:");
    uint64_t free_kb = number_after(text, "MemFree:");
    uint64_t avail = number_after(text, "MemAvailable:");
    CHECK(total >= 100 * 1024 && total <= free_kb + total);
    CHECK(free_kb > 0 && free_kb <= total);
    CHECK(avail <= total);
    uint64_t now = pmm_free_count() * 4;
    CHECK(free_kb + 4096 >= now && now + 4096 >= free_kb);
    CHECK(contains(text, "SwapTotal:"));

    slurp("/proc/uptime");
    uint64_t sec = number_after(text, "");
    CHECK(sec <= uptime_ms() / 1000 + 1);
    CHECK(count_of(text, ".") == 2 && text[slen(text) - 1] == '\n');

    slurp("/proc/cpuinfo");
    EQ(count_of(text, "processor\t: "), smp_cpu_count());
    CHECK(contains(text, "vendor_id\t: "));
    CHECK(contains(text, "model name\t: "));
    CHECK(contains(text, "cpu MHz\t\t: "));
    CHECK(contains(text, "flags\t\t: "));
    CHECK(contains(text, " fpu"));
    CHECK(contains(text, " lm"));
    kprintf("[OK] test_procfs meminfo\n");
}

void check_mounts(){
    slurp("/proc/mounts");
    CHECK(contains(text, "initrd / tarfs ro 0 0\n"));
    CHECK(contains(text, "devfs /dev devfs rw 0 0\n"));
    CHECK(contains(text, "tmpfs /tmp tmpfs rw 0 0\n"));
    CHECK(contains(text, "tmpfs /run tmpfs rw 0 0\n"));
    CHECK(contains(text, "proc /proc proc ro 0 0\n"));
    CHECK(starts(text, "initrd / "));
    // /etc/mtab -> ../proc/mounts
    int64_t n = slurp("/etc/mtab");
    CHECK(n > 0 && starts(text, "initrd / "));
    kprintf("[OK] test_procfs mounts\n");
}

void to_dec(uint64_t v, char* out){
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while(v);
    for(int i = 0; i < n; i++){
        out[i] = tmp[n - 1 - i];
    }
    out[n] = 0;
}

// Task cu spatiu user construit de mana (nu e pornit niciodata): o pagina de cod, doua de date, una de stiva
void check_pid(){
    Task* t = task_create_kernel("ptest", [](void*){}, nullptr);
    CHECK(t);
    AddressSpace* as = (AddressSpace*)kmalloc(sizeof(AddressSpace));
    CHECK(as);
    *as = as_create();
    CHECK(as->pml4_phys);
    const struct { uint64_t va; uint64_t flags; } pages[] = {
        {0x400000, PTE_USER},
        {0x402000, PTE_USER | PTE_RW | PTE_NX},
        {0x403000, PTE_USER | PTE_RW | PTE_NX},
        {USER_STACK_TOP - PAGE_SIZE, PTE_USER | PTE_RW | PTE_NX},
    };
    for(const auto& p : pages){
        uint64_t phys = pmm_alloc_page();
        CHECK(phys);
        CHECK(vmm_map(*as, p.va, phys, p.flags));
    }
    t->as = as;
    Process* proc = process_create(t, nullptr);
    CHECK(proc);
    const char* argv[] = {"/bin/ptest", "-x", nullptr};
    process_set_cmdline(proc, argv);

    char dir[32] = "/proc/";
    to_dec(t->tid, dir + 6);
    char path[64];
    auto file = [&](const char* name) -> const char* {
        memcpy(path, dir, slen(dir) + 1);
        memcpy(path + slen(dir), "/", 2);
        memcpy(path + slen(dir) + 1, name, slen(name) + 1);
        return path;
    };
    CHECK(listed("/proc", dir + 6, DT_DIR));
    CHECK(listed(dir, "status", DT_REG) && listed(dir, "cmdline", DT_REG) && listed(dir, "maps", DT_REG));

    slurp(file("status"));
    CHECK(starts(text, "Name:\tptest\nState:\t"));
    CHECK(number_after(text, "Pid:") == t->tid);
    CHECK(number_after(text, "Tgid:") == t->tid);
    EQ(number_after(text, "PPid:"), 0);
    EQ(number_after(text, "VmRSS:"), 16);
    EQ(number_after(text, "VmSize:"), 16);
    CHECK(contains(text, "VmRSS:\t      16 kB\n"));

    // cmdline: argv separat prin '\0'
    int64_t n = slurp(file("cmdline"));
    EQ(n, 14);
    CHECK(memcmp(text, "/bin/ptest\0-x\0", 14) == 0);

    n = slurp(file("maps"));
    CHECK(n > 0);
    CHECK(starts(text, "00400000-00401000 r-xp 00000000 00:00 0\n"
                       "00402000-00404000 rw-p 00000000 00:00 0\n"
                       "7fffffffd000-7fffffffe000 rw-p 00000000 00:00 0"));
    CHECK(contains(text, "[stack]\n"));
    EQ(count_of(text, "\n"), 3);

    process_abort(proc);
    task_destroy(t);                    // elibereaza si spatiul de adrese
    Stat st;
    EQ(vfs_stat(none, dir, true, st), -ENOENT);
    CHECK(!listed("/proc", dir + 6, DT_DIR));
    kprintf("[OK] test_procfs pid\n");
}

volatile bool self_done = false;

void self_thread(void*){
    Task* me = task_current();
    char want[24];
    to_dec(me->tid, want);
    char buf[32];
    int64_t n = vfs_readlink(none, "/proc/self", buf, sizeof(buf));
    EQ(n, slen(want));
    buf[n] = 0;
    CHECK(memcmp(buf, want, (uint64_t)n + 1) == 0);
    slurp("/proc/self/status");
    CHECK(starts(text, "Name:\tprocfs-self\n"));
    CHECK(number_after(text, "Pid:") == me->tid);
    EQ(slurp("/proc/self/cmdline"), 0);
    EQ(slurp("/proc/self/maps"), 0);
    self_done = true;
}

void check_self(){
    uint64_t tasks = task_count();
    Task* t = task_create_kernel("procfs-self", self_thread, nullptr);
    CHECK(t);
    sched_add(t);
    __asm__ volatile("sti");
    uint64_t start = uptime_ms();
    while(!self_done && uptime_ms() - start < 10000){
        sleep_ms(10);
    }
    CHECK(self_done);
    // Task-ul terminat e eliberat de idle: testele urmatoare numara task-urile
    while(task_count() > tasks && uptime_ms() - start < 10000){
        yield();
        __asm__ volatile("hlt");
    }
    EQ(task_count(), tasks);
    kprintf("[OK] test_procfs self\n");
}

}

void test_procfs_run(){
    uint64_t inodes = inode_count();
    uint64_t dentries = dentry_count();
    check_files();
    check_meminfo();
    check_mounts();
    check_pid();
    check_self();
    // Fara cache: nimic nu ramane dupa ce fisierele se inchid
    EQ(inode_count(), inodes);
    EQ(dentry_count(), dentries);
    kprintf("[OK] test_procfs (meminfo, uptime, cpuinfo, mounts, self, pid)\n");
}
