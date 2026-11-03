# Teste de integrare a retelei (T9.T): test=net porneste cu net=dhcp (adresa 10.0.2.15 de la serverul DHCP al slirp) si ruleaza
# /bin/nettest (user/bin/nettest.c): ifconfig/resolv.conf, ping 10.0.2.2, wget + SHA-256 de pe serverul HTTP al gazdei si echo
# TCP prin hostfwd la portul 7 al oaspetelui. Gazda ruleaza tests/net_server.py (pornit de run_qemu.sh cand oaspetele tipareste
# "astept host"). `make test-net` ruleaza testul pe ambele placi (NICS), `make test-net-one NICMODEL=rtl8139` pe una singura.
# Inclus din Makefile (include tests/net.mk); foloseste FREE_PORT din tests/lwip.mk. Face parte din `make test`.
NICS = e1000 rtl8139

test-net:
	set -e; for m in $(NICS); do $(MAKE) --no-print-directory test-net-one NICMODEL=$$m; done

test-net-one: $(KERNEL) $(INITRD) boot/grub-test.cfg
	HP=$$($(FREE_PORT)); WP=$$($(FREE_PORT)); \
	while [ "$$WP" = "$$HP" ]; do WP=$$($(FREE_PORT)); done; \
	mkdir -p $(BUILD)/iso-net/boot/grub; \
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-net/boot/; \
	sed "s/test=[^ ]*/test=net net=dhcp nethttp=$$WP/" boot/grub-test.cfg > $(BUILD)/iso-net/boot/grub/grub.cfg; \
	grub-mkrescue -o $(BUILD)/yokai-net.iso $(BUILD)/iso-net 2>/dev/null; \
	echo "== test-net: placa $(NICMODEL)"; \
	NIC="-nic user,model=$(NICMODEL),hostfwd=tcp:127.0.0.1:$$HP-:7" \
	HOST_CMD="python3 tests/net_server.py $$HP $$WP" \
	EXPECT_FILE=tests/expect-net.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $(BUILD)/yokai-net.iso $(BUILD)/net-$(NICMODEL).log
