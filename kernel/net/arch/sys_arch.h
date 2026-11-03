#pragma once
#include "lwip/opt.h"
// Tipurile sys_arch.cpp (semafoare / mailbox-uri cu asteptare cu termen, mutex-ul si thread-urile de kernel din T4)

#define SYS_MBOX_NULL   NULL
#define SYS_SEM_NULL    NULL

struct YkSem;
struct YkMutex;
struct YkMbox;
struct Task;

typedef struct YkSem*   sys_sem_t;
typedef struct YkMutex* sys_mutex_t;
typedef struct YkMbox*  sys_mbox_t;
typedef struct Task*    sys_thread_t;
typedef unsigned long   sys_prot_t;

#define sys_sem_valid(s)            (((s) != NULL) && (*(s) != NULL))
#define sys_sem_set_invalid(s)      do { if((s) != NULL) { *(s) = NULL; } } while(0)
#define sys_mutex_valid(m)          (((m) != NULL) && (*(m) != NULL))
#define sys_mutex_set_invalid(m)    do { if((m) != NULL) { *(m) = NULL; } } while(0)
#define sys_mbox_valid(m)           (((m) != NULL) && (*(m) != NULL))
#define sys_mbox_set_invalid(m)     do { if((m) != NULL) { *(m) = NULL; } } while(0)
