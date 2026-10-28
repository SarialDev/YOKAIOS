#pragma once
#include "types.h"

// Placement new: construieste un obiect in memorie deja alocata (ex. kzalloc, care poate intoarce nullptr)
inline void* operator new(size_t, void* p) noexcept{
    return p;
}
