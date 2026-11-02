#include "proc/file.h"
#include "uapi/syscall_nr.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "drivers/tty.h"

namespace {

const uint32_t CONSOLE_REFS = 1u << 30;     // nu ajunge niciodata la 0

// fd 0-2 ale proceselor pornite de kernel: terminalul (tastatura / ecran + serial); EOF cat timp tty nu e activat
int64_t console_read(File*, void* buf, uint64_t len, bool nonblock){
    return tty_read(buf, len, nonblock);
}

int64_t console_write(File*, const void* buf, uint64_t len, bool){
    return tty_write(buf, len);
}

void console_close(File*){
}

const FileOps console_ops = {console_read, console_write, console_close, nullptr, nullptr};
File console = {&console_ops, nullptr, CONSOLE_REFS, nullptr, nullptr, 0, 0};

}

File* file_alloc(const FileOps* ops, void* priv){
    File* f = (File*)kmalloc(sizeof(File));
    if(f){
        f->ops = ops;
        f->priv = priv;
        f->refs = 1;
        f->inode = nullptr;
        f->dentry = nullptr;
        f->pos = 0;
        f->flags = 0;
    }
    return f;
}

void file_get(File* f){
    __atomic_fetch_add(&f->refs, 1, __ATOMIC_RELAXED);
}

void file_put(File* f){
    if(__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0){
        f->ops->close(f);
        kfree(f);
    }
}

File* console_file(){
    return &console;
}

int fd_install(FdTable& t, File* f){
    for(int i = 0; i < PROC_MAX_FD; i++){
        if(!t.fd[i]){
            t.fd[i] = f;
            return i;
        }
    }
    return -EMFILE;
}

File* fd_get(const FdTable& t, int64_t fd){
    return fd >= 0 && fd < PROC_MAX_FD ? t.fd[fd] : nullptr;
}

int fd_close(FdTable& t, int64_t fd){
    File* f = fd_get(t, fd);
    if(!f){
        return -EBADF;
    }
    t.fd[fd] = nullptr;
    file_put(f);
    return 0;
}

void fd_copy(FdTable& dst, const FdTable& src){
    for(int i = 0; i < PROC_MAX_FD; i++){
        dst.fd[i] = src.fd[i];
        if(dst.fd[i]){
            file_get(dst.fd[i]);
        }
    }
}

void fd_close_all(FdTable& t){
    for(int i = 0; i < PROC_MAX_FD; i++){
        if(t.fd[i]){
            fd_close(t, i);
        }
    }
}

void fd_init_console(FdTable& t){
    for(int i = 0; i < 3; i++){
        file_get(console_file());
        t.fd[i] = console_file();
    }
}
