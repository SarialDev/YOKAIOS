#include "hal/input.h"
#include "sync/spinlock.h"

namespace {

static_assert((INPUT_RING_SIZE & (INPUT_RING_SIZE - 1)) == 0, "INPUT_RING_SIZE trebuie sa fie putere a lui 2");

Spinlock ring_lock("input");
InputEvent ring[INPUT_RING_SIZE];
uint32_t head = 0;      // urmatorul de citit
uint32_t tail = 0;      // urmatorul de scris
uint64_t dropped = 0;
InputListener listener = nullptr;
InputListener grabber = nullptr;     // input_grab: are prioritate fata de listener

}

void input_push(const InputEvent& ev){
    InputListener cb;
    {
        SpinlockGuard lock(ring_lock);
        if(tail - head == INPUT_RING_SIZE){
            head++;
            dropped++;
        }
        ring[tail++ & (INPUT_RING_SIZE - 1)] = ev;
        cb = grabber ? grabber : listener;
    }
    if(cb){
        cb(ev);
    }
}

bool input_pop(InputEvent& out){
    SpinlockGuard lock(ring_lock);
    if(head == tail){
        return false;
    }
    out = ring[head++ & (INPUT_RING_SIZE - 1)];
    return true;
}

uint64_t input_dropped(){
    SpinlockGuard lock(ring_lock);
    return dropped;
}

InputListener input_subscribe(InputListener cb){
    SpinlockGuard lock(ring_lock);
    InputListener old = listener;
    listener = cb;
    return old;
}

bool input_grab(InputListener cb){
    SpinlockGuard lock(ring_lock);
    if(cb && grabber){
        return false;
    }
    grabber = cb;
    return true;
}
