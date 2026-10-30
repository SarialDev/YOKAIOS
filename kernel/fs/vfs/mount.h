#pragma once
#include "fs/vfs/path.h"

// Un sistem de fisiere montat. Tine o referinta la radacina lui si una la mount point. Mount-urile nu au contor
// propriu: un mount e folosit cat timp un dentry din sb-ul lui e referit (fisier deschis, cwd)
struct Mount {
    Mount* parent;                  // nullptr la radacina arborelui
    Dentry* mountpoint;
    Dentry* root;
    SuperBlock* sb;
    Mount* next;
};

// Lock-ul VFS luat pentru toate
// Radacina arborelui (cu referinta), false daca nu e nimic montat
bool mount_root(Path& out);
// Daca p e un mount point, trece in radacina mount-ului montat deasupra (repetat, pentru montari stivuite)
void follow_mounts(Path& p);
// "Pe sub" mount: din radacina unui mount in directorul pe care e montat
void follow_up_mount(Path& p);
// flags: SuperBlock::flags. Primul mount trebuie sa fie pe "/". -ENODEV tip necunoscut, -ENOTDIR, -ENOENT
int mount_do(const Path& base, const char* dev_name, const char* target, const char* fstype, uint32_t flags,
             const char* data);
// target = radacina unui mount; -EINVAL altfel, -EBUSY daca are sub-mount-uri, fisiere deschise sau cwd in el
int umount_do(const Path& base, const char* target);
uint64_t mount_count();
