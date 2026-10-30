ARCH ?= x86_64

CC  = gcc
CXX = g++
LD  = ld

BUILD = build
ISO   = $(BUILD)/yokai.iso
TEST_ISO = $(BUILD)/yokai-test.iso
KERNEL = $(BUILD)/kernel.bin

QEMU = qemu-system-x86_64
SMP ?= 4
QEMU_FLAGS = -cdrom $(ISO) -m 512M -smp $(SMP) -serial stdio $(QEMU_EXTRA)
QEMU_EXTRA ?=

# -march fix: altfel compilatorul poate tinti un CPU mai nou (BMI2 etc.) decat cel emulat de QEMU (#UD)
COMMON = -march=x86-64 -mtune=generic -ffreestanding -nostdlib -fstack-protector-strong -mstack-protector-guard=global -fno-pic \
         -mno-sse -mno-mmx -mno-80387 -O2 -Wall -Wextra -Werror \
         -Ikernel/include -Ikernel -MMD -MP -fno-asynchronous-unwind-tables \
         -fno-omit-frame-pointer

ifeq ($(ARCH),x86_64)
ARCHFLAGS = -m64 -mno-red-zone -mcmodel=kernel
LDARCH    = -m elf_x86_64
else
ARCHFLAGS = -m32
LDARCH    = -m elf_i386
endif

CFLAGS   = $(ARCHFLAGS) $(COMMON)
CXXFLAGS = $(ARCHFLAGS) $(COMMON) -fno-exceptions -fno-rtti
ASFLAGS  = $(ARCHFLAGS) $(COMMON)
LDFLAGS  = $(LDARCH) -T boot/linker.ld -nostdlib -z noexecstack -z max-page-size=0x1000 --build-id=none

SRCS := $(shell find boot kernel -name '*.S' -o -name '*.c' -o -name '*.cpp')
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

# Programe user statice, puse in /bin din initrd
USER_SRCS := $(wildcard user/test/*.S)
USER_ELFS := $(patsubst user/test/%.S,$(BUILD)/user/%.elf,$(USER_SRCS))
USER_ASFLAGS = -m64 -ffreestanding -nostdlib -Ikernel/include
USER_LDFLAGS = -m elf_x86_64 -static -nostdlib -e _start -Ttext-segment=0x400000 \
               -z max-page-size=0x1000 -z separate-code -z noexecstack --build-id=none

# initrd.tar (ustar) = rootfs/ + layout-ul din blueprint + programele user in /bin; GRUB il incarca ca modul "initrd"
# si kernelul il monteaza pe "/" cu tarfs (read-only)
ROOTFS     = $(BUILD)/rootfs
INITRD     = $(BUILD)/initrd.tar
ROOT_DIRS  = bin boot dev etc home lib mnt media opt proc root run sbin sys tmp usr var
ROOTFS_SRC := $(shell find rootfs -type f 2>/dev/null)

# test-rt: jitter strict (<= 1 ms) cu KVM daca /dev/kvm e accesibil, altfel -icount (timp virtual, fara pauzele gazdei)
RT_ACCEL := $(shell [ -w /dev/kvm ] && echo "-enable-kvm -cpu host" || echo "-icount shift=auto")
# -icount ruleaza CPU-urile virtuale pe rand intr-un singur thread: cu mai multe, un CPU asteapta randul zeci de ms
RT_SMP := $(shell [ -w /dev/kvm ] && echo "$(SMP)" || echo 1)

.PHONY: all iso run test test-smash test-relock test-rt test-pci-q35 test-ahci test-ahci-gpt test-ahci-mbr test-fat32 test-ext4 test-ext4-refuse test-ext4-recover test-ext4w test-ext4-host run-ext4 test-mouse test-usb test-input-ps2 test-input-usb test-nofb debug clean

all: $(ISO)

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) boot/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/user/%.o: user/test/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_ASFLAGS) -c $< -o $@

$(BUILD)/user/%.elf: $(BUILD)/user/%.o
	$(LD) $(USER_LDFLAGS) -o $@ $<

$(INITRD): $(USER_ELFS) $(ROOTFS_SRC) Makefile
	rm -rf $(ROOTFS)
	mkdir -p $(addprefix $(ROOTFS)/,$(ROOT_DIRS))
	cp -r rootfs/. $(ROOTFS)/
	for f in $(USER_ELFS); do cp $$f $(ROOTFS)/bin/$$(basename $$f .elf); done
	ln -s ../proc/mounts $(ROOTFS)/etc/mtab
	find $(ROOTFS) -type d -exec chmod 755 {} +
	find $(ROOTFS) -type f -exec chmod 644 {} +
	chmod 755 $(ROOTFS)/bin/*
	chmod 700 $(ROOTFS)/root
	chmod 1777 $(ROOTFS)/tmp
	tar --format=ustar --owner=0 --group=0 --numeric-owner --sort=name --mtime='2026-01-01 00:00Z' \
	    -cf $@ -C $(ROOTFS) .

$(ISO): $(KERNEL) $(INITRD) boot/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso/boot/
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso 2>/dev/null

run: $(ISO)
	$(QEMU) $(QEMU_FLAGS)

$(TEST_ISO): $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-test/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-test/boot/
	cp boot/grub-test.cfg $(BUILD)/iso-test/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-test 2>/dev/null

test: $(TEST_ISO)
	SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $(TEST_ISO) $(BUILD)/test.log

# Acelasi kernel cu cmdline "test=<nume>": testul trebuie sa se termine intr-un panic anume
$(BUILD)/panic-%.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-$*/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-$*/boot/
	sed 's/test=[^ ]*/test=$*/' boot/grub-test.cfg > $(BUILD)/iso-$*/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-$* 2>/dev/null

test-smash: $(BUILD)/panic-smash.iso
	EXPECT_PANIC="stack smashing" SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/smash.log

test-relock: $(BUILD)/panic-relock.iso
	EXPECT_PANIC="re-lock" SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/relock.log

# PCI pe masina q35: config space prin ECAM (MCFG), restul testelor ramana pe i440fx
$(BUILD)/panic-pci.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-pci-q35: $(BUILD)/panic-pci.iso
	EXPECT_FILE=tests/expect-pci-q35.txt SMP=$(SMP) QEMU_EXTRA="-machine q35 $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/pci-q35.log

# Disc SATA real: tests/mkdisk.py face imaginile (GPT si MBR cu partitie extinsa), kernelul ruleaza doar test=block,ahci
$(BUILD)/disk-%.img: tests/mkdisk.py
	@mkdir -p $(BUILD)
	python3 tests/mkdisk.py $* $@

$(BUILD)/yokai-ahci.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-ahci/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-ahci/boot/
	sed 's/test=[^ ]*/test=block,ahci/' boot/grub-test.cfg > $(BUILD)/iso-ahci/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-ahci 2>/dev/null

AHCI_QEMU = -boot order=d -device ahci,id=ahci0 -drive file=$(BUILD)/disk-$(1).img,if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=ahci0.0

test-ahci-gpt: $(BUILD)/yokai-ahci.iso $(BUILD)/disk-gpt.img
	EXPECT_FILE=tests/expect-ahci-gpt.txt SMP=$(SMP) QEMU_EXTRA="$(call AHCI_QEMU,gpt) $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/ahci-gpt.log

test-ahci-mbr: $(BUILD)/yokai-ahci.iso $(BUILD)/disk-mbr.img
	EXPECT_FILE=tests/expect-ahci-mbr.txt SMP=$(SMP) QEMU_EXTRA="$(call AHCI_QEMU,mbr) $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/ahci-mbr.log

test-ahci: test-ahci-gpt test-ahci-mbr

# FAT32: imagine de 64 MiB facuta cu mkfs.fat (clustere de 512 B si de 4 KiB) si populata cu mtools (tests/mkfat32.sh);
# kernelul ruleaza doar test=fat32 si modifica discul, apoi tests/check_fat32.sh il verifica cu fsck.fat si mtools
$(BUILD)/yokai-fat32.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-fat32/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-fat32/boot/
	sed 's/test=[^ ]*/test=fat32/' boot/grub-test.cfg > $(BUILD)/iso-fat32/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-fat32 2>/dev/null

FAT32_QEMU = -boot order=d -device ahci,id=ahci0 -drive file=$(BUILD)/fat32-$(1).img,if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=ahci0.0

test-fat32-%: $(BUILD)/yokai-fat32.iso tests/mkfat32.sh tests/check_fat32.sh
	sh tests/mkfat32.sh $(BUILD)/fat32-$*.img $*
	EXPECT_FILE=tests/expect-fat32.txt SMP=$(SMP) QEMU_EXTRA="$(call FAT32_QEMU,$*) $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/fat32-$*.log
	sh tests/check_fat32.sh $(BUILD)/fat32-$*.img

test-fat32: test-fat32-1 test-fat32-8

# ext4 read-only: tests/mkext4.sh face discul (MBR, sda1 gol, sda2 = ext4 cu mke2fs -d din rootfs + date de test) in
# 3 variante (ext4 / ext2 / linear), kernelul porneste cu root=/dev/sda2, parcurge arborele si tipareste CRC32 pentru
# fiecare fisier; tests/check_ext4.sh il compara cu sursa si ruleaza e2fsck. Necesita e2fsprogs (mke2fs, e2fsck, debugfs)
EXT4_QEMU = -boot order=d -device ahci,id=ahci0 -drive file=$(1),if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=ahci0.0

$(BUILD)/ext4-%.img: tests/mkext4.sh tests/ext4_list.py $(INITRD)
	sh tests/mkext4.sh $@ $* $(ROOTFS)

$(BUILD)/ext4-refuse.img: $(BUILD)/ext4-ext4.img tests/mkext4.sh
	sh tests/mkext4.sh patch $< $@ refuse

$(BUILD)/ext4-recover.img: $(BUILD)/ext4-ext4.img tests/mkext4.sh
	sh tests/mkext4.sh patch $< $@ recover

$(BUILD)/yokai-ext4-%.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-ext4-$*/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-ext4-$*/boot/
	sed 's|test=[^ ]*|root=/dev/sda2 test=ext4 ext4mode=$*|' boot/grub-test.cfg > $(BUILD)/iso-ext4-$*/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-ext4-$* 2>/dev/null

test-ext4-%: $(BUILD)/yokai-ext4-ok.iso $(BUILD)/ext4-%.img tests/check_ext4.sh
	TEST_TIMEOUT=900 EXPECT_FILE=tests/expect-ext4.txt SMP=$(SMP) QEMU_EXTRA="$(call EXT4_QEMU,$(BUILD)/ext4-$*.img) $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/ext4-$*.log
	sh tests/check_ext4.sh $(BUILD)/ext4-$*.img $(BUILD)/ext4-$*.log ok

# Flag INCOMPAT necunoscut: montarea esueaza, radacina ramane initrd; jurnal cu needs_recovery: refuzat fara "noload"
test-ext4-refuse: $(BUILD)/yokai-ext4-refuse.iso $(BUILD)/ext4-refuse.img tests/check_ext4.sh
	TEST_TIMEOUT=900 EXPECT_FILE=tests/expect-ext4-refuse.txt SMP=$(SMP) QEMU_EXTRA="$(call EXT4_QEMU,$(BUILD)/ext4-refuse.img) $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/ext4-refuse.log
	sh tests/check_ext4.sh $(BUILD)/ext4-refuse.img $(BUILD)/ext4-refuse.log refuse

test-ext4-recover: $(BUILD)/yokai-ext4-recover.iso $(BUILD)/ext4-recover.img $(BUILD)/ext4-ext4.img tests/check_ext4.sh
	TEST_TIMEOUT=900 EXPECT_FILE=tests/expect-ext4-recover.txt SMP=$(SMP) QEMU_EXTRA="$(call EXT4_QEMU,$(BUILD)/ext4-recover.img) $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/ext4-recover.log
	sh tests/check_ext4.sh $(BUILD)/ext4-recover.img $(BUILD)/ext4-recover.log recover $(BUILD)/ext4-ext4.img.list

test-ext4: test-ext4-ext4 test-ext4-ext2 test-ext4-linear test-ext4-refuse test-ext4-recover

# ext4 cu scriere si jurnal JBD2 (T6.8): tests/check_ext4w.sh porneste QEMU de mai multe ori pe acelasi disc (sectorul 0
# din sda1 spune fiecarui boot ce sa faca) si opreste masina brusc: la etapele commit-ului (crash1..crash5), cu
# kill -9 in timpul scrierilor (kill), sau pornind de la un jurnal scris de debugfs (replay, replay-csum). Dupa replay:
# e2fsck -fn curat, continutul citit de kernel = cel citit de debugfs, iar replay-ul facut de e2fsprogs pe o copie da
# acelasi arbore. SEED=n repeta un kill anume
EXT4W_CASES = crash1 crash2 crash3 crash4 crash5 clean kill replay replay-csum

$(BUILD)/yokai-ext4w.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-ext4w/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-ext4w/boot/
	sed 's|test=[^ ]*|test=ext4w|' boot/grub-test.cfg > $(BUILD)/iso-ext4w/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-ext4w 2>/dev/null

test-ext4w-%: $(BUILD)/yokai-ext4w.iso $(BUILD)/ext4-ext4.img tests/check_ext4w.sh tests/ext4w_check.py
	SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/check_ext4w.sh $< $(BUILD)/ext4-ext4.img $* $(BUILD)/ext4w-$*

test-ext4w: $(addprefix test-ext4w-,$(EXT4W_CASES))

# Acelasi cod ext4/jbd2 compilat pe gazda cu ASAN/UBSan: model aleator + sute de caderi simulate (tests/host/run.sh)
test-ext4-host: $(BUILD)/ext4-ext4.img
	sh tests/host/run.sh $<

# Boot normal cu radacina ext4 pe sda2 (fara module necesare in afara de fallback)
$(BUILD)/yokai-rootext4.iso: $(KERNEL) $(INITRD) boot/grub.cfg
	@mkdir -p $(BUILD)/iso-rootext4/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-rootext4/boot/
	sed 's|/boot/kernel.bin|/boot/kernel.bin root=/dev/sda2|' boot/grub.cfg > $(BUILD)/iso-rootext4/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-rootext4 2>/dev/null

run-ext4: $(BUILD)/yokai-rootext4.iso $(BUILD)/ext4-ext4.img
	$(QEMU) -cdrom $< -m 512M -smp $(SMP) -serial stdio -boot order=d -device ahci,id=ahci0 -drive file=$(BUILD)/ext4-ext4.img,if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=ahci0.0 $(QEMU_EXTRA)

# Doar test=drivers; "version<ret>" trimis prin monitorul QEMU: o data pe PS/2 (fara xHCI), o data pe usb-kbd (fara 8042)
$(BUILD)/panic-drivers.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-input-ps2: $(BUILD)/panic-drivers.iso
	USB=0 EXPECT_FILE=tests/expect-drivers.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/input-ps2.log

test-input-usb: $(BUILD)/panic-drivers.iso
	EXPECT_FILE=tests/expect-drivers.txt SMP=$(SMP) QEMU_EXTRA="-machine pc,i8042=off $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/input-usb.log

# Boot normal cu "debug=mouse"; miscarea mouse-ului injectata prin monitorul QEMU trebuie sa apara pe serial
$(BUILD)/yokai-mouse.iso: $(KERNEL) $(INITRD) boot/grub.cfg
	@mkdir -p $(BUILD)/iso-mouse/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-mouse/boot/
	sed 's|/boot/kernel.bin|/boot/kernel.bin debug=mouse|' boot/grub.cfg > $(BUILD)/iso-mouse/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-mouse 2>/dev/null

test-mouse: $(BUILD)/yokai-mouse.iso
	QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_mouse.sh $< $(BUILD)/mouse.log

# Boot normal cu "debug=usb" pe o masina fara 8042: tastatura si mouse-ul vin doar prin xHCI
$(BUILD)/yokai-usb.iso: $(KERNEL) $(INITRD) boot/grub.cfg
	@mkdir -p $(BUILD)/iso-usb/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-usb/boot/
	sed 's|/boot/kernel.bin|/boot/kernel.bin debug=usb|' boot/grub.cfg > $(BUILD)/iso-usb/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-usb 2>/dev/null

test-usb: $(BUILD)/yokai-usb.iso
	QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_usb.sh $< $(BUILD)/usb.log

# Acelasi test cu cmdline "nofb": consola VGA text in loc de framebuffer
$(BUILD)/yokai-nofb.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-nofb/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-nofb/boot/
	sed 's|/boot/kernel.bin|/boot/kernel.bin nofb|' boot/grub-test.cfg > $(BUILD)/iso-nofb/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-nofb 2>/dev/null

test-nofb: $(BUILD)/yokai-nofb.iso
	FB=0 SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/nofb.log

$(BUILD)/yokai-rt.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-rt/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-rt/boot/
	sed 's/test=[^ ]*/test=rt rt_strict/' boot/grub-test.cfg > $(BUILD)/iso-rt/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-rt 2>/dev/null

test-rt: $(BUILD)/yokai-rt.iso
	@echo "test-rt cu: $(RT_ACCEL), $(RT_SMP) CPU"
	EXPECT_FILE=tests/expect-rt.txt SMP=$(RT_SMP) QEMU_EXTRA="$(RT_ACCEL) $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/rt.log

debug: $(ISO)
	$(QEMU) $(QEMU_FLAGS) -s -S

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
