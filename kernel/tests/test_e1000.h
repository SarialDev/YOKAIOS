#pragma once

// e1000 pe QEMU (-nic user,model=e1000, make test-e1000): MAC, link, ARP who-has 10.0.2.2 -> raspunsul slirp apare in
// statistici si la recv(), apoi prin handler; inele TX/RX parcurse de mai multe ori (700 de cereri). cmdline "test=e1000"
void test_e1000_run();
