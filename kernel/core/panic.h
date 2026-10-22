#pragma once
#include "interrupt.h"

[[noreturn]] void panic(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void panic_frame(InterruptFrame* f, const char* msg);

// Nume scurt pentru exceptiile 0-31, ex. "#DE Divide Error"
const char* exception_name(uint64_t vec);
