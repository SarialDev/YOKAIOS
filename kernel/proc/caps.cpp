#include "proc/caps.h"
#include "proc/cred.h"
#include "proc/process.h"
#include "proc/file.h"
#include "fs/vfs/inode.h"
#include "sched/sched.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "uapi/fs.h"
#include "fs/vfs/vfs.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/percpu.h"
#include "irqlock.h"
#include "mm/kmalloc.h"
#include "lib/string.h"

const Caps caps_full = {CAP_ALL, CAP_ALL};

namespace {

struct CapName {
    const char* name;
    uint64_t bit;
};

const CapName cap_names[] = {
    {"sys_admin", CAP_SYS_ADMIN}, {"net", CAP_NET},   {"rawio", CAP_RAWIO}, {"rt", CAP_RT},
    {"mount", CAP_MOUNT},         {"kill", CAP_KILL}, {"setuid", CAP_SETUID},
};

uint64_t str_len(const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

// s[0..n) == name?
bool word_is(const char* s, uint64_t n, const char* name){
    return str_len(name) == n && memcmp(s, name, n) == 0;
}

bool is_space(char c){
    return c == ' ' || c == '\t';
}

}

const Caps& caps_current(){
    Process* p = process_current();
    return p ? p->caps : caps_full;
}

bool caps_has(const Caps& c, uint64_t cap){
    if(cap == CAP_MOUNT && (c.effective & CAP_SYS_ADMIN)){
        return true;
    }
    return (c.effective & cap) == cap;
}

bool capable(uint64_t cap){
    return caps_has(caps_current(), cap);
}

bool caps_may_bind(const Caps& c, uint16_t port){
    return port >= CAP_PORT_PRIVILEGED || caps_has(c, CAP_NET);
}

void caps_on_setid(Caps& c, const Cred& before, const Cred& after){
    bool had_root = before.uid == 0 || before.euid == 0 || before.suid == 0;
    bool has_root = after.uid == 0 || after.euid == 0 || after.suid == 0;
    if(had_root && !has_root){
        c.permitted = c.effective = 0;
        return;
    }
    if(before.euid == 0 && after.euid != 0){
        c.effective = 0;
    } else if(before.euid != 0 && after.euid == 0){
        c.effective = c.permitted;
    }
}

void caps_limit(Caps& c, uint64_t mask){
    c.permitted &= mask;
    c.effective &= mask;
}

bool caps_parse(const char* s, uint64_t& out){
    out = 0;
    while(*s){
        const char* w = s;
        while(*s && *s != ','){
            s++;
        }
        uint64_t n = (uint64_t)(s - w);
        if(*s == ','){
            s++;
        }
        if(n == 0 || word_is(w, n, "none")){
            continue;
        }
        if(word_is(w, n, "all")){
            out |= CAP_ALL;
            continue;
        }
        if(n > 2 && w[0] == '0' && w[1] == 'x'){
            uint64_t v = 0;
            for(uint64_t i = 2; i < n; i++){
                char ch = w[i];
                uint64_t d = ch >= '0' && ch <= '9' ? (uint64_t)(ch - '0') : ch >= 'a' && ch <= 'f' ? (uint64_t)(ch - 'a' + 10) : 99;
                if(d > 15){
                    return false;
                }
                v = v * 16 + d;
            }
            out |= v & CAP_ALL;
            continue;
        }
        bool found = false;
        for(const CapName& cn : cap_names){
            if(word_is(w, n, cn.name)){
                out |= cn.bit;
                found = true;
            }
        }
        if(!found){
            return false;
        }
    }
    return true;
}

void caps_format(uint64_t mask, char* buf, uint64_t size){
    uint64_t len = 0;
    for(const CapName& cn : cap_names){
        if(!(mask & cn.bit)){
            continue;
        }
        uint64_t n = str_len(cn.name);
        if(len + n + 2 > size){
            break;
        }
        if(len){
            buf[len++] = ',';
        }
        memcpy(buf + len, cn.name, n);
        len += n;
    }
    if(!len){
        memcpy(buf, "none", 4);
        len = 4;
    }
    buf[len] = 0;
}

// ---- Politica de exec ----

namespace {

const uint64_t POLICY_MAX = 32768;

// Continutul unui fisier de politica (kmalloc, terminat cu 0); nullptr daca lipseste / e prea mare / e gol
char* read_policy(const char* path, uint64_t& size){
    File* f;
    if(vfs_open(Path{}, path, O_RDONLY, 0, f) != 0){
        return nullptr;
    }
    char* buf = nullptr;
    size = 0;
    if(f->inode->size > 0 && f->inode->size <= POLICY_MAX){
        buf = (char*)kmalloc(f->inode->size + 1);
    }
    while(buf && size < f->inode->size){
        int64_t n = f->ops->read(f, buf + size, f->inode->size - size, false);
        if(n <= 0){
            break;
        }
        size += (uint64_t)n;
    }
    file_put(f);
    if(buf){
        buf[size] = 0;
    }
    return buf;
}

// Cauta calea canonica in fisierul de politica; ultima aparitie castiga
void scan_policy(const char* file, const char* canon, uint64_t canon_len, bool& found, uint64_t& mask){
    uint64_t size;
    char* buf = read_policy(file, size);
    if(!buf){
        return;
    }
    for(uint64_t i = 0; i < size;){
        uint64_t end = i;
        while(end < size && buf[end] != '\n'){
            end++;
        }
        uint64_t p = i;
        i = end + 1;
        while(p < end && is_space(buf[p])){
            p++;
        }
        if(p >= end || buf[p] == '#'){
            continue;
        }
        uint64_t path_end = p;
        while(path_end < end && !is_space(buf[path_end])){
            path_end++;
        }
        if(path_end - p != canon_len || memcmp(buf + p, canon, canon_len) != 0){
            continue;
        }
        uint64_t q = path_end;
        while(q < end && is_space(buf[q])){
            q++;
        }
        uint64_t q_end = q;
        while(q_end < end && !is_space(buf[q_end])){
            q_end++;
        }
        char list[64];
        if(q_end - q >= sizeof(list)){
            continue;
        }
        memcpy(list, buf + q, q_end - q);
        list[q_end - q] = 0;
        uint64_t m;
        if(caps_parse(list, m)){
            found = true;
            mask = m;                       // lista invalida: linia se ignora
        }
    }
    kfree(buf);
}

}

void caps_exec_policy(const Path& base, const char* path, bool& found, uint64_t& mask){
    found = false;
    mask = 0;
    char canon[512];
    int n = vfs_realpath(base, path, canon, sizeof(canon));
    if(n <= 1){
        return;                             // cale prea lunga sau disparuta: fara politica
    }
    scan_policy("/etc/exec.caps", canon, (uint64_t)n - 1, found, mask);
    scan_policy("/var/lib/ypkg/exec.caps", canon, (uint64_t)n - 1, found, mask);
}

// ---- Syscall-uri ----

namespace {

struct CapQuery {
    cap_state st;
    bool found;
};

void visit_caps(Task* t, void* arg){
    CapQuery* q = (CapQuery*)arg;
    SpinlockGuard lock(process_tree_lock());
    if(t->proc && t->proc->state == ProcState::ALIVE){
        q->st.effective = t->proc->caps.effective;
        q->st.permitted = t->proc->caps.permitted;
        q->found = true;
    }
}

}

int64_t sys_capget(uint64_t pid, uint64_t out, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* self = process_current();
    if(!self){
        return -EINVAL;
    }
    int64_t id = (int64_t)(int32_t)pid;
    CapQuery q = {};
    if(id == 0 || (uint64_t)id == self->pid){
        q.st.effective = self->caps.effective;
        q.st.permitted = self->caps.permitted;
    } else if(id < 0 || !task_visit((uint64_t)id, visit_caps, &q) || !q.found){
        return -ESRCH;
    }
    return user_access_ok(out, sizeof(q.st), true) && copy_to_user(out, &q.st, sizeof(q.st)) ? 0 : -EFAULT;
}

int64_t sys_capset(uint64_t in, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p){
        return -EINVAL;
    }
    cap_state st;
    if(!user_access_ok(in, sizeof(st), false) || !copy_from_user(&st, in, sizeof(st))){
        return -EFAULT;
    }
    if((st.effective | st.permitted) & ~CAP_ALL){
        return -EINVAL;
    }
    // Nu se poate ridica nimic: noul permitted in cel vechi, noul effective in noul permitted
    if((st.permitted & ~p->caps.permitted) || (st.effective & ~st.permitted)){
        return -EPERM;
    }
    p->caps.permitted = st.permitted;
    p->caps.effective = st.effective;
    return 0;
}

int64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy, uint64_t param, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p){
        return -EINVAL;
    }
    int64_t id = (int64_t)(int32_t)pid;
    if(id < 0){
        return -EINVAL;
    }
    if(id != 0 && (uint64_t)id != p->pid){
        return -EPERM;                      // doar procesul curent
    }
    sched_param sp;
    if(!user_access_ok(param, sizeof(sp), false) || !copy_from_user(&sp, param, sizeof(sp))){
        return -EFAULT;
    }
    int pol = (int)(int64_t)policy;
    if(pol == SCHED_OTHER){
        if(sp.sched_priority != 0){
            return -EINVAL;
        }
        sched_set_class(task_current(), TaskClass::NORMAL, 0);
        return 0;
    }
    if(pol != SCHED_FIFO || sp.sched_priority < 1 || sp.sched_priority > 99){
        return -EINVAL;
    }
    if(!capable(CAP_RT)){
        return -EPERM;
    }
    sched_set_class(task_current(), TaskClass::RT, (uint8_t)sp.sched_priority);
    return 0;
}

int64_t sys_sched_getscheduler(uint64_t pid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    int64_t id = (int64_t)(int32_t)pid;
    if(!p || id < 0 || (id != 0 && (uint64_t)id != p->pid)){
        return -ESRCH;
    }
    return task_current()->cls == TaskClass::RT ? SCHED_FIFO : SCHED_OTHER;
}

int64_t sys_ioperm(uint64_t from, uint64_t num, uint64_t on, uint64_t, uint64_t, uint64_t){
    if(!process_current()){
        return -EINVAL;
    }
    if(from > 0xFFFF || num > 0x10000 || from + num > 0x10000){
        return -EINVAL;
    }
    if(!capable(CAP_RAWIO)){
        return -EPERM;
    }
    Task* t = task_current();
    if(!t->iomap){
        if(!on){
            return 0;
        }
        t->iomap = (uint8_t*)kmalloc(IOMAP_BYTES);
        if(!t->iomap){
            return -ENOMEM;
        }
        memset(t->iomap, 0xFF, IOMAP_BYTES);
    }
    IrqLock lock;
    for(uint64_t port = from; port < from + num; port++){
        if(on){
            t->iomap[port / 8] &= (uint8_t)~(1u << (port % 8));
        } else {
            t->iomap[port / 8] |= (uint8_t)(1u << (port % 8));
        }
    }
    tss_set_iomap(t->iomap);
    return 0;
}

void caps_exec_reset_io(){
    Task* t = task_current();
    if(t->iomap){
        uint8_t* old = t->iomap;
        {
            IrqLock lock;
            t->iomap = nullptr;
            tss_set_iomap(nullptr);
        }
        kfree(old);
    }
}
