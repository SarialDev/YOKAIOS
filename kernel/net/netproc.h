#pragma once
#include "types.h"

// /proc/net/tcp si /proc/net/udp (T9.5): tabelele de socket-uri lwIP in formatul Linux (adrese hex in ordinea de retea citita ca
// u32 little-endian, porturi hex). kind: 0 = tcp, 1 = udp. Scrie cel mult cap octeti in buf si intoarce lungimea. Blocheaza
// scurt (citeste listele din thread-ul tcpip), deci doar din task-uri
uint64_t netproc_sockets(int kind, char* buf, uint64_t cap);
