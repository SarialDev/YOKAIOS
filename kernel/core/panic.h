#pragma once
#include "interrupt.h"

[[noreturn]] void panic(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void panic_frame(InterruptFrame* f, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Nume scurt pentru exceptiile 0-31, ex. "#DE Divide Error"
const char* exception_name(uint64_t vec);

// Canary pentru -fstack-protector-strong: initializat din boot.S inainte de kernel_main
extern "C" uint64_t __stack_chk_guard;
extern "C" void stack_guard_init();
extern "C" [[noreturn]] void __stack_chk_fail();
// "RDRAND" sau "TSC", sursa din care a venit canary-ul
const char* stack_guard_source();
