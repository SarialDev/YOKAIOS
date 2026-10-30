#pragma once
#include <stdlib.h>
#include "types.h"
inline void* kmalloc(size_t n){ return malloc(n); }
inline void* kzalloc(size_t n){ return calloc(1, n); }
inline void* krealloc(void* p, size_t n){ return realloc(p, n); }
inline void kfree(void* p){ free(p); }
