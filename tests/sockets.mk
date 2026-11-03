# Socket-uri BSD (T9.3): test=sockets porneste /bin/socktest (user/test/socktest.c) cu ip= si sockhost=<port>.
# test-sockets: faza A (API, poll/select, EINTR) fara gazda; faza B: server echo TCP (2 clienti simultani) + UDP pe portul 7,
# la care tests/sockets_host.py se conecteaza prin hostfwd; faza C: oaspetele se conecteaza la serverul pornit de acelasi script
# pe gazda (10.0.2.2:<port>). ISO-ul se face la fiecare rulare, ca portul gazdei sa intre in cmdline.
# Inclus din Makefile (include tests/sockets.mk); foloseste LWIP_ARGS si FREE_PORT din tests/lwip.mk.
test-sockets: $(KERNEL) $(INITRD) boot/grub-test.cfg
	HP=$$($(FREE_PORT)); SP=$$($(FREE_PORT)); \
	while [ "$$SP" = "$$HP" ]; do SP=$$($(FREE_PORT)); done; \
	mkdir -p $(BUILD)/iso-sockets/boot/grub; \
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-sockets/boot/; \
	sed "s/test=[^ ]*/test=sockets $(LWIP_ARGS) sockhost=$$SP/" boot/grub-test.cfg > $(BUILD)/iso-sockets/boot/grub/grub.cfg; \
	grub-mkrescue -o $(BUILD)/yokai-sockets.iso $(BUILD)/iso-sockets 2>/dev/null; \
	NIC="-nic user,model=e1000,hostfwd=tcp:127.0.0.1:$$HP-:7,hostfwd=udp:127.0.0.1:$$HP-:7" \
	HOST_CMD="python3 tests/sockets_host.py $$HP $$SP" \
	EXPECT_FILE=tests/expect-sockets.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $(BUILD)/yokai-sockets.iso $(BUILD)/sockets.log
