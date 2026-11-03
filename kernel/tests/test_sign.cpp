#include "tests/test_sign.h"
#include "core/verify.h"
#include "core/kmod.h"
#include "core/panic.h"
#include "fs/vfs/vfs.h"
#include "fs/vfs/inode.h"
#include "proc/file.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "uapi/fs.h"
#include "uapi/syscall_nr.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "crypto/ed25519.h"

namespace {

#define CHECK(cond) do { if(!(cond)){ panic("test_sign:%d: %s", __LINE__, #cond); } } while(0)

const char MODULE_PATH[] = "/lib/modules/hello_mod.ko";

uint8_t* read_file(const char* path, uint64_t& size){
    File* f;
    if(vfs_open(Path{}, path, O_RDONLY, 0, f) != 0){
        panic("test_sign: %s lipseste din initrd", path);
    }
    size = f->inode->size;
    uint8_t* buf = (uint8_t*)kmalloc(size);
    CHECK(buf != nullptr);
    uint64_t got = 0;
    while(got < size){
        int64_t n = f->ops->read(f, buf + got, size - got, false);
        if(n <= 0){
            break;
        }
        got += (uint64_t)n;
    }
    CHECK(got == size);
    file_put(f);
    return buf;
}

}

void test_sign_run(){
    uint64_t len;
    uint8_t* blob = read_file(MODULE_PATH, len);
    CHECK(len > 64 + 72);
    uint64_t elf_len = len - SIG_SIZE - SIG_MODULE_MAGIC_SIZE;
    kprintf("test_sign: modul %lu octeti (ELF %lu + semnatura 64 + magic 8)\n", len, elf_len);

    uint64_t n;
    CHECK(sign_check_module(blob, len, n) == 0 && n == elf_len);
    CHECK(sign_verify(SignKind::Module, blob, elf_len, blob + elf_len));
    // Domenii separate: semnatura de modul nu valoreaza ca semnatura de pachet sau de kernel
    CHECK(!sign_verify(SignKind::Package, blob, elf_len, blob + elf_len));
    CHECK(!sign_verify(SignKind::Kernel, blob, elf_len, blob + elf_len));
    kprintf("[OK] test_sign: semnatura de modul valida, domenii separate\n");

    uint64_t free_before = pmm_free_count();
    int mods_before = kmod_count();

    // Un octet schimbat oriunde in ELF => semnatura invalida, modulul nu se incarca
    const uint64_t flips[] = {0, 1, 17, 64, elf_len / 3, elf_len / 2, elf_len - 1};
    for(uint64_t at : flips){
        blob[at] ^= 0x01;
        CHECK(sign_check_module(blob, len, n) == -EKEYREJECTED);
        CHECK(kmod_load(blob, len) == -EKEYREJECTED);
        blob[at] ^= 0x01;
    }
    // Semnatura stricata (primul, ultimul octet, bitul S)
    const uint64_t sflips[] = {elf_len, elf_len + 31, elf_len + 32, elf_len + 63};
    for(uint64_t at : sflips){
        blob[at] ^= 0x80;
        CHECK(kmod_load(blob, len) == -EKEYREJECTED);
        blob[at] ^= 0x80;
    }
    // Fara magic / trunchiat / doar ELF-ul / gol => nesemnat
    blob[len - 1] ^= 1;
    CHECK(kmod_load(blob, len) == -ENOKEY);
    blob[len - 1] ^= 1;
    CHECK(kmod_load(blob, len - 1) == -ENOKEY);
    CHECK(kmod_load(blob, elf_len) == -ENOKEY);
    CHECK(kmod_load(blob, 0) == -ENOKEY);
    CHECK(kmod_load(blob, 40) == -ENOKEY);
    // Semnat de altcineva (seed arbitrar), cu contextul corect de modul
    {
        static const uint8_t other_seed[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                               17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
        uint8_t saved[SIG_SIZE];
        memcpy(saved, blob + elf_len, SIG_SIZE);
        ed25519_sign(blob + elf_len, other_seed, "yokai-kmod-v1\n", 14, blob, elf_len);
        CHECK(kmod_load(blob, len) == -EKEYREJECTED);
        memcpy(blob + elf_len, saved, SIG_SIZE);
    }
    CHECK(kmod_count() == mods_before && pmm_free_count() == free_before);
    kprintf("[OK] test_sign: modul modificat cu un octet / nesemnat / semnat de altcineva: refuzat (%lu pozitii)\n",
            (uint64_t)(sizeof(flips) / sizeof(flips[0]) + sizeof(sflips) / sizeof(sflips[0]) + 6));

    // Modulul bun: incarcare, init ruleaza (tipareste), nume unic, descarcare
    CHECK(!kmod_loaded("hello_mod"));
    CHECK(kmod_load(blob, len) == 0);
    CHECK(kmod_loaded("hello_mod") && kmod_count() == mods_before + 1);
    CHECK(kmod_load(blob, len) == -EEXIST);
    CHECK(kmod_unload("hello_mod") == 0);
    CHECK(!kmod_loaded("hello_mod") && kmod_unload("hello_mod") == -ENOENT);
    // a doua incarcare merge (alt interval virtual) si nu pierde pagini
    CHECK(kmod_load(blob, len) == 0 && kmod_unload("hello_mod") == 0);
    CHECK(kmod_count() == mods_before);
    CHECK(pmm_free_count() + 8 >= free_before);
    kprintf("[OK] test_sign: hello_mod incarcat, init/exit rulate, descarcat\n");
    kfree(blob);
    kprintf("[OK] test_sign (Ed25519: module de kernel semnate)\n");
}
