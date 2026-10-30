#pragma once
#include "types.h"
struct Task;
typedef void (*TaskFn)(void*);
Task* task_create_kernel(const char* name, TaskFn fn, void* arg, uint8_t prio = 0);
