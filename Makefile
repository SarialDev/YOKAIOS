CC = gcc
CXX = g++
AS = as
LD = ld

BUILD = build

ASFLAGS = --32
CFLAGS = -m32 -nostdlib -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs
CXXFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-rtti -nostdlib -fno-stack-protector -fno-pie -no-pie
LDFLAGS = -m elf_i386 -T boot/linker.ld -nostdlib -e _start

.PHONY: all clean run

all: $(BUILD)/kernel.bin

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.s | $(BUILD)
	$(AS) $(ASFLAGS) boot/boot.s -o $(BUILD)/boot.o

$(BUILD)/kernel.o: kernel/kernel.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c kernel/kernel.cpp -o $(BUILD)/kernel.o

$(BUILD)/kernel.bin: $(BUILD)/boot.o $(BUILD)/kernel.o boot/linker.ld
	$(CXX) -m32 -nostdlib -no-pie -Wl,--build-id=none -Wl,-T,boot/linker.ld -Wl,-e,_start -o $(BUILD)/kernel.bin $(BUILD)/boot.o $(BUILD)/kernel.o

run: $(BUILD)/kernel.bin
	qemu-system-i386 -kernel $(BUILD)/kernel.bin

clean:
	rm -rf $(BUILD)
