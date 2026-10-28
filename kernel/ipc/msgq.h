#pragma once
#include "types.h"

struct MsgQueue;

// Coada de mesaje in kernel: depth mesaje de cel mult size bytes fiecare, toata memoria alocata la creare,
// deci send/recv nu aloca si au durata marginita (o copiere de cel mult size bytes sub un spinlock).
// nullptr pentru parametri invalizi sau fara memorie
MsgQueue* msgq_create(uint32_t size, uint32_t depth);
// Coada nu trebuie sa mai fie folosita de nimeni
void msgq_destroy(MsgQueue* q);

// Copiaza mesajul in coada; cu block asteapta loc, altfel -EAGAIN daca e plina. 0 sau -EINVAL (len > size)
int msgq_send(MsgQueue* q, const void* msg, uint32_t len, bool block);
// Scoate cel mai vechi mesaj (FIFO); cu block asteapta, altfel -EAGAIN daca e goala.
// Intoarce lungimea mesajului, -EINVAL daca nu incape in cap (mesajul ramane in coada)
int msgq_recv(MsgQueue* q, void* buf, uint32_t cap, bool block);
// Mesaje in coada acum
uint32_t msgq_count(const MsgQueue* q);
