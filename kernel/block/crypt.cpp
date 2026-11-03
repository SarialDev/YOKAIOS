#include "block/crypt.h"
#include "block/blockdev.h"
#include "block/bcache.h"
#include "fs/vfs/vfs.h"
#include "fs/vfs/mount.h"
#include "hal/hal.h"
#include "mm/kmalloc.h"
#include "proc/caps.h"
#include "sync/mutex.h"
#include "syscall/syscall.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"
#include "crypto/ykcrypt.h"
#include "crypto/xts.h"
#include "crypto/chacha20.h"
#include "crypto/sha256.h"

namespace {

const uint32_t SECTOR = YKC_SECTOR;
const uint32_t BOUNCE_SECTORS = 128;            // scrierile se cripteaza intr-un buffer de 64 KiB

class CryptDevice final : public BlockDevice {
public:
    uint32_t block_size() const override { return SECTOR; }
    uint64_t block_count() const override { return active ? sectors : 0; }
    int read(uint64_t lba, uint32_t n, void* buf) override;
    int write(uint64_t lba, uint32_t n, const void* buf) override;
    int flush() override;

    Mutex io{"crypt-io"};
    bool registered = false;
    bool active = false;
    uint32_t cipher = 0;
    bool hw = false;
    uint64_t sectors = 0;                       // sectoare vizibile
    uint64_t offset = 0;                        // sectoare de antet pe parinte
    BlockDevice* parent = nullptr;
    char parent_name[CRYPT_DEV_MAX] = "";
    char name[CRYPT_NAME_MAX] = "";
    uint8_t uuid[16] = {};
    xts_ctx xts;
    uint8_t chacha_key[32] = {};
    uint8_t* bounce = nullptr;

    void crypt_sectors(bool decrypt, uint64_t lba, uint32_t n, const uint8_t* in, uint8_t* out);
};

// Obiectele au constructori (Mutex, vtable): se creeaza la prima folosire, nu ca globale (kernelul nu ruleaza constructorii statici)
CryptDevice* slots[CRYPT_SLOTS];
char slot_names[CRYPT_SLOTS][CRYPT_NAME_MAX];
Mutex ctl_lock("crypt-ctl");

// Cifreaza / decripteaza n sectoare consecutive incepand cu lba (numarul sectorului vazut de utilizator = tweak)
void CryptDevice::crypt_sectors(bool decrypt, uint64_t lba, uint32_t n, const uint8_t* in, uint8_t* out){
    for(uint32_t i = 0; i < n; i++){
        uint64_t s = lba + i;
        const uint8_t* src = in + (uint64_t)i * SECTOR;
        uint8_t* dst = out + (uint64_t)i * SECTOR;
        if(cipher == YKC_AES_XTS){
            if(decrypt){
                xts_decrypt(&xts, s, src, dst, SECTOR);
            } else {
                xts_encrypt(&xts, s, src, dst, SECTOR);
            }
        } else {
            uint8_t nonce[12] = {};
            for(int k = 0; k < 8; k++){
                nonce[k] = (uint8_t)(s >> (8 * k));
            }
            chacha20_xor(chacha_key, 0, nonce, src, dst, SECTOR);
        }
    }
}

int CryptDevice::read(uint64_t lba, uint32_t n, void* buf){
    MutexGuard g(io);
    if(!active){
        return -ENODEV;
    }
    if(lba > sectors || n > sectors - lba){
        return -EINVAL;
    }
    int err = parent->read(offset + lba, n, buf);
    if(!err){
        crypt_sectors(true, lba, n, (const uint8_t*)buf, (uint8_t*)buf);
    }
    return err;
}

int CryptDevice::write(uint64_t lba, uint32_t n, const void* buf){
    MutexGuard g(io);
    if(!active){
        return -ENODEV;
    }
    if(lba > sectors || n > sectors - lba){
        return -EINVAL;
    }
    const uint8_t* p = (const uint8_t*)buf;
    while(n){
        uint32_t m = n < BOUNCE_SECTORS ? n : BOUNCE_SECTORS;
        crypt_sectors(false, lba, m, p, bounce);
        int err = parent->write(offset + lba, m, bounce);
        if(err){
            return err;
        }
        lba += m;
        p += (uint64_t)m * SECTOR;
        n -= m;
    }
    return 0;
}

int CryptDevice::flush(){
    MutexGuard g(io);
    return active ? parent->flush() : -ENODEV;
}

bool name_eq(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return *a == *b;
}

const char* strip_dev(const char* n){
    return memcmp(n, "/dev/", 5) == 0 ? n + 5 : n;
}

BlockDevice* find_block(const char* name){
    name = strip_dev(name);
    for(int k = 0; k < hal_count(HalKind::Block); k++){
        if(name_eq(hal_name(HalKind::Block, k), name)){
            return hal_block(k);
        }
    }
    return nullptr;
}

// Dispozitivul (numele HAL) apare ca sursa a unei montari?
bool is_mounted(const char* devname){
    MountInfo* list = (MountInfo*)kmalloc(sizeof(MountInfo) * MOUNT_SNAPSHOT_MAX);
    if(!list){
        return true;                            // prudent
    }
    uint64_t n;
    {
        MutexGuard g(vfs_mutex());
        n = mount_snapshot(list, MOUNT_SNAPSHOT_MAX);
    }
    bool found = false;
    for(uint64_t i = 0; i < n; i++){
        if(name_eq(strip_dev(list[i].dev_name), devname)){
            found = true;
        }
    }
    kfree(list);
    return found;
}

int slot_by_name(const char* name){
    for(int i = 0; i < CRYPT_SLOTS; i++){
        if(slots[i] && slots[i]->registered && name_eq(slots[i]->name, name)){
            return i;
        }
    }
    return -1;
}

void slot_clear(CryptDevice& d){
    xts_wipe(&d.xts);
    crypto_wipe(d.chacha_key, sizeof(d.chacha_key));
    kfree(d.bounce);
    d.bounce = nullptr;
    d.parent = nullptr;
    d.parent_name[0] = 0;
    d.sectors = 0;
}

int do_open(const char* dev, const void* pass, uint32_t passlen, uint32_t flags, char* name){
    if(passlen == 0 || passlen > CRYPT_PASS_MAX){
        return -EINVAL;
    }
    MutexGuard g(ctl_lock);
    const char* devname = strip_dev(dev);
    BlockDevice* parent = find_block(dev);
    if(!parent){
        return -ENODEV;
    }
    for(int i = 0; i < CRYPT_SLOTS; i++){
        if(slots[i] && slots[i]->active && slots[i]->parent == parent){
            return -EBUSY;                      // deja deschis
        }
    }
    for(int s = 0; s < CRYPT_SLOTS; s++){
        if(parent == slots[s]){
            return -EINVAL;                     // nu se stivuieste un cryptN peste alt cryptN
        }
    }
    if(is_mounted(devname)){
        return -EBUSY;
    }
    if(parent->block_size() != SECTOR){
        return -EINVAL;
    }
    // slotul cerut sau primul liber
    int idx = -1;
    if(name[0]){
        if(memcmp(name, "crypt", 5) != 0 || name[5] < '0' || name[5] >= '0' + CRYPT_SLOTS || name[6]){
            return -EINVAL;
        }
        idx = name[5] - '0';
        if(slots[idx] && slots[idx]->active){
            return -EBUSY;
        }
    } else {
        for(int i = 0; i < CRYPT_SLOTS && idx < 0; i++){
            if(!slots[i] || !slots[i]->active){
                idx = i;
            }
        }
        if(idx < 0){
            return -EMFILE;
        }
    }
    uint8_t* hdr = (uint8_t*)kmalloc(YKC_HDR_SIZE);
    if(!hdr){
        return -ENOMEM;
    }
    int err = block_read_bytes(parent, 0, hdr, YKC_HDR_SIZE);
    ykc_info info;
    uint8_t master[YKC_MASTER_LEN];
    if(!err){
        err = ykc_parse(hdr, &info) ? -EINVAL : 0;
    }
    uint64_t parent_bytes = (uint64_t)parent->block_count() * SECTOR;
    if(!err && (info.payload_offset > parent_bytes || info.payload_bytes > parent_bytes - info.payload_offset)){
        err = -EINVAL;
    }
    if(!err){
        int rc = ykc_unlock(hdr, pass, passlen, master);
        err = rc == -2 ? -EACCES : rc ? -EINVAL : 0;
    }
    kfree(hdr);
    if(err){
        return err;
    }
    if(!slots[idx]){
        slots[idx] = new CryptDevice();
        if(!slots[idx]){
            crypto_wipe(master, sizeof(master));
            return -ENOMEM;
        }
    }
    CryptDevice& d = *slots[idx];
    uint8_t* bounce = (uint8_t*)kmalloc((uint64_t)BOUNCE_SECTORS * SECTOR);
    if(!bounce){
        crypto_wipe(master, sizeof(master));
        return -ENOMEM;
    }
    {
        MutexGuard io(d.io);
        d.cipher = info.cipher;
        aes_force_software((flags & CRYPT_FLAG_FORCE_SOFT) != 0);
        if(info.cipher == YKC_AES_XTS){
            err = xts_set_key(&d.xts, master, YKC_MASTER_LEN) ? -EINVAL : 0;
            d.hw = d.xts.data.hw != 0;
        } else {
            memcpy(d.chacha_key, master, 32);
            d.hw = false;
        }
        aes_force_software(0);
        crypto_wipe(master, sizeof(master));
        if(err){
            kfree(bounce);
            return err;
        }
        d.bounce = bounce;
        d.parent = parent;
        d.offset = info.payload_offset / SECTOR;
        d.sectors = info.payload_bytes / SECTOR;
        memcpy(d.uuid, info.uuid, 16);
        uint64_t l = 0;
        while(devname[l] && l < CRYPT_DEV_MAX - 1){
            d.parent_name[l] = devname[l];
            l++;
        }
        d.parent_name[l] = 0;
        d.active = true;
    }
    if(!d.registered){
        memcpy(slot_names[idx], "crypt0", 7);
        slot_names[idx][5] = (char)('0' + idx);
        memcpy(d.name, slot_names[idx], 7);
        if(!hal_register(HalKind::Block, &d, slot_names[idx])){
            MutexGuard io(d.io);
            d.active = false;
            slot_clear(d);
            return -ENOSPC;
        }
        d.registered = true;
    }
    memcpy(name, d.name, CRYPT_NAME_MAX);
    kprintf("crypt: %s = %s (%s%s, %lu sectoare)\n", d.name, d.parent_name,
            d.cipher == YKC_AES_XTS ? "aes-xts-plain64" : "chacha20-plain64", d.hw ? ", AES-NI" : "", d.sectors);
    return 0;
}

int do_close(const char* name){
    MutexGuard g(ctl_lock);
    int idx = slot_by_name(name);
    if(idx < 0){
        return -ENODEV;
    }
    CryptDevice& d = *slots[idx];
    if(!d.active){
        return -ENODEV;
    }
    if(is_mounted(d.name)){
        return -EBUSY;
    }
    // Blocurile murdare din cache se scriu inainte sa dispara cheia; daca raman blocuri tinute (pinned), ramane deschis
    int err = bcache().invalidate(&d);
    if(err){
        return err;
    }
    MutexGuard io(d.io);
    d.parent->flush();
    d.active = false;
    slot_clear(d);
    kprintf("crypt: %s inchis\n", d.name);
    return 0;
}

}

void crypt_init(){
    // sloturile se inregistreaza in HAL la prima deschidere
}

int crypt_open_kernel(const char* dev, const void* pass, uint32_t passlen, uint32_t flags, char* name){
    return do_open(dev, pass, passlen, flags, name);
}

int crypt_close_kernel(const char* name){
    return do_close(name);
}

int64_t sys_cryptctl(uint64_t op, uint64_t arg, uint64_t, uint64_t, uint64_t, uint64_t){
    if(!capable(CAP_SYS_ADMIN)){
        return -EPERM;
    }
    if(op == CRYPT_OP_OPEN){
        crypt_open_req* r = (crypt_open_req*)kzalloc(sizeof(crypt_open_req));
        if(!r){
            return -ENOMEM;
        }
        int64_t rc;
        if(!user_access_ok(arg, sizeof(*r), true) || !copy_from_user(r, arg, sizeof(*r))){
            rc = -EFAULT;
        } else {
            r->dev[CRYPT_DEV_MAX - 1] = 0;
            r->name[CRYPT_NAME_MAX - 1] = 0;
            rc = do_open(r->dev, r->pass, r->passlen, r->flags, r->name);
            if(!rc && !copy_to_user(arg, r, sizeof(*r))){
                rc = -EFAULT;
            }
        }
        crypto_wipe(r, sizeof(*r));
        kfree(r);
        return rc;
    }
    if(op == CRYPT_OP_CLOSE){
        char nm[CRYPT_NAME_MAX];
        if(!user_access_ok(arg, CRYPT_NAME_MAX, false) || !copy_from_user(nm, arg, CRYPT_NAME_MAX)){
            return -EFAULT;
        }
        nm[CRYPT_NAME_MAX - 1] = 0;
        return do_close(nm);
    }
    if(op == CRYPT_OP_INFO){
        crypt_info ci;
        if(!user_access_ok(arg, sizeof(ci), true) || !copy_from_user(&ci, arg, sizeof(ci))){
            return -EFAULT;
        }
        ci.name[CRYPT_NAME_MAX - 1] = 0;
        MutexGuard g(ctl_lock);
        int idx = slot_by_name(ci.name);
        if(idx < 0 || !slots[idx]->active){
            return -ENODEV;
        }
        CryptDevice& d = *slots[idx];
        memcpy(ci.dev, d.parent_name, CRYPT_DEV_MAX);
        ci.cipher = d.cipher;
        ci.hw_aes = d.hw;
        ci.sectors = d.sectors;
        ci.offset = d.offset;
        memcpy(ci.uuid, d.uuid, 16);
        return copy_to_user(arg, &ci, sizeof(ci)) ? 0 : -EFAULT;
    }
    return -EINVAL;
}
