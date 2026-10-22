#pragma once
#include "types.h"

// Pana la 2048 bytes din slab-urile generale, peste direct din buddy
void kmalloc_init();
void* kmalloc(size_t size);
void* kzalloc(size_t size);
void* krealloc(void* ptr, size_t size);
void kfree(void* ptr);
