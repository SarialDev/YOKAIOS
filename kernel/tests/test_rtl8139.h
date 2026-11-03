#pragma once

// rtl8139 pe QEMU (-nic user,model=rtl8139, make test-rtl8139): MAC, link, ARP who-has 10.0.2.2 -> raspunsul slirp apare in
// statistici si la recv(), apoi prin handler; inele TX/RX parcurse de mai multe ori (700 de cereri). cmdline "test=rtl8139"
void test_rtl8139_run();
