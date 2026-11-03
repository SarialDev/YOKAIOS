ARCH ?= x86_64
.DEFAULT_GOAL := all

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
         -Ikernel/include -Ikernel -Ilib -MMD -MP -fno-asynchronous-unwind-tables \
         -fno-omit-frame-pointer

ifeq ($(ARCH),x86_64)
ARCHFLAGS = -m64 -mno-red-zone -mcmodel=kernel
LDARCH    = -m elf_x86_64
else
ARCHFLAGS = -m32
LDARCH    = -m elf_i386
endif

# lwIP (T9.2): antetele vendorizate + optiunile/portul din kernel/net; compat/ da string.h etc. fara libc
LWIP_INC = -Ithird_party/lwip/src/include -Ikernel/net -isystem kernel/net/compat

CFLAGS   = $(ARCHFLAGS) $(COMMON) $(LWIP_INC)
CXXFLAGS = $(ARCHFLAGS) $(COMMON) $(LWIP_INC) -fno-exceptions -fno-rtti
ASFLAGS  = $(ARCHFLAGS) $(COMMON)
LDFLAGS  = $(LDARCH) -T boot/linker.ld -nostdlib -z noexecstack -z max-page-size=0x1000 --build-id=none

SRCS := $(shell find boot kernel lib -name '*.S' -o -name '*.c' -o -name '*.cpp')
# lwIP 2.2.1 vendorizat neschimbat: nucleul, IPv4 (ARP/ICMP/UDP/TCP), API-ul netconn/socket si ethernet.c
LWIP_DIR  = third_party/lwip/src
LWIP_SRCS = $(wildcard $(LWIP_DIR)/core/*.c) $(wildcard $(LWIP_DIR)/core/ipv4/*.c) $(wildcard $(LWIP_DIR)/api/*.c) \
            $(LWIP_DIR)/netif/ethernet.c
SRCS += $(LWIP_SRCS)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

# --- Semnaturi Ed25519 (T8.5) ---
# Cheia privata sta in AFARA depozitului: SIGN_KEY=<fisier> (sau $YOKAI_SIGN_KEY), altfel ~/.config/yokai/sign.key; daca
# lipseste, tools/sign.py genereaza o cheie de dezvoltare locala. Cheia PUBLICA se compileaza in kernel (build/yokai_pubkey.c);
# la schimbarea cheii se reconstruiesc kernelul, pachetele .yokai si modulele. Kernelul, pachetele si modulele se semneaza la build.
SIGN_KEY  ?=
SIGN      = python3 tools/sign.py $(if $(SIGN_KEY),--key $(SIGN_KEY))
MKYOKAI   = python3 tools/mkyokai
SIGN_ARG  = $(if $(SIGN_KEY),--key $(SIGN_KEY))
PUBKEY_C  = $(BUILD)/yokai_pubkey.c
OBJS     += $(BUILD)/yokai_pubkey.c.o $(BUILD)/third_party/monocypher/monocypher.c.o

# Se rescrie doar daca cheia s-a schimbat (altfel obiectul nu se reface)
$(PUBKEY_C): FORCE
	@mkdir -p $(BUILD)
	$(SIGN) pubkey --format c -o $@

$(BUILD)/yokai_pubkey.c.o: $(PUBKEY_C)
	$(CC) $(CFLAGS) -c $< -o $@

# Monocypher (EdDSA) e cod tert: fara avertismente-erori; crypto_wipe se redenumeste (lib/crypto/sha256.c are propriul crypto_wipe)
$(BUILD)/third_party/monocypher/monocypher.c.o: CFLAGS += -w -Dcrypto_wipe=monocypher_wipe
# Codul lwIP e tert: fara avertismente-erori
$(patsubst %,$(BUILD)/%.o,$(LWIP_SRCS)): CFLAGS += -w

FORCE:

# Programe user statice, puse in /bin din initrd: user/test/*.S (asm pur) si user/test/*.c (C cu libc-ul propriu)
USER_SRCS := $(wildcard user/test/*.S)
USER_CSRCS := $(wildcard user/test/*.c)
USER_ELFS := $(patsubst user/test/%.S,$(BUILD)/user/%.elf,$(USER_SRCS)) $(patsubst user/test/%.c,$(BUILD)/user/%.elf,$(USER_CSRCS))
# Programe C in /bin si /sbin: user/bin/<nume>.c (un fisier), user/bin/<nume>/*.c (mai multe fisiere, ex. ysh), user/sbin/<nume>.c
BIN_CSRCS   := $(wildcard user/bin/*.c)
BIN_DIRS    := $(filter-out user/bin/lua,$(patsubst %/,%,$(wildcard user/bin/*/)))
SBIN_CSRCS  := $(wildcard user/sbin/*.c)
BIN_ELFS    := $(patsubst user/bin/%.c,$(BUILD)/ubin/%.elf,$(BIN_CSRCS)) $(patsubst user/bin/%,$(BUILD)/ubin/%.elf,$(BIN_DIRS))
BIN_ELFS    += $(BUILD)/ubin/lua.elf
SBIN_ELFS   := $(patsubst user/sbin/%.c,$(BUILD)/usbin/%.elf,$(SBIN_CSRCS))
USER_ASFLAGS = -m64 -ffreestanding -nostdlib -Ikernel/include
USER_LDFLAGS = -m elf_x86_64 -static -nostdlib -e _start -Ttext-segment=0x400000 \
               -z max-page-size=0x1000 -z separate-code -z noexecstack --build-id=none

# libc proprie + SDK: make sysroot instaleaza build/sysroot/{include,lib/{libc.a,crt0.o,yokai.specs}}.
# Aplicatii: gcc --sysroot=build/sysroot -specs=build/sysroot/lib/yokai.specs prog.c -o prog  (sau tools/yokai-gcc)
SYSROOT      = $(BUILD)/sysroot
LIBC_SRCS   := $(wildcard user/libc/src/*.c)
LIBC_OBJS   := $(patsubst user/libc/src/%.c,$(BUILD)/libc/%.o,$(LIBC_SRCS)) $(BUILD)/libc/syscalls.o $(BUILD)/libc/setjmp.o
# lib/crypto/*.c (SHA-256, HMAC, PBKDF2, pwhash) se compileaza si in kernel (SRCS) si in libc.a; antetele merg in sysroot/include/crypto
CRYPTO_SRCS := $(wildcard lib/crypto/*.c)
CRYPTO_HDRS := $(wildcard lib/crypto/*.h)
LIBC_OBJS   += $(patsubst lib/crypto/%.c,$(BUILD)/libc/crypto_%.o,$(CRYPTO_SRCS))
LIBC_OBJS   += $(BUILD)/libc/monocypher.o
LIBC_HDRS   := $(shell find user/libc/include -type f) $(CRYPTO_HDRS)
# Userland cu SSE2/x87 (kernelul salveaza starea FPU la comutare, fork si semnale), fara stack protector/CET.
# libc e -fPIE: aceeasi libc.a merge si in programe ET_EXEC (yokai.specs) si in static-pie (yokai-pie.specs, ASLR)
LIBC_FLAGS   = -Ilib -m64 -O2 -g0 -Wall -Wextra -Werror -std=gnu11 -ffreestanding -nostdinc -isystem user/libc/include \
               -fPIE -fno-stack-protector -fcf-protection=none -fno-asynchronous-unwind-tables \
               -fno-tree-loop-distribute-patterns -fno-math-errno -march=x86-64
UCC          = $(CC) --sysroot=$(SYSROOT) -specs=$(SYSROOT)/lib/yokai.specs
# /bin si /sbin sunt static-pie (ET_DYN): kernelul le incarca la o adresa aleatoare (ASLR). USER_PIE=0 = ET_EXEC la 0x400000
USER_PIE    ?= 1
UCC_PIE      = $(CC) --sysroot=$(SYSROOT) -specs=$(SYSROOT)/lib/yokai-pie.specs
UCC_BIN      = $(if $(filter 1,$(USER_PIE)),$(UCC_PIE),$(UCC))

# initrd.tar (ustar) = rootfs/ + layout-ul din blueprint + programele user in /bin; GRUB il incarca ca modul "initrd"
# si kernelul il monteaza pe "/" cu tarfs (read-only)
ROOTFS     = $(BUILD)/rootfs
INITRD     = $(BUILD)/initrd.tar
ROOT_DIRS  = bin boot dev etc home lib mnt media media/cdrom media/disk opt proc root run sbin sys tmp usr var
ROOTFS_SRC := $(shell find rootfs -type f 2>/dev/null)

# test-rt: jitter strict (<= 1 ms) cu KVM daca /dev/kvm e accesibil, altfel -icount (timp virtual, fara pauzele gazdei)
RT_ACCEL := $(shell [ -w /dev/kvm ] && echo "-enable-kvm -cpu host" || echo "-icount shift=auto")
# -icount ruleaza CPU-urile virtuale pe rand intr-un singur thread: cu mai multe, un CPU asteapta randul zeci de ms
RT_SMP := $(shell [ -w /dev/kvm ] && echo "$(SMP)" || echo 1)

.PHONY: all iso run sysroot test test-ysh test-smash test-relock test-rt test-ysh test-pci-q35 test-coreutils test-lua test-vm test-ypkg test-perm test-caps test-aslr test-crypto test-sign test-crypt verify-sign FORCE test-ahci test-ahci-gpt test-ahci-mbr test-fat32 test-ext4 test-ext4-refuse test-ext4-recover test-ext4w test-ext4-host test-nvme test-e1000 test-lwip test-lwip-ping test-sockets test-dhcp test-iso test-fs test-core run-cd run-ext4 test-mouse test-usb test-input-ps2 test-input-usb test-nofb debug clean

all: $(ISO)

# Lua 5.4 (regulile se declara dupa "all", ca sa nu devina tinta implicita)
include user/bin/lua/lua.mk

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# boot/kaslr.c ruleaza la adresele fizice, inainte ca imaginea sa fie relocata: doar adresare relativa la RIP
$(BUILD)/boot/kaslr.c.o: CFLAGS += -fpie -mcmodel=small -fno-stack-protector -fno-jump-tables \
                                   -fno-tree-loop-distribute-patterns -fvisibility=hidden

# KASLR: link in doi pasi cu --emit-relocs. Pasul 1 (tabel gol) da relocarile, tools/kaslr_relocs.py le pune intr-un
# tabel, pasul 2 il adauga in .kaslr_relocs (dupa _kernel_end, deci nimic nu se muta); verify compara cele doua
KASLR_PRE = $(BUILD)/kernel.pre.elf

$(BUILD)/kaslr_empty.o:
	@mkdir -p $(dir $@)
	printf '.section .kaslr_relocs,"a"\n' | $(CC) -c -x assembler - -o $@

$(KASLR_PRE): $(OBJS) boot/linker.ld $(BUILD)/kaslr_empty.o
	$(LD) $(LDFLAGS) -q -o $@ $(OBJS) $(BUILD)/kaslr_empty.o

$(BUILD)/kaslr_relocs.bin: $(KASLR_PRE) tools/kaslr_relocs.py
	python3 tools/kaslr_relocs.py extract $< $@

$(BUILD)/kaslr_relocs.o: $(BUILD)/kaslr_relocs.bin
	printf '.section .kaslr_relocs,"a"\n.incbin "%s"\n' $< | $(CC) -c -x assembler - -o $@

# GRUB refuza ELF-uri cu sectiuni de relocari: kernel.elf le pastreaza (verify), kernel.bin e fara ele
$(KERNEL): $(OBJS) boot/linker.ld $(BUILD)/kaslr_relocs.o
	$(LD) $(LDFLAGS) -q -o $(BUILD)/kernel.elf $(OBJS) $(BUILD)/kaslr_relocs.o
	python3 tools/kaslr_relocs.py verify $(BUILD)/kernel.elf
	objcopy -R '.rela.*' $(BUILD)/kernel.elf $@

$(BUILD)/user/%.o: user/test/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_ASFLAGS) -c $< -o $@

$(BUILD)/user/%.elf: $(BUILD)/user/%.o
	$(LD) $(USER_LDFLAGS) -o $@ $<

# --- libc / SDK ---
$(BUILD)/libc/syscalls.S: tools/gen_syscalls.sh kernel/include/uapi/syscall_nr.h
	@mkdir -p $(dir $@)
	sh tools/gen_syscalls.sh kernel/include/uapi/syscall_nr.h > $@

$(BUILD)/libc/syscalls.o: $(BUILD)/libc/syscalls.S
	$(CC) -m64 -c $< -o $@

$(BUILD)/libc/setjmp.o: user/libc/src/setjmp.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(BUILD)/libc/%.o: user/libc/src/%.c $(LIBC_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_FLAGS) -c $< -o $@

$(BUILD)/libc/crypto_%.o: lib/crypto/%.c $(CRYPTO_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_FLAGS) -c $< -o $@

$(BUILD)/libc/monocypher.o: third_party/monocypher/monocypher.c third_party/monocypher/monocypher.h
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_FLAGS) -w -Dcrypto_wipe=monocypher_wipe -c $< -o $@

$(BUILD)/libc/crt0.o: user/crt/crt0.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(BUILD)/libc/libc.a: $(LIBC_OBJS)
	rm -f $@
	ar rcs $@ $(LIBC_OBJS)

$(SYSROOT)/.installed: $(BUILD)/libc/libc.a $(BUILD)/libc/crt0.o $(LIBC_HDRS) user/libc/yokai.specs user/libc/yokai-pie.specs kernel/include/uapi/syscall_nr.h kernel/include/uapi/caps.h kernel/include/uapi/crypt.h
	rm -rf $(SYSROOT)
	mkdir -p $(SYSROOT)/include/uapi $(SYSROOT)/lib
	cp -r user/libc/include/. $(SYSROOT)/include/
	mkdir -p $(SYSROOT)/include/crypto && cp $(CRYPTO_HDRS) $(SYSROOT)/include/crypto/
	cp kernel/include/uapi/syscall_nr.h kernel/include/uapi/caps.h kernel/include/uapi/crypt.h $(SYSROOT)/include/uapi/
	cp $(BUILD)/libc/libc.a $(BUILD)/libc/crt0.o user/libc/yokai.specs user/libc/yokai-pie.specs $(SYSROOT)/lib/
	touch $@

sysroot: $(SYSROOT)/.installed

# Programele C se leaga cu libc-ul din sysroot (spec-ul pune -nostdinc, crt0, libc.a si flagurile de legare)
$(BUILD)/user/%.elf: user/test/%.c $(SYSROOT)/.installed
	@mkdir -p $(dir $@)
	$(UCC) -O2 -Wall -Wextra -Werror -o $@ $<

$(BUILD)/ubin/%.elf: user/bin/%.c $(wildcard user/bin/*.h) $(SYSROOT)/.installed
	@mkdir -p $(dir $@)
	$(UCC_BIN) -O2 -Wall -Wextra -Werror -o $@ $<

# Un program din mai multe fisiere: user/bin/<nume>/*.c (+ *.h) -> /bin/<nume>
define BIN_DIR_RULE
$$(BUILD)/ubin/$(notdir $(1)).elf: $$(wildcard $(1)/*.c) $$(wildcard $(1)/*.h) $$(SYSROOT)/.installed
	@mkdir -p $$(dir $$@)
	$$(UCC_BIN) -O2 -Wall -Wextra -Werror -o $$@ $$(wildcard $(1)/*.c)
endef
$(foreach d,$(BIN_DIRS),$(eval $(call BIN_DIR_RULE,$(d))))

$(BUILD)/usbin/%.elf: user/sbin/%.c $(SYSROOT)/.installed
	@mkdir -p $(dir $@)
	$(UCC_BIN) -O2 -Wall -Wextra -Werror -o $@ $<

# Pachete .yokai (T8.4): user/pkg/<nume>/{manifest.toml, root/, *.c}; fiecare *.c se compileaza in root/opt/<nume>/bin/;
# tools/mkyokai pune hash-urile in manifest si face arhiva tar. Ajung in initrd la /usr/share/ypkg/<nume>.yokai
PKG_NAMES   := $(notdir $(patsubst %/,%,$(wildcard user/pkg/*/)))
PKG_FILES   := $(shell find user/pkg -type f 2>/dev/null)
PKG_YOKAI   := $(patsubst %,$(BUILD)/pkg/%.yokai,$(PKG_NAMES))

$(BUILD)/pkg/%.yokai: tools/mkyokai tools/sign.py $(PUBKEY_C) $(PKG_FILES) $(SYSROOT)/.installed
	rm -rf $(BUILD)/pkgstage/$*
	mkdir -p $(BUILD)/pkgstage/$*
	cp user/pkg/$*/manifest.toml $(BUILD)/pkgstage/$*/
	cp -r user/pkg/$*/root $(BUILD)/pkgstage/$*/root
	for c in user/pkg/$*/*.c; do \
	    [ -f $$c ] || continue; \
	    mkdir -p $(BUILD)/pkgstage/$*/root/opt/$*/bin; \
	    $(UCC) -O2 -Wall -Wextra -Werror -o $(BUILD)/pkgstage/$*/root/opt/$*/bin/$$(basename $$c .c) $$c || exit 1; \
	done
	$(MKYOKAI) build $(BUILD)/pkgstage/$* -o $@ $(SIGN_ARG)

# Module de kernel (T8.5): modules/<nume>/<nume>.c -> ELF relocabil (ET_REL) -> semnat (ELF || semnatura || magic) -> /lib/modules/<nume>.ko
MOD_NAMES   := $(notdir $(patsubst %/,%,$(wildcard modules/*/)))
MOD_KO      := $(patsubst %,$(BUILD)/modules/%.ko,$(MOD_NAMES))
MOD_CFLAGS   = -c -O2 -g0 -Wall -Wextra -Werror -std=gnu11 -ffreestanding -nostdlib -fPIC -fno-plt -fno-common -fno-stack-protector \
               -fcf-protection=none -mno-red-zone -mno-sse -mno-mmx -mno-80387 -fno-asynchronous-unwind-tables -march=x86-64

define MOD_RULE
$$(BUILD)/modules/$(1).o: $$(wildcard modules/$(1)/*.c)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(MOD_CFLAGS) -o $$@ $$^
endef
$(foreach m,$(MOD_NAMES),$(eval $(call MOD_RULE,$(m))))

$(BUILD)/modules/%.ko: $(BUILD)/modules/%.o tools/sign.py $(PUBKEY_C)
	$(SIGN) sign-module $< -o $@

# Semnatura detasata a kernelului (ajunge in ISO ca /boot/kernel.sig; se verifica pe gazda cu `make verify-sign`)
$(BUILD)/kernel.sig: $(KERNEL) tools/sign.py $(PUBKEY_C)
	$(SIGN) sign-kernel $(KERNEL) -o $@

$(INITRD): $(USER_ELFS) $(BIN_ELFS) $(SBIN_ELFS) $(ROOTFS_SRC) $(LUA_SHARE) $(PKG_YOKAI) $(MOD_KO) Makefile
	rm -rf $(ROOTFS)
	mkdir -p $(addprefix $(ROOTFS)/,$(ROOT_DIRS))
	cp -r rootfs/. $(ROOTFS)/
	for f in $(USER_ELFS) $(BIN_ELFS); do cp $$f $(ROOTFS)/bin/$$(basename $$f .elf); done
	for f in $(SBIN_ELFS); do cp $$f $(ROOTFS)/sbin/$$(basename $$f .elf); done
	mkdir -p $(ROOTFS)/usr/share/lua && cp $(LUA_SHARE) $(ROOTFS)/usr/share/lua/
	mkdir -p $(ROOTFS)/usr/share/ypkg && cp $(PKG_YOKAI) $(ROOTFS)/usr/share/ypkg/
	mkdir -p $(ROOTFS)/lib/modules && for f in $(MOD_KO); do cp $$f $(ROOTFS)/lib/modules/; done
	ln -s ../proc/mounts $(ROOTFS)/etc/mtab
	ln -s ../run/resolv.conf $(ROOTFS)/etc/resolv.conf
	find $(ROOTFS) -type d -exec chmod 755 {} +
	find $(ROOTFS) -type f -exec chmod 644 {} +
	chmod 755 $(ROOTFS)/bin/* $(ROOTFS)/sbin/*
	chmod 4755 $(ROOTFS)/bin/passwd
	chmod 700 $(ROOTFS)/root
	chmod 1777 $(ROOTFS)/tmp
	chmod 600 $(ROOTFS)/etc/shadow
	mkdir -p $(ROOTFS)/home/yokai && chmod 700 $(ROOTFS)/home/yokai
	tar --format=ustar --owner=0 --group=0 --numeric-owner --sort=name --mtime='2026-01-01 00:00Z' \
	    --exclude=./home/yokai -cf $@ -C $(ROOTFS) .
	tar --format=ustar --owner=1000 --group=1000 --numeric-owner --mtime='2026-01-01 00:00Z' \
	    -rf $@ -C $(ROOTFS) ./home/yokai

$(ISO): $(KERNEL) $(BUILD)/kernel.sig $(INITRD) boot/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/kernel.sig $(INITRD) $(BUILD)/iso/boot/
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso 2>/dev/null

run: $(ISO)
	$(QEMU) $(QEMU_FLAGS)

$(TEST_ISO): $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-test/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-test/boot/
	cp boot/grub-test.cfg $(BUILD)/iso-test/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-test 2>/dev/null

# make test = testele de nucleu (un boot) + integrarea stocarii (test-fs)
test: test-core test-ysh test-fs

test-core: $(TEST_ISO)
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

# libc proprie: numai test=libc (hello + libctest); `make test` il ruleaza si el, impreuna cu restul
$(BUILD)/panic-libc.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-libc: $(BUILD)/panic-libc.iso
	EXPECT_FILE=tests/expect-libc.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/libc.log

# Lua 5.4 (/bin/lua): numai test=lua; selftest.lua ruleaza ca proces user
$(BUILD)/panic-lua.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-lua: $(BUILD)/panic-lua.iso
	EXPECT_FILE=tests/expect-lua.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/lua.log

# utilitare de baza (user/bin): numai test=coreutils (/bin/coretest porneste fiecare utilitar si verifica iesirea)
$(BUILD)/panic-coreutils.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-coreutils: $(BUILD)/panic-coreutils.iso
	EXPECT_FILE=tests/expect-coreutils.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/coreutils.log

# memorie virtuala + semnale (T8): numai test=vm (/bin/mmtest: mmap, fork CoW 64 MiB, semnale, Ctrl-C)
$(BUILD)/panic-vm.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-vm: $(BUILD)/panic-vm.iso
	EXPECT_FILE=tests/expect-vm.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/vm.log

# pachete .yokai (T8.4): numai test=ypkg (/bin/ypkgtest instaleaza hello.yokai, facut de tools/mkyokai, intr-o radacina in /tmp)
$(BUILD)/panic-ypkg.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-ypkg: $(BUILD)/panic-ypkg.iso
	python3 tools/mkyokai verify $(BUILD)/pkg/hello.yokai
	EXPECT_FILE=tests/expect-ypkg.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/ypkg.log

# utilizatori si drepturi (T8.x): numai test=perm (/bin/permtest: root + utilizatorul 1000, rwx, sticky, setuid, umask, su)
$(BUILD)/panic-perm.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-perm: $(BUILD)/panic-perm.iso
	EXPECT_FILE=tests/expect-perm.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/perm.log

# capabilitati per proces (T8.x): numai test=caps (/bin/captest: mostenire, capset, mount, RT, ioperm, kill, setuid, politica exec.caps)
$(BUILD)/panic-caps.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-caps: $(BUILD)/panic-caps.iso
	EXPECT_FILE=tests/expect-caps.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/caps.log

# ASLR + KASLR: test=aslr (verificarile din kernel + /bin/aslrtest static-pie); tests/check_aslr.sh porneste de doua
# ori acelasi ISO si cere adrese diferite pentru kernel_main si pentru stiva / heap / mmap / codul unui program user
$(BUILD)/panic-aslr.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-aslr: $(BUILD)/panic-aslr.iso
	SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/check_aslr.sh $< $(BUILD)/aslr

# aslrtest e static-pie si cand USER_PIE=0
$(BUILD)/user/aslrtest.elf: user/test/aslrtest.c $(SYSROOT)/.installed
	@mkdir -p $(dir $@)
	$(UCC_PIE) -O2 -Wall -Wextra -Werror -o $@ $<

# criptografie (lib/crypto in kernel si in userland): numai test=crypto (vectori RFC 6234 / 4231 / 7914)
$(BUILD)/panic-crypto.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-crypto: $(BUILD)/panic-crypto.iso
	EXPECT_FILE=tests/expect-crypto.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/crypto.log

# semnaturi Ed25519 (T8.5): `make verify-sign` verifica pe gazda semnaturile kernelului, modulelor si pachetelor; test-sign adauga
# test=sign (kernel: module semnate / modificate, domenii separate) si respingerea pe gazda a unui pachet modificat cu un octet
$(BUILD)/panic-sign.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

verify-sign: $(BUILD)/kernel.sig $(MOD_KO) $(PKG_YOKAI)
	$(SIGN) verify-kernel $(KERNEL) $(BUILD)/kernel.sig
	for f in $(MOD_KO); do $(SIGN) verify-module $$f || exit 1; done
	for f in $(PKG_YOKAI); do $(MKYOKAI) verify $$f $(SIGN_ARG) || exit 1; done

test-sign: $(BUILD)/panic-sign.iso verify-sign
	SIGN_KEY="$(SIGN_KEY)" sh tests/check_sign.sh $(BUILD)
	EXPECT_FILE=tests/expect-sign.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/sign.log

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

# ysh interactiv (T7.2): kernelul porneste /sbin/init (care monteaza /dev /tmp /run /proc, cmdline nopseudo) si asteapta;
# tests/ysh_test.py tasteaza in ysh prin monitorul QEMU un script cu editare de linie, istoric, Tab, pipe-uri si redirectari,
# iar la final verifica logul. Dureaza cateva minute (tastele se trimit una cate una)
$(BUILD)/yokai-ysh.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-ysh/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-ysh/boot/
	sed 's/test=[^ ]*/test=ysh nopseudo/' boot/grub-test.cfg > $(BUILD)/iso-ysh/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-ysh 2>/dev/null

test-ysh: $(BUILD)/yokai-ysh.iso
	TEST_TIMEOUT=900 EXPECT_FILE=tests/expect-ysh.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/ysh.log
	python3 tests/ysh_test.py check $(BUILD)/ysh.log

test-rt: $(BUILD)/yokai-rt.iso
	@echo "test-rt cu: $(RT_ACCEL), $(RT_SMP) CPU"
	EXPECT_FILE=tests/expect-rt.txt SMP=$(RT_SMP) QEMU_EXTRA="$(RT_ACCEL) $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/rt.log

# GDB pe :1234 (symbol-file build/kernel.elf): ISO-ul de debug porneste cu "nokaslr", ca adresele sa fie cele din ELF
$(BUILD)/yokai-debug.iso: $(KERNEL) $(INITRD) boot/grub.cfg
	@mkdir -p $(BUILD)/iso-debug/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-debug/boot/
	sed 's|multiboot2 /boot/kernel.bin|& nokaslr|' boot/grub.cfg > $(BUILD)/iso-debug/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-debug 2>/dev/null

debug: $(BUILD)/yokai-debug.iso
	$(QEMU) $(subst $(ISO),$<,$(QEMU_FLAGS)) -s -S

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

# NVMe: namespace 1 = imaginea GPT de 32 MiB (tests/mkdisk.py), namespace 2 = 4 MiB cu sectoare de 4096 B (tests/mknvme.py);
# kernelul ruleaza doar test=nvme (MSI-X, PRP cu lista, partitii nvme0n1p1.., cache)
$(BUILD)/yokai-nvme.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-nvme/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-nvme/boot/
	sed 's/test=[^ ]*/test=nvme/' boot/grub-test.cfg > $(BUILD)/iso-nvme/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-nvme 2>/dev/null

NVME_QEMU = -boot order=d -drive file=$(BUILD)/nvme-ns1.img,if=none,id=nv0,format=raw -drive file=$(BUILD)/nvme-ns2.img,if=none,id=nv1,format=raw -device nvme,id=nvme0,serial=yokai0 -device nvme-ns,drive=nv0,bus=nvme0,nsid=1 -device nvme-ns,drive=nv1,bus=nvme0,nsid=2,logical_block_size=4096,physical_block_size=4096

# Imaginile se refac la fiecare rulare (testul scrie pe ele)
test-nvme: $(BUILD)/yokai-nvme.iso
	python3 tests/mkdisk.py gpt $(BUILD)/nvme-ns1.img
	python3 tests/mknvme.py $(BUILD)/nvme-ns2.img
	EXPECT_FILE=tests/expect-nvme.txt SMP=$(SMP) QEMU_EXTRA="$(NVME_QEMU) $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/nvme.log

# ISO 9660: CD-ul de boot (cd0 pe AHCI = sr0, acelasi fisier ca -cdrom) montat pe /media/cdrom, plus o imagine facuta cu
# xorriso (tests/mkiso.sh, cd1 = sr1) montata de test pe /mnt
$(BUILD)/yokai-iso9660.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-iso9660/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-iso9660/boot/
	sed 's/test=[^ ]*/test=iso9660/' boot/grub-test.cfg > $(BUILD)/iso-iso9660/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-iso9660 2>/dev/null

$(BUILD)/isotest.iso: tests/mkiso.sh
	@mkdir -p $(BUILD)
	sh tests/mkiso.sh $@ $(BUILD)/isotest-work

ISO_QEMU = -boot order=d -device ahci,id=ahci0 -drive file=$(BUILD)/yokai-iso9660.iso,if=none,id=cd0,format=raw,media=cdrom,readonly=on -device ide-cd,drive=cd0,bus=ahci0.0 -drive file=$(BUILD)/isotest.iso,if=none,id=cd1,format=raw,media=cdrom,readonly=on -device ide-cd,drive=cd1,bus=ahci0.1

test-iso: $(BUILD)/yokai-iso9660.iso $(BUILD)/isotest.iso
	EXPECT_FILE=tests/expect-iso9660.txt SMP=$(SMP) QEMU_EXTRA="$(ISO_QEMU) $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/iso9660.log

# e1000 (QEMU -nic user,model=e1000 pus de run_qemu.sh): kernelul ruleaza doar test=e1000 (ARP catre gateway-ul slirp,
# statistici, handler, inele rasucite); QEMU nu da MSI pe e1000, deci driverul merge in modul de interogare
$(BUILD)/yokai-e1000.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-e1000/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-e1000/boot/
	sed 's/test=[^ ]*/test=e1000/' boot/grub-test.cfg > $(BUILD)/iso-e1000/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-e1000 2>/dev/null

include tests/lwip.mk
include tests/sockets.mk
include tests/dhcp.mk

test-e1000: $(BUILD)/yokai-e1000.iso
	EXPECT_FILE=tests/expect-e1000.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/e1000.log

# Dezvoltare interactiva: ISO-ul apare si ca CD pe AHCI (/media/cdrom) si exista un disc NVMe gol de 64 MiB (nvme0n1)
build/nvme-run.img:
	@mkdir -p $(BUILD)
	truncate -s 64M $@

run-cd: $(ISO) $(BUILD)/nvme-run.img
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -smp $(SMP) -serial stdio -boot order=d -device ahci,id=ahci0 \
	    -drive file=$(ISO),if=none,id=cd0,format=raw,media=cdrom,readonly=on -device ide-cd,drive=cd0,bus=ahci0.0 \
	    -drive file=$(BUILD)/nvme-run.img,if=none,id=nvr,format=raw -device nvme,drive=nvr,serial=yokai1 $(QEMU_EXTRA)

# Stocare, integrare (T6.T): tests/mkdisk.sh face un disc GPT (sda1 FAT32, sda2 ext4) cu fisiere de continut cunoscut si
# MANIFEST.SHA; kernelul (test=fs) boot 1 verifica SHA-256 si scrie fisiere noi pe ambele, boot 2 le regaseste; dupa
# fiecare boot tests/check_fs.sh ruleaza fsck.fat -n, e2fsck -fn si compara SHA-256 de pe gazda
$(BUILD)/yokai-fs.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-fs/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-fs/boot/
	sed 's/test=[^ ]*/test=fs/' boot/grub-test.cfg > $(BUILD)/iso-fs/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-fs 2>/dev/null

test-fs: $(BUILD)/yokai-fs.iso tests/mkdisk.sh tests/check_fs.sh
	sh tests/mkdisk.sh $(BUILD)/fs.img $(BUILD)/fs
	TEST_TIMEOUT=900 DISK=$(BUILD)/fs.img EXPECT_FILE=tests/expect-fs1.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/fs-boot1.log
	sh tests/check_fs.sh $(BUILD)/fs.img 1 $(BUILD)/fs
	TEST_TIMEOUT=900 DISK=$(BUILD)/fs.img EXPECT_FILE=tests/expect-fs2.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/fs-boot2.log
	sh tests/check_fs.sh $(BUILD)/fs.img 2 $(BUILD)/fs

# Disc criptat (T8.8): test=crypt (vectori AES / XTS / ChaCha20, ram0 cu /bin/cryptsetup, ext4 pe sda criptat de tools/cryptimg);
# tests/check_crypt.sh face imaginea, boot-eaza si verifica pe gazda ca discul brut nu are text in clar. Trei rulari:
# AES-XTS (CPU implicit QEMU = fara AES-NI: varianta software), ChaCha20, AES-XTS cu "-cpu max" (AES-NI daca QEMU il emuleaza)
$(BUILD)/yokai-crypt.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg
	@mkdir -p $(BUILD)/iso-crypt/boot/grub
	cp $(KERNEL) $(INITRD) $(BUILD)/iso-crypt/boot/
	sed 's/test=[^ ]*/test=crypt/' boot/grub-test.cfg > $(BUILD)/iso-crypt/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso-crypt 2>/dev/null

test-crypt: $(BUILD)/yokai-crypt.iso tests/check_crypt.sh
	SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/check_crypt.sh $< $(BUILD)/crypt-xts xts
	SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/check_crypt.sh $< $(BUILD)/crypt-chacha chacha20
	SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA) -cpu max" sh tests/check_crypt.sh $< $(BUILD)/crypt-xts-ni xts
