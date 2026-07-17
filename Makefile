CC = gcc
CXX = g++
AS = as
LD = ld

ASFLAGS = --32
CFLAGS = -m32 -nostdlib -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs
CXXFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-rtti -nostdlib -fno-stack-protector -fno-pie -no-pie
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib -e _start

.PHONY: all clean run

all: kernel.bin

boot.o: boot.s
	$(AS) $(ASFLAGS) boot.s -o boot.o

kernel.o: kernel.cpp
	$(CXX) $(CXXFLAGS) -c kernel.cpp -o kernel.o

kernel.bin: boot.o kernel.o linker.ld
	$(CXX) -m32 -nostdlib -no-pie -Wl,-T,linker.ld -Wl,-e,_start -o kernel.bin boot.o kernel.o

run: kernel.bin
	qemu-system-i386 -kernel kernel.bin

clean:
	rm -f *.o kernel.bin