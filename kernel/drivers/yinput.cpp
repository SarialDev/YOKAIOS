// Intrarea compozitorului (T10.2): inputctl(INPUTCTL_GRAB) -> fd cu evenimentele brute de tastatura si mouse.
// Captura (hal/input.h input_grab) muta evenimentele de la terminal la acest fd cat timp e deschis.
#include "drivers/yinput.h"
#include "hal/input.h"
#include "uapi/yinput.h"
#include "uapi/syscall_nr.h"
#include "uapi/net.h"
#include "proc/process.h"
#include "proc/caps.h"
#include "syscall/syscall.h"
#include "sync/spinlock.h"
#include "sync/waitqueue.h"
#include "sync/pollwait.h"
#include "time/clock.h"
#include "lib/string.h"

namespace {

static_assert(sizeof(yk_input_event) == 24, "layout-ul din uapi/yinput.h");
static_assert(YK_KEY_ENTER == KEY_ENTER && YK_KEY_ESC == KEY_ESC && YK_KEY_SPACE == KEY_SPACE &&
              YK_KEY_UP == KEY_UP && YK_KEY_DELETE == KEY_DELETE && YK_KEY_F1 == KEY_F1, "codurile de taste");

const uint32_t RING = 256;                  // putere a lui 2; la plin se pierde cel mai vechi

Spinlock lock("yinput");
WaitQueue readers;
yk_input_event ring[RING];
uint32_t head = 0, tail = 0;
bool open_now = false;

yk_input_event to_user(const InputEvent& ev){
    yk_input_event u;
    memset(&u, 0, sizeof(u));
    u.time_ns = ev.time_ns;
    if(ev.type == InputType::Key){
        u.type = YK_EV_KEY;
        u.keycode = ev.key.keycode;
        u.pressed = ev.key.pressed;
        u.ascii = (uint8_t)ev.key.ascii;
        u.modifiers = ev.key.modifiers;
    } else {
        u.type = YK_EV_MOUSE;
        u.buttons = ev.mouse.buttons;
        u.dx = ev.mouse.dx;
        u.dy = ev.mouse.dy;
        u.dz = ev.mouse.dz;
    }
    return u;
}

// Din IRQ (sau din inputctl INJECT)
void on_event(const InputEvent& ev){
    yk_input_event u = to_user(ev);
    uint64_t flags = lock.lock_irqsave();
    if(tail - head == RING){
        head++;
    }
    ring[tail++ & (RING - 1)] = u;
    lock.unlock_irqrestore(flags);
    readers.wake_all();
    poll_notify();
}

int64_t dev_read(File*, void* buf, uint64_t len, bool nonblock){
    if(len < sizeof(yk_input_event)){
        return -EINVAL;
    }
    uint64_t flags = lock.lock_irqsave();
    while(head == tail){
        if(nonblock){
            lock.unlock_irqrestore(flags);
            return -EAGAIN;
        }
        if(!readers.wait_intr(lock) && head == tail){
            lock.unlock_irqrestore(flags);
            return -EINTR;
        }
    }
    uint64_t n = 0;
    yk_input_event* o = (yk_input_event*)buf;
    while(head != tail && (n + 1) * sizeof(yk_input_event) <= len){
        o[n++] = ring[head++ & (RING - 1)];
    }
    lock.unlock_irqrestore(flags);
    return (int64_t)(n * sizeof(yk_input_event));
}

int64_t dev_write(File*, const void*, uint64_t, bool){
    return -EBADF;
}

void dev_close(File*){
    input_grab(nullptr);
    uint64_t flags = lock.lock_irqsave();
    head = tail = 0;
    open_now = false;
    lock.unlock_irqrestore(flags);
}

uint32_t dev_poll(File*){
    uint64_t flags = lock.lock_irqsave();
    bool any = head != tail;
    lock.unlock_irqrestore(flags);
    return any ? POLLIN : 0;
}

const FileOps yinput_ops = {dev_read, dev_write, dev_close, nullptr, nullptr, dev_poll};

int64_t do_grab(){
    Process* p = process_current();
    if(!p){
        return -EINVAL;
    }
    {
        uint64_t flags = lock.lock_irqsave();
        if(open_now){
            lock.unlock_irqrestore(flags);
            return -EBUSY;
        }
        open_now = true;
        head = tail = 0;
        lock.unlock_irqrestore(flags);
    }
    if(!input_grab(on_event)){
        open_now = false;                       // altcineva din kernel are captura
        return -EBUSY;
    }
    File* f = file_alloc(&yinput_ops, nullptr);
    if(!f){
        input_grab(nullptr);
        open_now = false;
        return -ENOMEM;
    }
    f->flags = O_RDONLY;
    int fd = fd_install(p->fds, f);
    if(fd < 0){
        file_put(f);
    }
    return fd;
}

int64_t do_inject(uint64_t arg){
    if(!capable(CAP_SYS_ADMIN)){
        return -EPERM;
    }
    yk_input_event u;
    if(!copy_from_user(&u, arg, sizeof(u))){
        return -EFAULT;
    }
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.time_ns = monotonic_ns();
    if(u.type == YK_EV_KEY){
        ev.type = InputType::Key;
        ev.key.keycode = u.keycode;
        ev.key.ascii = (char)u.ascii;
        ev.key.pressed = u.pressed != 0;
        ev.key.modifiers = u.modifiers;
    } else if(u.type == YK_EV_MOUSE){
        ev.type = InputType::Mouse;
        ev.mouse.dx = u.dx;
        ev.mouse.dy = u.dy;
        ev.mouse.dz = u.dz;
        ev.mouse.buttons = u.buttons;
    } else {
        return -EINVAL;
    }
    input_push(ev);
    return 0;
}

}

bool yinput_grabbed(){
    return open_now;
}

int64_t sys_inputctl(uint64_t op, uint64_t arg, uint64_t, uint64_t, uint64_t, uint64_t){
    switch(op){
    case INPUTCTL_GRAB:
        return do_grab();
    case INPUTCTL_INJECT:
        return do_inject(arg);
    default:
        return -EINVAL;
    }
}
