#pragma once

// Utilitare de retea (T9.5): porneste /bin/nettest (user/test/nettest.c), care ruleaza ping, ifconfig, netstat, wget si nc;
// cmdline "test=netutils ip=... nethttp=<port> netecho=<port>" (vezi tests/netutils.mk)
void test_netutils_run();
