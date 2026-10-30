#include "fs/pseudofs.h"
#include "fs/vfs/vfs.h"
#include "lib/kprintf.h"

int fs_mount_pseudo(){
    Path none = {};
    int err = vfs_mount(none, "devfs", "/dev", "devfs", 0, nullptr);
    if(err){
        kprintf("devfs: montarea pe /dev a esuat (%d)\n", err);
        return err;
    }
    kprintf("[OK] devfs montat pe /dev (random: %s)\n", devfs_has_rdrand() ? "RDRAND" : "TSC");
    err = vfs_mount(none, "tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
    if(!err){
        err = vfs_mount(none, "tmpfs", "/run", "tmpfs", 0, "mode=0755");
    }
    if(err){
        kprintf("tmpfs: montarea pe /tmp sau /run a esuat (%d)\n", err);
        return err;
    }
    kprintf("[OK] tmpfs montat pe /tmp si /run\n");
    err = vfs_mount(none, "proc", "/proc", "proc", 0, nullptr);
    if(err){
        kprintf("procfs: montarea pe /proc a esuat (%d)\n", err);
        return err;
    }
    kprintf("[OK] procfs montat pe /proc\n");
    return 0;
}

int fs_umount_pseudo(){
    Path none = {};
    int first = 0;
    const char* const targets[] = {"/proc", "/run", "/tmp", "/dev"};
    for(const char* t : targets){
        int err = vfs_umount(none, t);
        if(err && !first){
            first = err;
        }
    }
    return first;
}
