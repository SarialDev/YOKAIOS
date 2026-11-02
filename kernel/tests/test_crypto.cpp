#include "tests/test_crypto.h"
#include "crypto/pwhash.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/string.h"
#include "lib/kprintf.h"

namespace {

#define CHECK(cond) do { if(!(cond)){ panic("test_crypto:%d: %s", __LINE__, #cond); } } while(0)

const char* const envp[] = {"YOKAI=1", "PATH=/bin:/sbin", nullptr};

bool eq(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return *a == *b;
}

void to_hex(const uint8_t* b, size_t n, char* out){
    static const char d[] = "0123456789abcdef";
    for(size_t i = 0; i < n; i++){
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = 0;
}

// Vectorii principali, calculati de codul din kernel (acelasi lib/crypto ca in userland)
void kernel_side(){
    uint8_t d[32];
    char h[65];
    sha256("abc", 3, d);
    to_hex(d, 32, h);
    CHECK(eq(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    to_hex(d, 32, h);
    CHECK(eq(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

    uint8_t o[64];
    char hh[129];
    CHECK(pbkdf2_hmac_sha256("passwd", 6, "salt", 4, 1, o, 64) == 0);
    to_hex(o, 64, hh);
    CHECK(eq(hh, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));

    // hash cunoscut din /etc/shadow-ul de test ("hunter2") + gresit + stricat
    static const char known[] = "$pbkdf2-sha256$100000$000102030405060708090a0b0c0d0e0f$844d6e092def956a3d2773e41d721bf2d95a1d8f524d970ca76cd20528a03679";
    CHECK(pwhash_verify("hunter2", 7, known) == 1);
    CHECK(pwhash_verify("hunter3", 7, known) == 0);
    CHECK(pwhash_verify("hunter2", 7, "!") == -1);
    CHECK(crypto_ct_equal("abc", "abc", 3) == 1 && crypto_ct_equal("abc", "abd", 3) == 0);
}

}

void test_crypto_run(){
    __asm__ volatile("sti");
    kernel_side();
    kprintf("[OK] test_crypto kernel\n");

    static const char* const argv[] = {"cryptotest", nullptr};
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn("/bin/cryptotest", argv, envp);
    if(!pid){
        panic("test_crypto: /bin/cryptotest nu a pornit");
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_crypto: cryptotest (pid %lu) a disparut fara exit", pid);
    }
    if(code != 0){
        panic("test_crypto: cryptotest a iesit cu %d (vezi liniile \"cryptotest: FAIL\")", code);
    }
    if(process_count() != procs_before){
        panic("test_crypto: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 8 < free_before){
        panic("test_crypto: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_crypto (SHA-256, HMAC, PBKDF2, pwhash: kernel + userland)\n");
}
