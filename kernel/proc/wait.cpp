#include "proc/process.h"
#include "uapi/syscall_nr.h"

namespace {

Process* sibling_of(ListLink* l){
    return (Process*)((uint8_t*)l - __builtin_offsetof(Process, sibling));
}

}

int64_t process_waitpid(int64_t pid, int* status, int options){
    Process* self = process_current();
    if(!self){
        return -ECHILD;
    }
    Spinlock& lock = process_tree_lock();
    uint64_t flags = lock.lock_irqsave();
    for(;;){
        bool any = false;
        Process* done = nullptr;
        for(ListLink* l = self->children.next; l != &self->children; l = l->next){
            Process* c = sibling_of(l);
            if(pid == -1 || (int64_t)c->pid == pid){
                any = true;
                if(c->state == ProcState::ZOMBIE){
                    done = c;
                    break;
                }
            }
        }
        if(!any){
            lock.unlock_irqrestore(flags);
            return -ECHILD;
        }
        if(done){
            // Copilul iese din lista; structura se elibereaza aici sau, daca task-ul lui inca exista, la task_destroy
            done->sibling.prev->next = done->sibling.next;
            done->sibling.next->prev = done->sibling.prev;
            done->sibling.prev = done->sibling.next = &done->sibling;
            done->parent = nullptr;
            done->reaped = true;
            int64_t rpid = (int64_t)done->pid;
            if(status){
                *status = done->wait_status;
            }
            bool free_now = !done->main;
            lock.unlock_irqrestore(flags);
            if(free_now){
                process_free(done);
            }
            return rpid;
        }
        if(options & WNOHANG){
            lock.unlock_irqrestore(flags);
            return 0;
        }
        self->child_exit.wait(lock);
    }
}
