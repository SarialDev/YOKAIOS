#pragma once
/* Capabilitati per proces (comune kernel / userland). Un bit = un drept; fiecare proces are doua seturi:
 * permitted (plafonul: ce poate avea vreodata) si effective (ce se verifica acum). Setul nu creste niciodata;
 * se poate doar reduce (capset, exec dupa manifestul pachetului, trecerea de la root la un utilizator). */

#define CAP_SYS_ADMIN   (1ULL << 0)     /* administrare; implica CAP_MOUNT */
#define CAP_NET         (1ULL << 1)     /* porturi < 1024 (bind) */
#define CAP_RAWIO       (1ULL << 2)     /* ioperm: acces direct la porturi de I/O */
#define CAP_RT          (1ULL << 3)     /* clasa RT: sched_setscheduler(SCHED_FIFO) */
#define CAP_MOUNT       (1ULL << 4)     /* mount / umount */
#define CAP_KILL        (1ULL << 5)     /* kill catre procese ale altor utilizatori */
#define CAP_SETUID      (1ULL << 6)     /* setuid/setgid/setreuid/setregid/setgroups spre orice id */
#define CAP_ALL         0x7FULL

struct cap_state {
    unsigned long long effective;
    unsigned long long permitted;
};

/* sched_setscheduler: politici (numerele Linux) */
#define SCHED_OTHER     0
#define SCHED_FIFO      1
struct sched_param {
    int sched_priority;
};

#define CAP_PORT_PRIVILEGED 1024        /* bind pe un port mai mic cere CAP_NET */
