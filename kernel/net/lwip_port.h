#pragma once
#include "types.h"

struct netif;

// Porneste lwIP daca exista o placa de retea in HAL. Adresa: ip=a.b.c.d (optional mask= implicit 255.255.255.0, gw=, dns=)
// sau DHCP cand ip= lipseste / e ip=dhcp (net=off opreste reteaua; in teste doar cu ip= sau net=dhcp): thread-ul "tcpip" (NORMAL ridicat) creeaza interfata pe prima placa. Nu blocheaza (se poate chema din
// boot). false fara ip= / fara placa / fara memorie.
bool net_init();
// true dupa ce thread-ul tcpip a creat interfata
bool net_ready();
// Placa pe care ruleaza lwIP (nullptr inainte de net_init)
class NetDevice;
NetDevice* net_device();
// Interfata creata de net_init (nullptr inainte de net_ready)
struct netif* net_netif();
