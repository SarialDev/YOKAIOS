ARCH ?= x86_64

CC  = gcc
CXX = g++
LD  = ld

BUILD = build
ISO   = $(BUILD)/yokai.iso
KERNEL = $(BUILD)/kernel.bin

QEMU = qemu-system-x86_64
QEMU_FLAGS = -cdrom $(ISO) -m 512M -serial stdio $(QEMU_EXTRA)
QEMU_EXTRA ?=

COMMON = -ffreestanding -nostdlib -fno-stack-protector -fno-pic \
         -mno-sse -mno-mmx -mno-80387 -O2 -Wall -Wextra -Werror \
         -Ikernel/include -MMD -MP -fno-asynchronous-unwind-tables

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

.PHONY: all iso run test debug clean

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

$(ISO): $(KERNEL) boot/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/kernel.bin
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso 2>/dev/null

run: $(ISO)
	$(QEMU) $(QEMU_FLAGS)

# Smoke test until tests/run_qemu.sh exists (T1.T): QEMU must still be running after 10 s.
test: $(ISO)
	@if [ -x tests/run_qemu.sh ]; then tests/run_qemu.sh $(ISO); else \
	timeout 10 $(QEMU) -cdrom $(ISO) -m 512M -display none -serial none -no-reboot $(QEMU_EXTRA); \
	[ $$? -eq 124 ] && echo "[OK] smoke boot" || { echo "FAIL: QEMU exited early"; exit 1; }; fi

debug: $(ISO)
	$(QEMU) $(QEMU_FLAGS) -s -S

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
