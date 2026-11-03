#pragma once
#include "types.h"

struct netif;

// Configuratia IPv4 a interfetei (T9.4): adresa statica (ip=) sau luata prin DHCP, plus serverele DNS. Adresele sunt in ordine
// de retea (ca ip4_addr_t::addr); 0 = nesetat.
struct NetCfgInfo {
    bool dhcp;              // adresa vine din DHCP (altfel ip= din cmdline)
    bool bound;             // interfata are adresa
    bool resolv_written;    // /run/resolv.conf scris cu serverele curente
    uint32_t addr, mask, gw;
    uint32_t dns[2];
};

// Se cheama o singura data din thread-ul tcpip dupa netif_add: legam callback-ul de stare si, cu dhcp, pornim clientul DHCP
// (lwIP, cu verificare ARP a adresei). dns = server DNS din cmdline (dns=), folosit doar fara DHCP; 0 = niciunul.
void netcfg_attach(struct netif* nif, bool dhcp, uint32_t dns);
// Instantaneu al configuratiei curente
void netcfg_info(NetCfgInfo& out);
// true dupa ce interfata are adresa (statica sau DHCP)
bool netcfg_bound();
// Setare statica a interfetei (ioctl SIOCSIF* / SIOCADDRT, T9.5): doar campurile cu set_* se schimba; opreste DHCP (adresa
// devine statica). Ruleaza in thread-ul tcpip si blocheaza pana termina, deci doar din task-uri. 0 sau -errno
int netcfg_set(bool set_addr, uint32_t addr, bool set_mask, uint32_t mask, bool set_gw, uint32_t gw);
// Scrie serverele DNS in /run/resolv.conf (/etc/resolv.conf e un symlink spre el). Cheama task-ul "netcfg"; 0 sau -errno
int netcfg_write_resolv();
