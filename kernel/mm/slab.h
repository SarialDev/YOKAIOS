#pragma once
#include "types.h"

struct KmemCache;

// Cache de obiecte de aceeasi marime; fiecare slab e o pagina cu header la inceput
KmemCache* kmem_cache_create(const char* name, size_t size, size_t align);
void* kmem_cache_alloc(KmemCache* cache);
void kmem_cache_free(KmemCache* cache, void* obj);
size_t kmem_cache_object_size(const KmemCache* cache);

// Cache-ul din care vine obj (dupa headerul paginii), nullptr daca nu e obiect de slab
KmemCache* slab_cache_of(const void* obj);
