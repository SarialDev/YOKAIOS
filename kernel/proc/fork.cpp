#include "proc/process.h"
#include "fs/vfs/vfs.h"
#include "uapi/syscall_nr.h"
#include "syscall/syscall.h"
#include "sched/sched.h"
#include "mm/kmalloc.h"
#include "lib/string.h"

namespace {

// Primul cod al copilului: revine in ring 3 cu registrele parintelui din momentul syscall-ului, rax = 0
void child_entry(void* arg){
    InterruptFrame f = *(InterruptFrame*)arg;
    kfree(arg);
    f.rax = 0;
    user_return(&f);
}

}

int64_t process_fork(const InterruptFrame& parent_frame){
    Task* self = task_current();
    Process* parent = self->proc;
    if(!parent){
        return -EINVAL;
    }
    InterruptFrame* start = (InterruptFrame*)kmalloc(sizeof(InterruptFrame));
    AddressSpace* as = (AddressSpace*)kmalloc(sizeof(AddressSpace));
    if(!start || !as){
        kfree(start);
        kfree(as);
        return -ENOMEM;
    }
    *start = parent_frame;
    *as = as_create();
    if(!as->pml4_phys){
        kfree(start);
        kfree(as);
        return -ENOMEM;
    }
    Task* t = as_copy_user(*as, *self->as) ? task_create_kernel(self->name, child_entry, start, self->prio) : nullptr;
    if(!t){
        as_destroy(*as);
        kfree(as);
        kfree(start);
        return -ENOMEM;
    }
    t->as = as;
    Process* child = process_create(t, parent);
    if(!child){
        task_destroy(t);                        // elibereaza si spatiul de adrese
        kfree(start);
        return -ENOMEM;
    }
    process_set_name(child, parent->name);
    {
        SpinlockGuard lock(process_tree_lock());
        memcpy(child->cmdline, parent->cmdline, parent->cmdline_len);
        child->cmdline_len = parent->cmdline_len;
    }
    fd_copy(child->fds, parent->fds);
    vfs_path_copy(child->cwd, parent->cwd);
    int64_t pid = (int64_t)child->pid;
    sched_add(t);
    return pid;
}
