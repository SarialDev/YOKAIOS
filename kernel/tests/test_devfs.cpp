#include "tests/test_devfs.h"
#include "uapi/syscall_nr.h"
#include "fs/vfs/vfs.h"
#include "fs/pseudofs.h"
#include "drivers/fb/fb.h"
#include "hal/block.h"
#include "core/panic.h"
#include "lib/string.h"
#include "lib/kprintf.h"

namespace {

#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_devfs:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_devfs:%d: %s", __LINE__, #cond); } } while(0)

const Path none = {};

// 16 sectoare de 512 bytes, in memorie
class FakeDisk final : public BlockDevice {
public:
    constexpr FakeDisk() = default;
    uint32_t block_size() const override { return 512; }
    uint64_t block_count() const override { return 16; }
    int read(uint64_t lba, uint32_t count, void* buf) override {
        if(lba + count > 16){
            return -1;
        }
        memcpy(buf, data + lba * 512, count * 512);
        return 0;
    }
    int write(uint64_t lba, uint32_t count, const void* buf) override {
        if(lba + count > 16){
            return -1;
        }
        memcpy(data + lba * 512, buf, count * 512);
        return 0;
    }
    uint8_t data[16 * 512] = {};
};

FakeDisk sda_disk;
FakeDisk nvme_disk;

uint8_t buf[4096];

Stat stat_ok(const char* path){
    Stat st;
    EQ(vfs_stat(none, path, true, st), 0);
    return st;
}

File* open_ok(const char* path, int flags){
    File* f;
    EQ(vfs_open(none, path, flags, 0644, f), 0);
    return f;
}

int open_err(const char* path, int flags){
    File* f;
    int err = vfs_open(none, path, flags, 0644, f);
    if(!err){
        file_put(f);
    }
    return err;
}

int64_t wr(File* f, const void* b, uint64_t n){
    return f->ops->write(f, b, n, false);
}

int64_t rd(File* f, void* b, uint64_t n){
    return f->ops->read(f, b, n, false);
}

void seek(File* f, uint64_t pos){
    EQ(f->ops->lseek(f, (int64_t)pos, SEEK_SET), (int64_t)pos);
}

bool has_name(const char* list, const char* name){
    uint64_t n = 0;
    while(name[n]){
        n++;
    }
    for(const char* p = list; *p;){
        if(memcmp(p, name, n) == 0 && p[n] == ','){
            return true;
        }
        while(*p && *p != ','){
            p++;
        }
        if(*p){
            p++;
        }
    }
    return false;
}

void list_dir(char* out, uint64_t cap){
    File* f = open_ok("/dev", O_RDONLY | O_DIRECTORY);
    uint64_t n = 0;
    int64_t got;
    while((got = f->ops->getdents(f, buf, sizeof(buf))) > 0){
        for(int64_t off = 0; off < got;){
            Dirent64* d = (Dirent64*)(buf + off);
            off += d->d_reclen;
            for(const char* c = d->d_name; *c && n + 2 < cap; c++){
                out[n++] = *c;
            }
            out[n++] = ',';
        }
    }
    EQ(got, 0);
    out[n] = 0;
    file_put(f);
}

void check_basic(){
    Stat st = stat_ok("/dev");
    CHECK(S_ISDIR(st.st_mode));
    CHECK(st.st_dev != stat_ok("/").st_dev);

    st = stat_ok("/dev/null");
    CHECK(S_ISCHR(st.st_mode));
    File* f = open_ok("/dev/null", O_RDWR);
    EQ(wr(f, "ignorat", 7), 7);
    EQ(rd(f, buf, sizeof(buf)), 0);
    file_put(f);

    memset(buf, 0xAA, sizeof(buf));
    f = open_ok("/dev/zero", O_RDONLY);
    EQ(rd(f, buf, sizeof(buf)), sizeof(buf));
    for(uint64_t i = 0; i < sizeof(buf); i++){
        CHECK(buf[i] == 0);
    }
    EQ(wr(f, "x", 1), -EBADF);
    file_put(f);

    // Nu exista udev: nimic nu se creeaza sau sterge
    EQ(open_err("/dev/nou", O_CREAT | O_WRONLY), -EPERM);
    EQ(vfs_mkdir(none, "/dev/d", 0755), -EPERM);
    EQ(vfs_unlink(none, "/dev/null"), -EPERM);
    EQ(vfs_symlink(none, "null", "/dev/vid"), -EPERM);
    EQ(open_err("/dev/nu-exista", O_RDONLY), -ENOENT);
    EQ(open_err("/dev/null/x", O_RDONLY), -ENOTDIR);
}

void check_random(){
    static uint8_t a[64], b[64];
    const char* const names[] = {"/dev/random", "/dev/urandom"};
    for(const char* name : names){
        File* f = open_ok(name, O_RDWR);
        EQ(rd(f, a, sizeof(a)), sizeof(a));
        EQ(rd(f, b, sizeof(b)), sizeof(b));
        CHECK(memcmp(a, b, sizeof(a)) != 0);
        bool nonzero = false;
        for(uint64_t i = 0; i < sizeof(a); i++){
            nonzero |= a[i] != 0;
        }
        CHECK(nonzero);
        EQ(rd(f, a, 3), 3);                                     // lungimi care nu sunt multiplu de 8
        EQ(wr(f, "seed", 4), 4);
        file_put(f);
    }
}

void check_console(){
    const char msg[] = "devfs: scris prin /dev/console\n";
    File* f = open_ok("/dev/console", O_WRONLY);
    EQ(wr(f, msg, sizeof(msg) - 1), sizeof(msg) - 1);
    file_put(f);
    const char tty[] = "devfs: scris prin /dev/ttyS0\n";
    f = open_ok("/dev/ttyS0", O_RDWR);
    EQ(wr(f, tty, sizeof(tty) - 1), sizeof(tty) - 1);
    EQ(rd(f, buf, 16), 0);
    file_put(f);
}

void check_fb(){
    if(!fb_ready()){
        CHECK(open_err("/dev/fb0", O_RDWR) == -ENOENT);
        return;
    }
    uint64_t w = fb_width(), h = fb_height();
    Stat st = stat_ok("/dev/fb0");
    CHECK(S_ISCHR(st.st_mode));
    EQ(st.st_size, w * h * 4);
    File* f = open_ok("/dev/fb0", O_RDWR);
    uint64_t pos = 4 * (w * 5 + 7);
    uint32_t old = 0, now = 0;
    seek(f, pos);
    EQ(rd(f, &old, 4), 4);
    EQ(old, fb_getpixel(7, 5));
    seek(f, pos);
    EQ(wr(f, "\x56\x34\x12\x00", 4), 4);
    CHECK(fb_getpixel(7, 5) == 0x00123456);
    seek(f, pos);
    EQ(rd(f, &now, 4), 4);
    CHECK(now == 0x00123456);
    seek(f, pos);
    EQ(wr(f, &old, 4), 4);                                      // pune pixelul la loc
    CHECK(fb_getpixel(7, 5) == old);
    seek(f, 2);
    EQ(rd(f, &now, 4), -EINVAL);
    seek(f, w * h * 4);
    EQ(rd(f, &now, 4), 0);
    EQ(wr(f, &now, 4), -ENOSPC);
    file_put(f);
}

void check_block(){
    char list[160];
    // Inainte de inregistrare nu exista; dupa hal_register apar singure, cu numele dat de driver
    EQ(open_err("/dev/sda", O_RDONLY), -ENOENT);
    CHECK(hal_register(HalKind::Block, &sda_disk, "sda"));
    CHECK(hal_register(HalKind::Block, &nvme_disk, "nvme0n1"));
    list_dir(list, sizeof(list));
    CHECK(has_name(list, "null") && has_name(list, "zero") && has_name(list, "random") &&
          has_name(list, "urandom") && has_name(list, "console") && has_name(list, "ttyS0"));
    CHECK(has_name(list, "sda") && has_name(list, "nvme0n1"));
    CHECK(has_name(list, ".") && has_name(list, ".."));
    CHECK(has_name(list, "fb0") == fb_ready());

    Stat st = stat_ok("/dev/sda");
    CHECK(S_ISBLK(st.st_mode));
    EQ(st.st_size, 16 * 512);
    EQ(stat_ok("/dev/nvme0n1").st_size, 16 * 512);

    for(uint64_t i = 0; i < sizeof(sda_disk.data); i++){
        sda_disk.data[i] = (uint8_t)(i * 7 + 1);
    }
    File* f = open_ok("/dev/sda", O_RDWR);
    // Citire nealiniata, peste 3 sectoare
    seek(f, 700);
    EQ(rd(f, buf, 900), 900);
    CHECK(memcmp(buf, sda_disk.data + 700, 900) == 0);
    // Citire aliniata, 4 sectoare direct
    seek(f, 1024);
    EQ(rd(f, buf, 2048), 2048);
    CHECK(memcmp(buf, sda_disk.data + 1024, 2048) == 0);
    // Scriere nealiniata: sectoarele de la margini se citesc si se pastreaza, in afara intervalului nimic nu se schimba
    uint8_t before[16 * 512];
    memcpy(before, sda_disk.data, sizeof(before));
    memset(buf, 0xEE, 1100);
    seek(f, 600);
    EQ(wr(f, buf, 1100), 1100);
    for(uint64_t i = 0; i < sizeof(before); i++){
        uint8_t want = i >= 600 && i < 1700 ? 0xEE : before[i];
        if(sda_disk.data[i] != want){
            panic("test_devfs: sda[%lu] = %x, asteptat %x", i, sda_disk.data[i], want);
        }
    }
    // Capatul dispozitivului
    seek(f, 8000);
    EQ(rd(f, buf, 1000), 192);
    EQ(rd(f, buf, 1000), 0);
    EQ(wr(f, buf, 1), -ENOSPC);
    EQ(f->ops->lseek(f, 0, SEEK_END), 8192);
    file_put(f);

    f = open_ok("/dev/nvme0n1", O_WRONLY);
    memset(buf, 0x42, 512);
    EQ(wr(f, buf, 512), 512);
    file_put(f);
    CHECK(nvme_disk.data[0] == 0x42 && nvme_disk.data[511] == 0x42 && nvme_disk.data[512] == 0);
}

}

void test_devfs_run(){
    check_basic();
    kprintf("[OK] test_devfs null zero\n");
    check_random();
    kprintf("[OK] test_devfs random (%s)\n", devfs_has_rdrand() ? "RDRAND" : "TSC");
    check_console();
    kprintf("[OK] test_devfs console\n");
    check_fb();
    kprintf("[OK] test_devfs fb0\n");
    check_block();
    kprintf("[OK] test_devfs block\n");
    kprintf("[OK] test_devfs (null, zero, random, console, ttyS0, fb0, sda/nvme0n1 din HAL, fara udev)\n");
}
