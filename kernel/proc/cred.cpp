#include "proc/cred.h"
#include "proc/process.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "uapi/fs.h"

const Cred cred_root = {};

const Cred& cred_current(){
    Process* p = process_current();
    return p ? p->cred : cred_root;
}

bool cred_is_root(const Cred& c){
    return c.euid == 0;
}

bool cred_in_group(const Cred& c, uint32_t gid){
    if(c.egid == gid){
        return true;
    }
    for(uint32_t i = 0; i < c.ngroups; i++){
        if(c.groups[i] == gid){
            return true;
        }
    }
    return false;
}

bool cred_may_signal(const Cred& c, const Cred& t){
    return cred_is_root(c) || c.uid == t.uid || c.uid == t.suid || c.euid == t.uid || c.euid == t.suid;
}

void cred_exec(Cred& c, uint32_t mode, uint32_t uid, uint32_t gid, bool nosuid){
    if(!nosuid && (mode & S_ISUID)){
        c.euid = uid;
    }
    if(!nosuid && (mode & S_ISGID) && (mode & 0010)){
        c.egid = gid;
    }
    c.suid = c.euid;
    c.sgid = c.egid;
}

uint32_t cred_umask(){
    Process* p = process_current();
    return p ? p->umask : 0;
}

namespace {

// Procesul curent; syscall-urile de mai jos vin doar din procese user
Cred& self(){
    return process_current()->cred;
}

// setuid/setgid: root schimba toate trei; altfel doar efectivul, si doar spre cel real sau salvat
int set_one(uint32_t& real, uint32_t& eff, uint32_t& saved, uint32_t id, bool root){
    if(id == CRED_NONE){
        return -EINVAL;
    }
    if(root){
        real = eff = saved = id;
        return 0;
    }
    if(id != real && id != saved){
        return -EPERM;
    }
    eff = id;
    return 0;
}

// setreuid/setregid ca pe Linux: realul poate lua real/efectiv, efectivul real/efectiv/salvat; salvatul devine noul
// efectiv daca realul s-a schimbat sau efectivul difera de vechiul real
int set_re(uint32_t& real, uint32_t& eff, uint32_t& saved, uint32_t r, uint32_t e, bool root){
    if(!root){
        if(r != CRED_NONE && r != real && r != eff){
            return -EPERM;
        }
        if(e != CRED_NONE && e != real && e != eff && e != saved){
            return -EPERM;
        }
    }
    uint32_t old_real = real;
    if(r != CRED_NONE){
        real = r;
    }
    if(e != CRED_NONE){
        eff = e;
    }
    if(r != CRED_NONE || (e != CRED_NONE && e != old_real)){
        saved = eff;
    }
    return 0;
}

}

int64_t sys_getuid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    return self().uid;
}

int64_t sys_geteuid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    return self().euid;
}

int64_t sys_getgid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    return self().gid;
}

int64_t sys_getegid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    return self().egid;
}

int64_t sys_setuid(uint64_t uid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Cred& c = self();
    return set_one(c.uid, c.euid, c.suid, (uint32_t)uid, cred_is_root(c));
}

int64_t sys_setgid(uint64_t gid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Cred& c = self();
    return set_one(c.gid, c.egid, c.sgid, (uint32_t)gid, cred_is_root(c));
}

int64_t sys_setreuid(uint64_t ruid, uint64_t euid, uint64_t, uint64_t, uint64_t, uint64_t){
    Cred& c = self();
    return set_re(c.uid, c.euid, c.suid, (uint32_t)ruid, (uint32_t)euid, cred_is_root(c));
}

int64_t sys_setregid(uint64_t rgid, uint64_t egid, uint64_t, uint64_t, uint64_t, uint64_t){
    Cred& c = self();
    return set_re(c.gid, c.egid, c.sgid, (uint32_t)rgid, (uint32_t)egid, cred_is_root(c));
}

int64_t sys_getgroups(uint64_t size, uint64_t list, uint64_t, uint64_t, uint64_t, uint64_t){
    const Cred& c = self();
    int32_t sz = (int32_t)size;
    if(sz < 0){
        return -EINVAL;
    }
    if(sz == 0){
        return c.ngroups;
    }
    if((uint32_t)sz < c.ngroups){
        return -EINVAL;
    }
    uint64_t bytes = c.ngroups * sizeof(uint32_t);
    if(bytes && (!user_access_ok(list, bytes, true) || !copy_to_user(list, c.groups, bytes))){
        return -EFAULT;
    }
    return c.ngroups;
}

int64_t sys_setgroups(uint64_t size, uint64_t list, uint64_t, uint64_t, uint64_t, uint64_t){
    Cred& c = self();
    if(!cred_is_root(c)){
        return -EPERM;
    }
    if(size > CRED_NGROUPS_MAX){
        return -EINVAL;
    }
    uint32_t g[CRED_NGROUPS_MAX];
    uint64_t bytes = size * sizeof(uint32_t);
    if(bytes && (!user_access_ok(list, bytes, false) || !copy_from_user(g, list, bytes))){
        return -EFAULT;
    }
    for(uint64_t i = 0; i < size; i++){
        c.groups[i] = g[i];
    }
    c.ngroups = (uint32_t)size;
    return 0;
}

int64_t sys_umask(uint64_t mask, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    uint32_t old = p->umask;
    p->umask = (uint32_t)mask & 0777;
    return old;
}
