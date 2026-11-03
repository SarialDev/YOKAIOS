#pragma once
#include "types.h"

struct netif;

// Porneste lwIP daca cmdline-ul are ip=a.b.c.d (cu optional mask= implicit 255.255.255.0 si gw=) si exista o placa de
// retea in HAL: thread-ul "tcpip" (NORMAL ridicat) creeaza interfata pe prima placa. Nu blocheaza (se poate chema din
// boot). false fara ip= / fara placa / fara memorie.
bool net_init();
// true dupa ce thread-ul tcpip a creat interfata
bool net_ready();
// Interfata creata de net_init (nullptr inainte de net_ready)
struct netif* net_netif();
