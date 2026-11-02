#pragma once
#include "types.h"

// 64 de biti aleatori pentru ASLR: RDRAND daca CPU-ul il are, altfel TSC amestecat (splitmix64, necriptografic)
uint64_t entropy_u64();
bool entropy_has_rdrand();
