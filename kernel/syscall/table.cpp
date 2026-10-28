#include "syscall/table.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "proc/process.h"
#include "ipc/pipe.h"
#include "exec/elf.h"
#include "sched/sched.h"
#include "mm/kmalloc.h"

namespace {

const uint64_t IO_CHUNK = 256;
const int MAX_EXEC_ARGS = 32;
const uint64_t MAX_EXEC_BYTES = 4096;
const uint64_t MAX_PATH = 64;

File* current_file(uint64_t fd){
    Process* p = process_current();
    return p ? fd_get(p->fds, (int64_t)fd) : nullptr;
}

int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len, uint64_t, uint64_t, uint64_t){
    File* f = current_file(fd);
    if(!f){
        return -EBADF;
    }
    if(!user_access_ok(buf, len, true)){
        return -EFAULT;
    }
    // Asteapta doar pentru primul fragment; dupa ce are date intoarce cat s-a putut citi fara blocare
    char chunk[IO_CHUNK];
    uint64_t done = 0;
    while(done < len){
        uint64_t want = len - done < IO_CHUNK ? len - done : IO_CHUNK;
        int64_t n = f->ops->read(f, chunk, want, done > 0);
        if(n <= 0){
            return done ? (int64_t)done : n;
        }
        if(!copy_to_user(buf + done, chunk, (uint64_t)n)){
            return done ? (int64_t)done : -EFAULT;
        }
        done += (uint64_t)n;
        if((uint64_t)n < want){
            break;
        }
    }
    return (int64_t)done;
}

int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len, uint64_t, uint64_t, uint64_t){
    File* f = current_file(fd);
    if(!f){
        return -EBADF;
    }
    if(!user_access_ok(buf, len, false)){
        return -EFAULT;
    }
    char chunk[IO_CHUNK];
    uint64_t done = 0;
    while(done < len){
        uint64_t n = len - done < IO_CHUNK ? len - done : IO_CHUNK;
        if(!copy_from_user(chunk, buf + done, n)){
            return done ? (int64_t)done : -EFAULT;
        }
        int64_t w = f->ops->write(f, chunk, n, false);
        if(w < 0){
            return done ? (int64_t)done : w;
        }
        done += (uint64_t)w;
        if((uint64_t)w < n){
            break;
        }
    }
    return (int64_t)done;
}

int64_t sys_close(uint64_t fd, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    return p ? fd_close(p->fds, (int64_t)fd) : -EBADF;
}

int64_t sys_pipe(uint64_t fds_user, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p){
        return -EINVAL;
    }
    if(!user_access_ok(fds_user, 2 * sizeof(int), true)){
        return -EFAULT;
    }
    File* rd;
    File* wr;
    int err = pipe_create(rd, wr);
    if(err){
        return err;
    }
    int fds[2] = {fd_install(p->fds, rd), -1};
    if(fds[0] >= 0){
        fds[1] = fd_install(p->fds, wr);
    }
    if(fds[1] < 0){
        if(fds[0] >= 0){
            fd_close(p->fds, fds[0]);
        } else {
            file_put(rd);
        }
        file_put(wr);
        return -EMFILE;
    }
    copy_to_user(fds_user, fds, sizeof(fds));
    return 0;
}

int64_t sys_yield(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    yield();
    return 0;
}

int64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    return (int64_t)task_current()->pid;
}

int64_t sys_getppid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    return p ? (int64_t)__atomic_load_n(&p->ppid, __ATOMIC_RELAXED) : 0;
}

int64_t sys_fork(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    return process_fork(*syscall_frame());
}

// argv/envp si sirurile lor, copiate in kernel inainte ca imaginea veche sa dispara
struct ExecArgs {
    char path[MAX_PATH];
    const char* argv[MAX_EXEC_ARGS + 1];
    const char* envp[MAX_EXEC_ARGS + 1];
    char strings[MAX_EXEC_BYTES];
};

int64_t copy_vector(ExecArgs& a, const char** out, uint64_t user_vec, uint64_t& used){
    int n = 0;
    for(; user_vec; n++){
        uint64_t ptr;
        if(!copy_from_user(&ptr, user_vec + n * sizeof(uint64_t), sizeof(ptr))){
            return -EFAULT;
        }
        if(!ptr){
            break;
        }
        if(n == MAX_EXEC_ARGS){
            return -E2BIG;
        }
        int64_t len = copy_string_from_user(a.strings + used, ptr, MAX_EXEC_BYTES - used);
        if(len < 0){
            return len;
        }
        out[n] = a.strings + used;
        used += (uint64_t)len + 1;
    }
    out[n] = nullptr;
    return 0;
}

int64_t sys_execve(uint64_t path, uint64_t argv, uint64_t envp, uint64_t, uint64_t, uint64_t){
    ExecArgs* a = (ExecArgs*)kmalloc(sizeof(ExecArgs));
    if(!a){
        return -ENOMEM;
    }
    uint64_t used = 0;
    int64_t err = copy_string_from_user(a->path, path, MAX_PATH);
    if(err >= 0){
        err = copy_vector(*a, a->argv, argv, used);
    }
    if(err >= 0){
        err = copy_vector(*a, a->envp, envp, used);
    }
    if(err >= 0){
        // Pana la VFS, calea numeste un modul Multiboot2: "/boot/hello", "/hello" si "hello" sunt acelasi
        const char* name = a->path;
        if(name[0] == '/' && name[1] == 'b' && name[2] == 'o' && name[3] == 'o' && name[4] == 't' && name[5] == '/'){
            name += 6;
        }
        while(*name == '/'){
            name++;
        }
        // La reusita nu revine: a se elibereaza dupa ce argumentele sunt pe stiva noului program
        err = exec_replace_module(name, a->argv, a->envp, a);
    }
    kfree(a);
    return err;
}

int64_t sys_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    process_exit((int)code);
}

int64_t sys_wait4(uint64_t pid, uint64_t status_user, uint64_t options, uint64_t, uint64_t, uint64_t){
    if(options & ~(uint64_t)WNOHANG){
        return -EINVAL;
    }
    if(status_user && !user_access_ok(status_user, sizeof(int), true)){
        return -EFAULT;
    }
    int status = 0;
    int64_t r = process_waitpid((int64_t)pid, &status, (int)options);
    if(r > 0 && status_user && !copy_to_user(status_user, &status, sizeof(status))){
        return -EFAULT;
    }
    return r;
}

struct Table {
    SyscallFn fn[SYS_COUNT];
};

constexpr Table make_table(){
    Table t{};
    t.fn[SYS_READ] = sys_read;
    t.fn[SYS_WRITE] = sys_write;
    t.fn[SYS_CLOSE] = sys_close;
    t.fn[SYS_PIPE] = sys_pipe;
    t.fn[SYS_YIELD] = sys_yield;
    t.fn[SYS_GETPID] = sys_getpid;
    t.fn[SYS_FORK] = sys_fork;
    t.fn[SYS_EXECVE] = sys_execve;
    t.fn[SYS_EXIT] = sys_exit;
    t.fn[SYS_WAIT4] = sys_wait4;
    t.fn[SYS_GETPPID] = sys_getppid;
    return t;
}

constexpr Table table = make_table();

}

SyscallFn syscall_lookup(uint64_t nr){
    return nr < SYS_COUNT ? table.fn[nr] : nullptr;
}
