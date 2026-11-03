#pragma once

// DHCP + DNS + statistici (T9.4): fara ip= (cmdline "test=dhcp net=dhcp dnshost=<port>"), asteapta adresa 10.0.2.15 de la
// serverul DHCP al slirp, /run/resolv.conf scris de netcfg, apoi ruleaza /bin/dnstest (vezi tests/dhcp.mk)
void test_dhcp_run();
