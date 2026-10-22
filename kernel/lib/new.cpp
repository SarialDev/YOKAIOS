#include "mm/kmalloc.h"
#include "core/panic.h"

// Fara exceptii: lipsa de memorie la new e fatala (compilatorul presupune ca new nu intoarce nullptr)
void* operator new(size_t size){
    void* p = kmalloc(size);
    if(!p){
        panic("operator new: memorie insuficienta (%lu bytes)", size);
    }
    return p;
}

void* operator new[](size_t size){
    return operator new(size);
}

void operator delete(void* p) noexcept{
    kfree(p);
}

void operator delete[](void* p) noexcept{
    kfree(p);
}

void operator delete(void* p, size_t) noexcept{
    kfree(p);
}

void operator delete[](void* p, size_t) noexcept{
    kfree(p);
}
