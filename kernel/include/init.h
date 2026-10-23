#pragma once

// Cod folosit doar la boot: ajunge in sectiunea .init.text, ale carei pagini
// se elibereaza dupa init (fault_protect_kernel). Nu se apeleaza dupa aceea
#define __init __attribute__((section(".init.text"), noinline))

extern "C" char _init_start[];
extern "C" char _init_end[];
