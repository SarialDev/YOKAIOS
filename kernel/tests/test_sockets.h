#pragma once

// Ruleaza /bin/socktest (socket-uri BSD din userland: TCP/UDP, poll/select, EINTR; server echo spre gazda prin hostfwd si
// client spre gazda prin 10.0.2.2); cmdline "test=sockets ip=... sockhost=<port>" (vezi tests/sockets.mk)
void test_sockets_run();
