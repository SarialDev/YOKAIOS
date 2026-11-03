#include "core/verify.h"
#include "boot/multiboot2.h"
#include "mm/kmalloc.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "lib/string.h"
#include "crypto/ed25519.h"

// build/yokai_pubkey.c (generat din cheia de semnare; vezi Makefile)
extern "C" const uint8_t yokai_sign_pubkey[32];

namespace {

const char CTX_PKG[] = "yokai-pkg-v1\n";
const char CTX_KMOD[] = "yokai-kmod-v1\n";
const char CTX_KERNEL[] = "yokai-kernel-v1\n";
const uint8_t MODULE_MAGIC[SIG_MODULE_MAGIC_SIZE] = {'~', 'Y', 'K', 'S', 'I', 'G', '~', '\n'};

}

const uint8_t* sign_pubkey(){
    return yokai_sign_pubkey;
}

bool sign_verify(SignKind kind, const void* data, uint64_t len, const uint8_t* sig){
    const char* ctx;
    size_t ctx_len;
    switch(kind){
    case SignKind::Package: ctx = CTX_PKG; ctx_len = sizeof(CTX_PKG) - 1; break;
    case SignKind::Module: ctx = CTX_KMOD; ctx_len = sizeof(CTX_KMOD) - 1; break;
    case SignKind::Kernel: ctx = CTX_KERNEL; ctx_len = sizeof(CTX_KERNEL) - 1; break;
    default: return false;
    }
    return ed25519_verify(sig, yokai_sign_pubkey, ctx, ctx_len, data, len) == 0;
}

bool sign_permissive(){
    const char* v = cmdline_get("sign=");
    const char want[] = "permissive";
    if(!v){
        return false;
    }
    size_t i = 0;
    for(; want[i]; i++){
        if(v[i] != want[i]){
            return false;
        }
    }
    return v[i] == 0 || v[i] == ' ';
}

int sign_check_module(const void* blob, uint64_t len, uint64_t& elf_len){
    elf_len = 0;
    const uint8_t* b = (const uint8_t*)blob;
    if(len < SIG_SIZE + SIG_MODULE_MAGIC_SIZE + 64 || memcmp(b + len - SIG_MODULE_MAGIC_SIZE, MODULE_MAGIC, SIG_MODULE_MAGIC_SIZE) != 0){
        return -ENOKEY;
    }
    uint64_t n = len - SIG_SIZE - SIG_MODULE_MAGIC_SIZE;
    if(!sign_verify(SignKind::Module, b, n, b + n)){
        return -EKEYREJECTED;
    }
    elf_len = n;
    return 0;
}

int64_t sys_sigverify(uint64_t op, uint64_t data, uint64_t len, uint64_t sig, uint64_t, uint64_t){
    if(op == 1){
        return sign_permissive() ? 1 : 0;
    }
    if(op != 0){
        return -EINVAL;
    }
    if(len == 0 || len > SIG_VERIFY_MAX){
        return -EINVAL;
    }
    uint8_t s[SIG_SIZE];
    if(!user_access_ok(sig, SIG_SIZE, false) || !copy_from_user(s, sig, SIG_SIZE) || !user_access_ok(data, len, false)){
        return -EFAULT;
    }
    uint8_t* buf = (uint8_t*)kmalloc(len);
    if(!buf){
        return -ENOMEM;
    }
    int64_t rc = 0;
    if(!copy_from_user(buf, data, len)){
        rc = -EFAULT;
    } else if(!sign_verify(SignKind::Package, buf, len, s)){
        rc = -EKEYREJECTED;
    }
    kfree(buf);
    return rc;
}
