#pragma once

// lwIP (T9.2), cmdline "test=lwip ip=10.0.2.15 gw=10.0.2.2 mask=255.255.255.0":
//  - implicit (make test-lwip, QEMU user-net): interfata sus cu adresa din cmdline, ping (raw socket ICMP) spre gateway-ul
//    slirp, apoi servere echo TCP si UDP pe portul 7, la care se conecteaza gazda prin hostfwd (tests/lwip_host.py):
//    256 KiB TCP verificate in ambele sensuri + 3 datagrame UDP;
//  - "lwipmode=host-ping" (make test-lwip-ping, netdev socket): un peer pe gazda (tests/lwip_peer.py) face ARP si
//    3 echo ICMP spre sistem, iar kernelul cere sa fi vazut cele 3 cereri (raspunsul il da lwIP).
void test_lwip_run();
