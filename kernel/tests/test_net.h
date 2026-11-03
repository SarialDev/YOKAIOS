#pragma once

// Test de integrare a retelei (T9.T): cmdline "test=net net=dhcp nethttp=<port>"; asteapta adresa DHCP (10.0.2.15), apoi
// ruleaza /bin/nettest (user/bin/nettest.c) cu tests/net_server.py pe gazda (vezi tests/net.mk)
void test_net_run();
