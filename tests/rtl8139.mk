# RTL8139 (T9.6): test=rtl8139 = aceleasi verificari ca test=e1000 (ARP catre slirp, statistici, handler, 700 de cereri
# prin inelul RX / cele 4 descriptori TX), cu placa `-nic user,model=rtl8139`. Celelalte teste de retea ruleaza cu aceasta
# placa prin NICMODEL: `make test-lwip NICMODEL=rtl8139` (la fel test-sockets, test-dhcp, test-netutils, test-lwip-ping).
# Inclus din Makefile (include tests/rtl8139.mk).
NICMODEL ?= e1000

$(BUILD)/yokai-rtl8139.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-rtl8139/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-rtl8139/boot/
	sed 's/test=[^ ]*/test=rtl8139/' boot/grub-test.cfg > $(BUILD)/iso-rtl8139/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-rtl8139 2>/dev/null

test-rtl8139: $(BUILD)/yokai-rtl8139.iso
	NIC="-nic user,model=rtl8139" EXPECT_FILE=tests/expect-rtl8139.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/rtl8139.log
