# Utilitare de retea (T9.5): test=netutils porneste /bin/nettest (user/test/nettest.c) cu ip= static, nethttp=<port> si
# netecho=<port>. Faza A (ping, ifconfig, netstat, utilizare) nu are nevoie de gazda; apoi oaspetele porneste `nc -l 7` si scrie
# "astept host": tests/netutils_host.py (serverele HTTP si echo ale gazdei, ajunse ca 10.0.2.2:<port>) se conecteaza prin
# hostfwd la nc -l, iar oaspetele face wget si nc spre serverele gazdei. ISO-ul se face la fiecare rulare (porturile in cmdline).
# Inclus din Makefile (include tests/netutils.mk); foloseste LWIP_ARGS si FREE_PORT din tests/lwip.mk.
test-netutils: $(KERNEL) $(INITRD) boot/grub-test.cfg
	HP=$$($(FREE_PORT)); WP=$$($(FREE_PORT)); EP=$$($(FREE_PORT)); \
	while [ "$$WP" = "$$HP" ]; do WP=$$($(FREE_PORT)); done; \
	while [ "$$EP" = "$$HP" ] || [ "$$EP" = "$$WP" ]; do EP=$$($(FREE_PORT)); done; \
	mkdir -p $(BUILD)/iso-netutils/boot/grub; \
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-netutils/boot/; \
	sed "s/test=[^ ]*/test=netutils $(LWIP_ARGS) nethttp=$$WP netecho=$$EP/" boot/grub-test.cfg > $(BUILD)/iso-netutils/boot/grub/grub.cfg; \
	grub-mkrescue -o $(BUILD)/yokai-netutils.iso $(BUILD)/iso-netutils 2>/dev/null; \
	NIC="-nic user,model=$(NICMODEL),hostfwd=tcp:127.0.0.1:$$HP-:7" \
	HOST_CMD="python3 tests/netutils_host.py $$HP $$WP $$EP" \
	EXPECT_FILE=tests/expect-netutils.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $(BUILD)/yokai-netutils.iso $(BUILD)/netutils.log
