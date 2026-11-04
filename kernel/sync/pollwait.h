#pragma once
#include "types.h"
#include "sched/task.h"

// Asteptarea comuna a poll/select pentru fd-urile care nu sunt socket-uri lwIP (pipe, terminal, AF_UNIX, shm,
// /dev input al compozitorului): oricine schimba starea unui astfel de fisier cheama poll_notify(), care creste un
// contor global si trezeste toti cei care asteapta in poll. Asa poll(-1) doarme cu adevarat (fara sondare la 1 ms).

// Contorul curent; se citeste inainte de verificarea fd-urilor
uint64_t poll_seq();
// Starea unui fisier s-a schimbat (orice context, si din IRQ)
void poll_notify();
// Doarme pana cand contorul nu mai e seq, pana la deadline_ns (0 = fara termen) sau pana la un semnal
void poll_wait(uint64_t seq, uint64_t deadline_ns);
// Semnal pentru t (proc/signal.cpp): il trezeste daca asteapta in poll_wait
void poll_interrupt(Task* t);
