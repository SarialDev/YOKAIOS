// Teste pe gazda pentru ext4 + jbd2 (T6.8): acelasi cod din kernel/fs/{ext4,jbd2} + bcache, compilat userland cu stub-uri
// (tests/host/stubs). Dispozitivul de bloc e o imagine intr-un fisier cu cache de scriere simulat: scrierile raman
// "in asteptare" pana la flush, iar la o cadere simulata se aplica doar un subset aleator (ca un disc cu cache).
//
//   ext4_host model <img> <seed> <ops>        operatii aleatoare comparate cu un model, apoi umount, e2fsck, remount
//   ext4_host sweep <img> <pas> <seed>        aceeasi sarcina de lucru, cazuta la fiecare al <pas>-lea write: dupa
//                                             replay arborele trebuie sa fie o stare comisa si e2fsck curat
//   ext4_host replay <img>                    doar monteaza (replay) si inchide curat
// Imaginea = un sistem de fisiere ext4 gol de la offset 0 (fara tabela de partitii)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

#include "fs/ext4/ext4.h"
#include "fs/vfs/inode.h"
#include "fs/jbd2/jbd2.h"
#include "block/bcache.h"
#include "hal/block.h"
#include "lib/crc.h"

// ---- stub-uri ale kernelului ----
static uint64_t fake_time = 1700000000;
Timespec vfs_now(){ return Timespec{(int64_t)fake_time++, 0}; }
uint64_t wall_time(){ return fake_time; }
uint64_t uptime_ms(){ return 0; }
void sleep_ms(uint32_t){}
struct Task;
Task* task_create_kernel(const char*, void (*)(void*), void*, uint8_t){ return nullptr; }
void sched_add(Task*){}

static FileSystemType* fs_types;
int fs_register(FileSystemType* t){ t->next = fs_types; fs_types = t; return 0; }
FileSystemType* fs_find(const char* name){
    for(FileSystemType* t = fs_types; t; t = t->next){
        if(!strcmp(t->name, name)){
            return t;
        }
    }
    return nullptr;
}
Inode* inode_alloc(SuperBlock* sb, uint64_t ino, uint32_t mode, const InodeOps* ops){
    Inode* i = (Inode*)calloc(1, sizeof(Inode));
    i->sb = sb;
    i->ino = ino;
    i->mode = mode;
    i->ops = ops;
    i->nlink = 1;
    i->atime = i->mtime = i->ctime = vfs_now();
    return i;
}
void inode_free(Inode* i){ free(i); }

// ---- dispozitiv de bloc cu cache de scriere ----
struct Pend {
    uint64_t lba;
    std::vector<uint8_t> data;
};

class FileDev : public BlockDevice {
public:
    FILE* f = nullptr;
    uint64_t nsect = 0;
    std::vector<Pend> pend;
    uint64_t nwrites = 0;
    uint64_t nflush = 0;
    uint64_t crash_at = ~0ull;
    unsigned seed = 1;

    uint32_t block_size() const override { return 512; }
    uint64_t block_count() const override { return nsect; }
    int read(uint64_t lba, uint32_t count, void* buf) override {
        fseeko(f, (off_t)lba * 512, SEEK_SET);
        if(fread(buf, 512, count, f) != count){
            memset(buf, 0, (size_t)count * 512);
        }
        for(auto& p : pend){
            for(uint32_t i = 0; i < count; i++){
                uint64_t s = lba + i;
                if(s >= p.lba && s < p.lba + p.data.size() / 512){
                    memcpy((uint8_t*)buf + i * 512, p.data.data() + (s - p.lba) * 512, 512);
                }
            }
        }
        return 0;
    }
    int write(uint64_t lba, uint32_t count, const void* buf) override {
        nwrites++;
        Pend p;
        p.lba = lba;
        p.data.assign((const uint8_t*)buf, (const uint8_t*)buf + (size_t)count * 512);
        pend.push_back(std::move(p));
        if(nwrites == crash_at){
            crash();
        }
        return 0;
    }
    int flush() override {
        apply_all();
        nflush++;
        return 0;
    }
    void apply(const Pend& p){
        fseeko(f, (off_t)p.lba * 512, SEEK_SET);
        fwrite(p.data.data(), 1, p.data.size(), f);
    }
    void apply_all(){
        for(auto& p : pend){
            apply(p);
        }
        pend.clear();
        fflush(f);
    }
    // Pierdere de curent: un subset aleator al scrierilor nesincronizate ajunge pe disc, in ordine aleatoare
    [[noreturn]] void crash(){
        srand(seed);
        std::vector<Pend*> keep;
        for(auto& p : pend){
            if(rand() & 1){
                keep.push_back(&p);
            }
        }
        for(size_t i = keep.size(); i > 1; i--){
            std::swap(keep[i - 1], keep[(size_t)rand() % i]);
        }
        for(Pend* p : keep){
            apply(*p);
        }
        fflush(f);
        _exit(0);
    }
};

static FileDev dev;
static HalDevice* hal_dev = &dev;
bool hal_register(HalKind, HalDevice*, const char*){ return true; }
int hal_count(HalKind k){ return k == HalKind::Block ? 1 : 0; }
HalDevice* hal_get(HalKind k, int i){ return k == HalKind::Block && i == 0 ? hal_dev : nullptr; }
const char* hal_name(HalKind k, int i){ return k == HalKind::Block && i == 0 ? "sda2" : nullptr; }

// ---- mini-VFS ----
struct Fs {
    SuperBlock sb;
    Inode* root;
};
static Fs fs;

static const char* g_img;

static bool fs_mount(const char* img, bool crash_mode){
    dev.f = fopen(img, "r+b");
    if(!dev.f){
        perror(img);
        return false;
    }
    fseeko(dev.f, 0, SEEK_END);
    dev.nsect = (uint64_t)ftello(dev.f) / 512;
    dev.pend.clear();
    (void)crash_mode;
    static bool inited;
    if(!inited){
        ext4_init();
        inited = true;
    }
    memset(&fs, 0, sizeof(fs));
    FileSystemType* t = fs_find("ext4");
    fs.sb.type = t;
    int rc = t->mount(t, &fs.sb, "sda2", nullptr);
    if(rc){
        printf("mount: %d\n", rc);
        return false;
    }
    fs.root = fs.sb.root_inode;
    return true;
}

static void fs_umount(){
    fs.sb.ops->kill(&fs.sb);
    dev.apply_all();
    fclose(dev.f);
    dev.f = nullptr;
}

static Inode* lookup(Inode* d, const std::string& name){
    Inode* out = nullptr;
    if(d->ops->lookup(d, name.c_str(), (uint32_t)name.size(), out)){
        return nullptr;
    }
    return out;
}

static std::vector<std::string> split(const std::string& p){
    std::vector<std::string> v;
    size_t i = 0;
    while(i < p.size()){
        size_t j = p.find('/', i);
        if(j == std::string::npos){
            j = p.size();
        }
        if(j > i){
            v.push_back(p.substr(i, j - i));
        }
        i = j + 1;
    }
    return v;
}

static Inode* walk(const std::string& p){
    Inode* cur = fs.root;
    for(auto& c : split(p)){
        cur = lookup(cur, c);
        if(!cur){
            return nullptr;
        }
    }
    return cur;
}

static Inode* parent_of(const std::string& p, std::string& leaf){
    auto parts = split(p);
    leaf = parts.back();
    parts.pop_back();
    Inode* cur = fs.root;
    for(auto& c : parts){
        cur = lookup(cur, c);
        if(!cur){
            return nullptr;
        }
    }
    return cur;
}

static void release(Inode* i){
    if(i && i->nlink == 0){
        fs.sb.ops->release_inode(i);
    }
}

#define CHECKR(cond) do { if(!(cond)){ printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fflush(stdout); abort(); } } while(0)

static int op_mkdir(const std::string& p){
    std::string leaf;
    Inode* d = parent_of(p, leaf);
    CHECKR(d);
    Inode* out;
    return d->ops->mkdir(d, leaf.c_str(), (uint32_t)leaf.size(), S_IFDIR | 0755, out);
}

static Inode* op_create(const std::string& p, int* rc = nullptr){
    std::string leaf;
    Inode* d = parent_of(p, leaf);
    CHECKR(d);
    Inode* out = nullptr;
    int r = d->ops->create(d, leaf.c_str(), (uint32_t)leaf.size(), S_IFREG | 0644, out);
    if(rc){
        *rc = r;
    }
    return r ? nullptr : out;
}

static int64_t op_write(Inode* i, uint64_t pos, const std::string& data){
    return i->ops->write(i, pos, data.data(), data.size());
}

static int op_unlink(const std::string& p){
    std::string leaf;
    Inode* d = parent_of(p, leaf);
    CHECKR(d);
    Inode* v = lookup(d, leaf);
    CHECKR(v);
    int r = S_ISDIR(v->mode) ? d->ops->rmdir(d, leaf.c_str(), (uint32_t)leaf.size(), v)
                             : d->ops->unlink(d, leaf.c_str(), (uint32_t)leaf.size(), v);
    if(!r){
        release(v);
    }
    return r;
}

static int op_rename(const std::string& a, const std::string& b){
    std::string la, lb;
    Inode* da = parent_of(a, la);
    Inode* db = parent_of(b, lb);
    CHECKR(da && db);
    Inode* src = lookup(da, la);
    CHECKR(src);
    Inode* victim = lookup(db, lb);
    int r = da->ops->rename(da, la.c_str(), (uint32_t)la.size(), db, lb.c_str(), (uint32_t)lb.size(), src, victim);
    if(!r && victim){
        release(victim);
    }
    return r;
}

static int op_symlink(const std::string& p, const std::string& target){
    std::string leaf;
    Inode* d = parent_of(p, leaf);
    CHECKR(d);
    Inode* out;
    return d->ops->symlink(d, leaf.c_str(), (uint32_t)leaf.size(), target.c_str(), out);
}

static void commit(){
    CHECKR(ext4_commit(fs.root) == 0);
}

// ---- parcurgerea arborelui ----
static std::string read_all(Inode* i){
    std::string out;
    std::vector<uint8_t> buf(65536);
    uint64_t pos = 0;
    for(;;){
        int64_t n = i->ops->read(i, pos, buf.data(), buf.size());
        CHECKR(n >= 0);
        if(n == 0){
            break;
        }
        out.append((char*)buf.data(), (size_t)n);
        pos += (uint64_t)n;
    }
    return out;
}

static std::vector<std::string> list_dir(Inode* d){
    std::vector<std::string> names;
    DirEntry e;
    for(uint64_t k = 0;; k++){
        int r = d->ops->readdir(d, k, e);
        CHECKR(r >= 0);
        if(r == 0){
            break;
        }
        names.push_back(std::string(e.name, e.name_len));
    }
    return names;
}

struct Sums {
    std::vector<std::string> lines;
};

static void walk_tree(Inode* d, const std::string& path, bool content, Sums& s, int depth = 0){
    CHECKR(depth < 64);
    for(auto& name : list_dir(d)){
        Inode* c = lookup(d, name);
        CHECKR(c);
        std::string p = path + "/" + name;
        char line[512];
        if(S_ISDIR(c->mode)){
            snprintf(line, sizeof(line), "D %s", p.c_str());
            s.lines.push_back(line);
            walk_tree(c, p, content, s, depth + 1);
        } else if(S_ISLNK(c->mode)){
            std::vector<char> t(c->size + 1);
            int64_t n = c->ops->readlink(c, t.data(), c->size);
            CHECKR(n == (int64_t)c->size);
            snprintf(line, sizeof(line), "L %s -> %.*s", p.c_str(), (int)n, t.data());
            s.lines.push_back(line);
        } else if(content){
            std::string data = read_all(c);
            CHECKR(data.size() == c->size);
            snprintf(line, sizeof(line), "F %s %lu %08x", p.c_str(), (unsigned long)c->size,
                     crc32c(0xFFFFFFFFu, data.data(), data.size()));
            s.lines.push_back(line);
        } else {
            snprintf(line, sizeof(line), "F %s %lu", p.c_str(), (unsigned long)c->size);
            s.lines.push_back(line);
        }
    }
}

static uint64_t digest_of(Sums& s){
    std::sort(s.lines.begin(), s.lines.end());
    if(const char* dump = getenv("YK_DUMP")){                      // depanare: arborele fiecarui digest
        FILE* f = fopen(dump, "a");
        for(auto& l : s.lines) fprintf(f, "%s\n", l.c_str());
        fprintf(f, "----\n");
        fclose(f);
    }
    uint64_t h = 1469598103934665603ull;
    for(auto& l : s.lines){
        for(char c : l){
            h = (h ^ (uint8_t)c) * 1099511628211ull;
        }
        h = (h ^ 10) * 1099511628211ull;
    }
    return h;
}

// Starea observabila a sarcinii de lucru: /w cu continut, /many /huge /data doar nume si dimensiuni
static uint64_t tree_digest(){
    Sums s;
    for(const char* top : {"/w", "/many", "/huge", "/data", "/m"}){
        Inode* d = walk(top);
        if(!d){
            continue;
        }
        s.lines.push_back(std::string("T ") + top);
        // fisierele mari din /data: doar dimensiunea
        walk_tree(d, top, strcmp(top, "/data") != 0, s);
    }
    // /w/f07 e suprascris pe loc (blocuri deja alocate): in modul ordered datele noi ajung pe disc inainte de commit,
    // deci dupa o cadere continutul poate fi oricare; conteaza doar dimensiunea (metadata)
    for(auto& l : s.lines){
        if(l.compare(0, 9, "F /w/f07 ") == 0){
            l = l.substr(0, l.rfind(' '));
        }
    }
    return digest_of(s);
}

// ---- sarcina de lucru deterministica (pentru sweep) ----
static uint32_t rng_state;
static uint32_t rnd(){
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static std::string blob(size_t n, uint32_t salt){
    std::string s(n, 0);
    uint32_t x = salt * 2654435761u + 12345;
    for(size_t i = 0; i < n; i++){
        x = x * 1103515245u + 12345;
        s[i] = (char)(x >> 16);
    }
    return s;
}

static void must(int rc, const char* what){
    if(rc){
        printf("FAIL %s: %d\n", what, rc);
        fflush(stdout);
        abort();
    }
}

static void put_file(const std::string& p, size_t n, uint32_t salt){
    Inode* i = op_create(p);
    CHECKR(i);
    if(n){
        CHECKR(op_write(i, 0, blob(n, salt)) == (int64_t)n);
    }
}

static void workload(){
    rng_state = 12345;
    must(op_mkdir("/w"), "mkdir /w");
    must(op_mkdir("/w/a"), "mkdir /w/a");
    must(op_mkdir("/w/a/b"), "mkdir /w/a/b");
    commit();
    static const size_t sizes[] = {0, 1, 100, 4095, 4096, 4097, 20000, 100000, 300000};
    for(int i = 0; i < 18; i++){
        char p[64];
        snprintf(p, sizeof(p), "/w/f%02d", i);
        put_file(p, sizes[i % 9], (uint32_t)i);
    }
    commit();
    must(op_symlink("/w/s_short", "f01"), "symlink");
    must(op_symlink("/w/a/s_long", std::string(100, 'x') + "/target"), "symlink long");
    {
        Inode* f = walk("/w/f07");
        CHECKR(f && op_write(f, f->size, blob(9000, 77)) == 9000);          // append
        CHECKR(op_write(f, 1000, blob(5000, 78)) == 5000);                  // suprascriere
        CHECKR(op_write(f, 3000000, blob(10, 79)) == 10);                   // gaura
    }
    commit();
    // truncate (reducere in mijlocul unui bloc, la zero, extindere)
    must(walk("/w/f08")->ops->truncate(walk("/w/f08"), 150000), "trunc");
    must(walk("/w/f07")->ops->truncate(walk("/w/f07"), 0), "trunc0");
    must(walk("/w/f06")->ops->truncate(walk("/w/f06"), 50000), "trunc ext");
    commit();
    // directoare: /w/many linear care creste, apoi /many (htree) primeste intrari noi si pierde altele
    must(op_mkdir("/w/many"), "mkdir w/many");
    for(int i = 0; i < 260; i++){
        char p[64];
        snprintf(p, sizeof(p), "/w/many/entry_with_a_longer_name_%04d", i);
        put_file(p, (size_t)(i % 7) * 10, (uint32_t)i);
        if(i % 80 == 79){
            commit();
        }
    }
    commit();
    for(int i = 0; i < 400; i++){
        char p[64];
        snprintf(p, sizeof(p), "/many/new_%05d", i);
        put_file(p, (i % 9) ? 0 : 20, (uint32_t)i);
        if(i % 100 == 99){
            commit();
        }
    }
    for(int i = 0; i < 120; i++){
        char p[64];
        snprintf(p, sizeof(p), "/many/f%05d", i * 7);
        must(op_unlink(p), "unlink many");
    }
    commit();
    for(int i = 0; i < 60; i++){
        char p[96];
        snprintf(p, sizeof(p), "/huge/added_%05d_pad_pad", i);
        put_file(p, 0, 1);
    }
    commit();
    // redenumiri: fisier peste fisier, director intre parinti, director peste director gol
    must(op_rename("/w/f03", "/w/a/b/f03moved"), "rename 1");
    must(op_rename("/w/f04", "/w/f05"), "rename over file");
    must(op_mkdir("/w/d1"), "mkdir d1");
    must(op_mkdir("/w/d1/in"), "mkdir d1/in");
    must(op_mkdir("/w/d2"), "mkdir d2");
    must(op_rename("/w/d1/in", "/w/d2/in2"), "rename dir across");
    must(op_mkdir("/w/e1"), "mkdir e1");
    must(op_rename("/w/d2", "/w/e1"), "rename dir over empty dir");
    commit();
    // stergeri
    must(op_unlink("/w/f10"), "unlink big");
    must(op_unlink("/w/s_short"), "unlink symlink");
    must(op_unlink("/w/a/b/f03moved"), "unlink moved");
    must(op_unlink("/w/a/b"), "rmdir");
    commit();
    // fisiere vechi din imagine: arbore de extente adanc eliberat, fisier sparse trunchiat
    must(op_unlink("/data/islands.bin"), "unlink islands");
    commit();
    Inode* sp = walk("/data/sparse.bin");
    if(sp){
        must(sp->ops->truncate(sp, 3 << 20), "trunc sparse");
        commit();
    }
    Inode* big = walk("/data/big.bin");
    if(big){
        CHECKR(op_write(big, 1 << 20, blob(70000, 5)) == 70000);
    }
    must(op_unlink("/data/hello.txt"), "unlink hello");
    commit();
}

// ---- model ----
struct Model {
    std::map<std::string, std::string> files;
    std::set<std::string> dirs;
    std::map<std::string, std::string> links;
};

static std::string pick_name(){
    char b[40];
    snprintf(b, sizeof(b), "n%u", rnd() % 100000);
    return b;
}

static bool exists(Model& m, const std::string& p){
    return m.files.count(p) || m.dirs.count(p) || m.links.count(p);
}

static bool dir_empty(Model& m, const std::string& d){
    std::string pre = d + "/";
    for(auto& f : m.files) if(f.first.compare(0, pre.size(), pre) == 0) return false;
    for(auto& f : m.dirs) if(f.compare(0, pre.size(), pre) == 0) return false;
    for(auto& f : m.links) if(f.first.compare(0, pre.size(), pre) == 0) return false;
    return true;
}

static std::string rand_dir(Model& m){
    std::vector<std::string> v(m.dirs.begin(), m.dirs.end());
    return v[rnd() % v.size()];
}

template<class T> static const std::string& rand_key(std::map<std::string, T>& mp){
    auto it = mp.begin();
    std::advance(it, rnd() % mp.size());
    return it->first;
}

static void model_verify(Model& m, bool full){
    Sums s;
    Inode* d = walk("/m");
    CHECKR(d);
    walk_tree(d, "/m", true, s);
    std::set<std::string> got(s.lines.begin(), s.lines.end());
    size_t want = m.files.size() + m.dirs.size() - 1 + m.links.size();
    if(s.lines.size() != want){
        printf("model: %zu intrari, asteptat %zu\n", s.lines.size(), want);
        for(auto& l : s.lines) printf("  fs: %s\n", l.c_str());
        abort();
    }
    for(auto& f : m.files){
        char line[512];
        snprintf(line, sizeof(line), "F %s %lu %08x", f.first.c_str(), (unsigned long)f.second.size(),
                 crc32c(0xFFFFFFFFu, f.second.data(), f.second.size()));
        if(!got.count(line)){
            printf("model: lipseste/diferit: %s\n", line);
            abort();
        }
    }
    for(auto& f : m.dirs){
        if(f != "/m" && !got.count("D " + f)){
            printf("model: lipseste dir %s\n", f.c_str());
            abort();
        }
    }
    for(auto& f : m.links){
        if(!got.count("L " + f.first + " -> " + f.second)){
            printf("model: lipseste symlink %s\n", f.first.c_str());
            abort();
        }
    }
    (void)full;
}

static void model_run(unsigned seed, int ops){
    rng_state = seed;
    Model m;
    must(op_mkdir("/m"), "mkdir /m");
    m.dirs.insert("/m");
    for(int n = 0; n < ops; n++){
        uint32_t r = rnd() % 100;
        std::string dir = rand_dir(m);
        Ext4Info inf;
        ext4_info(fs.root, inf);
        if(inf.free_blocks < 1000 && !m.files.empty()){
            const std::string p = rand_key(m.files);       // aproape plin: se sterg fisiere
            must(op_unlink(p), "unlink (spatiu)");
            m.files.erase(p);
            continue;
        }
        if(r < 12 && m.dirs.size() < 40){
            std::string p = dir + "/" + pick_name();
            if(exists(m, p)) continue;
            must(op_mkdir(p), "mkdir");
            m.dirs.insert(p);
        } else if(r < 40){
            std::string p = dir + "/" + pick_name();
            if(exists(m, p)) continue;
            static const size_t szs[] = {0, 1, 17, 4095, 4096, 4097, 9000, 33000, 150000, 700000};
            size_t sz = szs[rnd() % 10];
            std::string data = blob(sz, rnd());
            Inode* i = op_create(p);
            CHECKR(i);
            if(sz){ int64_t w = op_write(i, 0, data); if(w != (int64_t)sz){ Ext4Info inf; ext4_info(fs.root, inf); printf("write %zu -> %ld, libere %lu blocuri, op %d\n", sz, (long)w, (unsigned long)inf.free_blocks, n); abort(); } }
            m.files[p] = data;
        } else if(r < 52 && !m.files.empty()){
            const std::string p = rand_key(m.files);
            std::string add = blob(rnd() % 20000 + 1, rnd());
            Inode* i = walk(p);
            CHECKR(i);
            CHECKR(op_write(i, i->size, add) == (int64_t)add.size());
            m.files[p] += add;
        } else if(r < 62 && !m.files.empty()){
            const std::string p = rand_key(m.files);
            std::string& cur = m.files[p];
            uint64_t pos = cur.empty() ? 0 : rnd() % (cur.size() + 5000);
            std::string add = blob(rnd() % 12000 + 1, rnd());
            Inode* i = walk(p);
            CHECKR(op_write(i, pos, add) == (int64_t)add.size());
            if(cur.size() < pos + add.size()) cur.resize(pos + add.size(), 0);
            memcpy(&cur[pos], add.data(), add.size());
        } else if(r < 70 && !m.files.empty()){
            const std::string p = rand_key(m.files);
            std::string& cur = m.files[p];
            uint64_t ns = rnd() % 3 == 0 ? 0 : rnd() % (cur.size() + 20000);
            Inode* i = walk(p);
            must(i->ops->truncate(i, ns), "truncate");
            cur.resize(ns, 0);
        } else if(r < 80 && !m.files.empty()){
            const std::string p = rand_key(m.files);
            must(op_unlink(p), "unlink");
            m.files.erase(p);
        } else if(r < 84 && m.dirs.size() > 1){
            std::string d = rand_dir(m);
            if(d == "/m" || !dir_empty(m, d)) continue;
            must(op_unlink(d), "rmdir");
            m.dirs.erase(d);
        } else if(r < 92 && !m.files.empty()){
            const std::string p = rand_key(m.files);
            std::string np = rand_dir(m) + "/" + pick_name();
            if(exists(m, np)) continue;
            must(op_rename(p, np), "rename");
            m.files[np] = m.files[p];
            m.files.erase(p);
        } else if(r < 94 && m.files.size() > 1){
            const std::string p = rand_key(m.files);
            const std::string q = rand_key(m.files);
            if(p == q) continue;
            must(op_rename(p, q), "rename over");
            m.files[q] = m.files[p];
            m.files.erase(p);
        } else if(r < 96 && m.dirs.size() > 2){
            std::string d = rand_dir(m);
            std::string nd = rand_dir(m) + "/" + pick_name();
            if(d == "/m" || exists(m, nd) || nd.compare(0, d.size() + 1, d + "/") == 0) continue;
            must(op_rename(d, nd), "rename dir");
            std::vector<std::string> mv;
            for(auto& x : m.dirs) if(x == d || x.compare(0, d.size() + 1, d + "/") == 0) mv.push_back(x);
            for(auto& x : mv){ m.dirs.erase(x); m.dirs.insert(nd + x.substr(d.size())); }
            std::vector<std::pair<std::string, std::string>> mf;
            for(auto& x : m.files) if(x.first.compare(0, d.size() + 1, d + "/") == 0) mf.push_back(x);
            for(auto& x : mf){ m.files.erase(x.first); m.files[nd + x.first.substr(d.size())] = x.second; }
            std::vector<std::pair<std::string, std::string>> ml;
            for(auto& x : m.links) if(x.first.compare(0, d.size() + 1, d + "/") == 0) ml.push_back(x);
            for(auto& x : ml){ m.links.erase(x.first); m.links[nd + x.first.substr(d.size())] = x.second; }
        } else if(r < 98){
            std::string p = dir + "/" + pick_name();
            if(exists(m, p)) continue;
            std::string t = rnd() % 2 ? std::string("short") : std::string(70 + rnd() % 200, 'q');
            must(op_symlink(p, t), "symlink");
            m.links[p] = t;
        } else if(!m.links.empty()){
            const std::string p = rand_key(m.links);
            must(op_unlink(p), "unlink symlink");
            m.links.erase(p);
        } else {
            commit();
        }
        if(n % 50 == 49){
            model_verify(m, false);
        }
        if(rnd() % 40 == 0){
            commit();
        }
    }
    model_verify(m, true);
    commit();
    fs_umount();
    CHECKR(fs_mount(g_img, false));                  // remontare: tot ce s-a scris trebuie sa se citeasca la fel
    model_verify(m, true);
    fs_umount();
    printf("model: %d operatii, %zu fisiere, %zu directoare, %zu symlink-uri OK\n", ops, m.files.size(), m.dirs.size(),
           m.links.size());
}

static int fsck(const char* img){
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "e2fsck -fn %s > /tmp/yk_fsck.out 2>&1", img);
    int rc = system(cmd);
    if(rc){
        printf("e2fsck a raportat erori (%d):\n", rc);
        if(system("head -40 /tmp/yk_fsck.out")){}
    }
    return rc;
}

// ---- sweep ----
static const char* DIGESTS = "/tmp/yk_digests.txt";

static void hook_record(int stage){
    if(stage == 0){
        FILE* f = fopen(DIGESTS, "a");
        fprintf(f, "%016lx\n", (unsigned long)tree_digest());
        fclose(f);
    }
}

static std::set<std::string> load_digests(){
    std::set<std::string> s;
    FILE* f = fopen(DIGESTS, "r");
    char line[64];
    while(f && fgets(line, sizeof(line), f)){
        line[strcspn(line, "\n")] = 0;
        s.insert(line);
    }
    if(f) fclose(f);
    return s;
}

static int run_child(void (*fn)()){
    fflush(stdout);
    pid_t p = fork();
    if(p == 0){
        fn();
        fflush(stdout);
        _exit(0);
    }
    int st;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static uint64_t g_crash_at;
static unsigned g_seed;

static void child_workload(){
    if(!fs_mount(g_img, true)){
        _exit(2);
    }
    dev.crash_at = g_crash_at;
    dev.seed = g_seed;
    if(g_crash_at == ~0ull){
        FILE* f = fopen(DIGESTS, "a");
        fprintf(f, "%016lx\n", (unsigned long)tree_digest());        // starea initiala
        fclose(f);
        jbd2_crash_hook = hook_record;
    }
    dev.nwrites = 0;
    workload();
    FILE* f = fopen(DIGESTS, "a");
    fprintf(f, "%016lx\n", (unsigned long)tree_digest());
    fclose(f);
    fs_umount();
    FILE* c = fopen("/tmp/yk_writes.txt", "w");
    fprintf(c, "%lu\n", (unsigned long)dev.nwrites);
    fclose(c);
}

static void child_verify(){
    if(!fs_mount(g_img, false)){
        printf("mount dupa cadere a esuat\n");
        _exit(3);
    }
    Ext4Info info;
    ext4_info(fs.root, info);
    uint64_t d = tree_digest();
    char buf[32];
    snprintf(buf, sizeof(buf), "%016lx", (unsigned long)d);
    if(!load_digests().count(buf)){
        printf("starea de dupa replay (%s) nu e una comisa (replay: %lu tranzactii)\n", buf,
               (unsigned long)info.replayed_txns);
        _exit(4);
    }
    fs_umount();
}

static int sweep(const char* base, unsigned step, unsigned seed){
    char run[256];
    snprintf(run, sizeof(run), "%s.run", base);
    unlink(DIGESTS);
    char cp[600];
    snprintf(cp, sizeof(cp), "cp --sparse=always %s %s", base, run);
    CHECKR(system(cp) == 0);
    g_img = run;
    g_crash_at = ~0ull;
    g_seed = seed;
    CHECKR(run_child(child_workload) == 0);
    FILE* f = fopen("/tmp/yk_writes.txt", "r");
    unsigned long total = 0;
    CHECKR(f && fscanf(f, "%lu", &total) == 1);
    fclose(f);
    printf("sweep: %lu scrieri in sarcina de lucru, %zu stari comise\n", total, load_digests().size());
    CHECKR(fsck(run) == 0);
    int bad = 0, n = 0;
    unsigned long k0 = 1 + seed % step;
    if(const char* only = getenv("YK_ONLYK")){                     // depanare: o singura cadere
        k0 = strtoul(only, nullptr, 10);
        total = k0;
    }
    for(unsigned long k = k0; k <= total; k += step){
        CHECKR(system(cp) == 0);
        g_crash_at = k;
        g_seed = seed + (unsigned)k;
        run_child(child_workload);
        g_crash_at = ~0ull;
        int rc = run_child(child_verify);
        n++;
        if(rc){
            printf("sweep: cadere la scrierea %lu: verificare esuata (%d)\n", k, rc);
            bad++;
        } else if(fsck(run)){
            printf("sweep: cadere la scrierea %lu: e2fsck nu e curat\n", k);
            bad++;
        }
        if(bad > 3){
            break;
        }
    }
    printf("sweep: %d caderi simulate, %d esecuri\n", n, bad);
    return bad ? 1 : 0;
}

int main(int argc, char** argv){
    if(argc < 3){
        fprintf(stderr, "uz: %s model|sweep|replay <img> ...\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    bcache_init(512);
    const char* mode = argv[1];
    g_img = argv[2];
    if(!strcmp(mode, "model")){
        if(!fs_mount(argv[2], false)){
            return 1;
        }
        model_run((unsigned)atoi(argv[3]), atoi(argv[4]));
        return fsck(argv[2]) ? 1 : 0;
    }
    if(!strcmp(mode, "sweep")){
        return sweep(argv[2], (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]));
    }
    if(!strcmp(mode, "replay")){
        if(!fs_mount(argv[2], false)){
            return 1;
        }
        fs_umount();
        return fsck(argv[2]) ? 1 : 0;
    }
    return 2;
}

// ext4_mount_root nu se foloseste pe gazda
struct Path;
int vfs_mount(const Path&, const char*, const char*, const char*, uint32_t, const char*){ return -1; }
