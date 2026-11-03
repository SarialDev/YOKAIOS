# lwIP (T9.2): kernelul ruleaza doar test=lwip cu ip= din cmdline (adresele implicite ale slirp: 10.0.2.15, gw 10.0.2.2).
# test-lwip: ping spre gateway, apoi servere echo TCP/UDP pe portul 7 al oaspetelui, la care tests/lwip_host.py se
# conecteaza prin hostfwd. test-lwip-ping: placa e legata printr-un netdev socket la tests/lwip_peer.py, care face ARP si
# trimite 3 ping-uri spre sistem (ping de pe gazda spre OS, fara tap / root).
# Inclus din Makefile (include tests/lwip.mk).
LWIP_ARGS = ip=10.0.2.15 gw=10.0.2.2 mask=255.255.255.0
FREE_PORT = python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'

$(BUILD)/yokai-lwip.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-lwip/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-lwip/boot/
	sed 's/test=[^ ]*/test=lwip $(LWIP_ARGS)/' boot/grub-test.cfg > $(BUILD)/iso-lwip/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-lwip 2>/dev/null

$(BUILD)/yokai-lwip-ping.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-lwip-ping/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-lwip-ping/boot/
	sed 's/test=[^ ]*/test=lwip lwipmode=host-ping $(LWIP_ARGS)/' boot/grub-test.cfg > $(BUILD)/iso-lwip-ping/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-lwip-ping 2>/dev/null

test-lwip: $(BUILD)/yokai-lwip.iso
	HP=$$($(FREE_PORT)); \
	NIC="-nic user,model=$(NICMODEL),hostfwd=tcp:127.0.0.1:$$HP-:7,hostfwd=udp:127.0.0.1:$$HP-:7" \
	HOST_CMD="python3 tests/lwip_host.py $$HP" \
	EXPECT_FILE=tests/expect-lwip.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/lwip.log

test-lwip-ping: $(BUILD)/yokai-lwip-ping.iso
	HP=$$($(FREE_PORT)); \
	NIC="-netdev socket,id=ln0,listen=127.0.0.1:$$HP -device $(NICMODEL),netdev=ln0" \
	HOST_CMD="python3 tests/lwip_peer.py $$HP" \
	EXPECT_FILE=tests/expect-lwip-ping.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/lwip-ping.log
