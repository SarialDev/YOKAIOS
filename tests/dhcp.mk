# DHCP + DNS + statistici (T9.4): test=dhcp porneste fara ip= (net=dhcp), asteapta 10.0.2.15 de la serverul DHCP al slirp si
# /run/resolv.conf scris de netcfg, apoi /bin/dnstest (user/test/dnstest.c): /proc/net/dev, /etc/hosts, gethostbyname/getaddrinfo
# spre tests/dns_host.py (server DNS pe gazda, ajuns ca 10.0.2.2#<port> din dnshost=). ISO-ul se face la fiecare rulare (portul
# in cmdline). Inclus din Makefile (include tests/dhcp.mk); foloseste FREE_PORT din tests/lwip.mk.
test-dhcp: $(KERNEL) $(INITRD) boot/grub-test.cfg
	DP=$$($(FREE_PORT)); \
	mkdir -p $(BUILD)/iso-dhcp/boot/grub; \
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-dhcp/boot/; \
	sed "s/test=[^ ]*/test=dhcp net=dhcp dnshost=$$DP/" boot/grub-test.cfg > $(BUILD)/iso-dhcp/boot/grub/grub.cfg; \
	grub-mkrescue -o $(BUILD)/yokai-dhcp.iso $(BUILD)/iso-dhcp 2>/dev/null; \
	python3 tests/dns_host.py $$DP > $(BUILD)/dns_host.log 2>&1 & DNSPID=$$!; \
	EXPECT_FILE=tests/expect-dhcp.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $(BUILD)/yokai-dhcp.iso $(BUILD)/dhcp.log; RC=$$?; \
	kill $$DNSPID 2>/dev/null; exit $$RC
