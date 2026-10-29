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

# Programe user statice, puse in ISO ca module Multiboot2
USER_SRCS := $(wildcard user/test/*.S)
USER_ELFS := $(patsubst user/test/%.S,$(BUILD)/user/%.elf,$(USER_SRCS))
USER_ASFLAGS = -m64 -ffreestanding -nostdlib -Ikernel/include
USER_LDFLAGS = -m elf_x86_64 -static -nostdlib -e _start -Ttext-segment=0x400000 \
               -z max-page-size=0x1000 -z separate-code -z noexecstack --build-id=none

# test-rt: jitter strict (<= 1 ms) cu KVM daca /dev/kvm e accesibil, altfel -icount (timp virtual, fara pauzele gazdei)
RT_ACCEL := $(shell [ -w /dev/kvm ] && echo "-enable-kvm -cpu host" || echo "-icount shift=auto")
# -icount ruleaza CPU-urile virtuale pe rand intr-un singur thread: cu mai multe, un CPU asteapta randul zeci de ms
RT_SMP := $(shell [ -w /dev/kvm ] && echo "$(SMP)" || echo 1)

.PHONY: all iso run test test-smash test-relock test-rt test-pci-q35 test-mouse test-nofb debug clean

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

$(ISO): $(KERNEL) $(USER_ELFS) boot/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(USER_ELFS) $(BUILD)/iso/boot/
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso 2>/dev/null

run: $(ISO)
	$(QEMU) $(QEMU_FLAGS)

$(TEST_ISO): $(KERNEL) $(USER_ELFS) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-test/boot/grub
	cp $(KERNEL) $(USER_ELFS) $(BUILD)/iso-test/boot/
	cp boot/grub-test.cfg $(BUILD)/iso-test/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-test 2>/dev/null

test: $(TEST_ISO)
	SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $(TEST_ISO) $(BUILD)/test.log

# Acelasi kernel cu cmdline "test=<nume>": testul trebuie sa se termine intr-un panic anume
$(BUILD)/panic-%.iso: $(KERNEL) $(USER_ELFS) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-$*/boot/grub
	cp $(KERNEL) $(USER_ELFS) $(BUILD)/iso-$*/boot/
	sed 's/test=[^ ]*/test=$*/' boot/grub-test.cfg > $(BUILD)/iso-$*/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-$* 2>/dev/null

test-smash: $(BUILD)/panic-smash.iso
	EXPECT_PANIC="stack smashing" SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/smash.log

test-relock: $(BUILD)/panic-relock.iso
	EXPECT_PANIC="re-lock" SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/relock.log

# PCI pe masina q35: config space prin ECAM (MCFG), restul testelor ramana pe i440fx
$(BUILD)/panic-pci.iso: $(KERNEL) $(USER_ELFS) boot/grub-test.cfg

test-pci-q35: $(BUILD)/panic-pci.iso
	EXPECT_FILE=tests/expect-pci-q35.txt SMP=$(SMP) QEMU_EXTRA="-machine q35 $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/pci-q35.log

# Boot normal cu "debug=mouse"; miscarea mouse-ului injectata prin monitorul QEMU trebuie sa apara pe serial
$(BUILD)/yokai-mouse.iso: $(KERNEL) $(USER_ELFS) boot/grub.cfg
	@mkdir -p $(BUILD)/iso-mouse/boot/grub
	cp $(KERNEL) $(USER_ELFS) $(BUILD)/iso-mouse/boot/
	sed 's|/boot/kernel.bin|/boot/kernel.bin debug=mouse|' boot/grub.cfg > $(BUILD)/iso-mouse/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-mouse 2>/dev/null

test-mouse: $(BUILD)/yokai-mouse.iso
	QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_mouse.sh $< $(BUILD)/mouse.log

# Acelasi test cu cmdline "nofb": consola VGA text in loc de framebuffer
$(BUILD)/yokai-nofb.iso: $(KERNEL) $(USER_ELFS) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-nofb/boot/grub
	cp $(KERNEL) $(USER_ELFS) $(BUILD)/iso-nofb/boot/
	sed 's|/boot/kernel.bin|/boot/kernel.bin nofb|' boot/grub-test.cfg > $(BUILD)/iso-nofb/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-nofb 2>/dev/null

test-nofb: $(BUILD)/yokai-nofb.iso
	FB=0 SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/nofb.log

$(BUILD)/yokai-rt.iso: $(KERNEL) $(USER_ELFS) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-rt/boot/grub
	cp $(KERNEL) $(USER_ELFS) $(BUILD)/iso-rt/boot/
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
