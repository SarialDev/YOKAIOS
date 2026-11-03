#pragma once
#include "types.h"
#include "hal/net.h"

struct netif;

// Interfata lwIP peste un NetDevice (HAL): cadrele primite din handlerul dispozitivului intra in lwIP prin
// tcpip_input (pbuf din pool, fara blocare), transmisia copiaza lantul de pbuf intr-un cadru si cheama NetDevice::send.
// Se apeleaza din thread-ul tcpip (sau inainte sa porneasca): netif_add nu e thread-safe.
// Adresele sunt IPv4 in ordine de retea (ca ip4_addr_t::addr); addr == 0 lasa interfata fara adresa.
// Intoarce nullptr daca netif_add esueaza.
struct netif* yk_netif_create(NetDevice* dev, uint32_t addr, uint32_t mask, uint32_t gw);

// Contoare ale interfetei (pentru teste / diagnostic)
struct YkNetifStats {
    uint64_t rx_frames;
    uint64_t rx_nomem;      // fara pbuf in pool sau coada tcpip plina
    uint64_t tx_frames;
    uint64_t tx_errors;
};
void yk_netif_stats(YkNetifStats& out);
