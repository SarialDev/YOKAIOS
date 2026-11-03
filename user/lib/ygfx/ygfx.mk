# ygfx (T10.1): biblioteca grafica user (numai CPU). Inclus din Makefile dupa "all".
#   user/lib/ygfx/*.c -> build/ygfx/libygfx.a, instalat in sysroot (lib/libygfx.a, include/ygfx.h, include/stb_truetype.h)
#   user/lib/ygfx/demo/*.c -> /bin/gfxdemo (scena in ambele teme, direct in /dev/fb0)
#   user/lib/ygfx/test/ygfxtest.c -> /bin/ygfxtest (test=ygfx, `make test-ygfx`)
# Fonturile (rootfs/usr/share/fonts/DejaVuSans*.ttf) ajung in initrd odata cu restul rootfs-ului.
# Aplicatii: tools/yokai-gcc prog.c -lygfx  (sau $(UCC_BIN) ... -lygfx)
YGFX_DIR     := user/lib/ygfx
YGFX_SRCS    := $(wildcard $(YGFX_DIR)/*.c)
YGFX_HDRS    := $(wildcard $(YGFX_DIR)/*.h) third_party/stb_truetype.h
YGFX_OBJS    := $(patsubst $(YGFX_DIR)/%.c,$(BUILD)/ygfx/%.o,$(YGFX_SRCS))
YGFX_LIB     := $(BUILD)/ygfx/libygfx.a
YGFX_STAMP   := $(SYSROOT)/.ygfx
# Compilata cu spec-ul PIE (ca libc, -fPIE): aceeasi biblioteca merge in ET_EXEC si in static-pie
YGFX_CFLAGS  := -O2 -Wall -Wextra -Werror -I$(YGFX_DIR) -Ithird_party
GFXDEMO_SRCS := $(wildcard $(YGFX_DIR)/demo/*.c)

BIN_ELFS += $(BUILD)/ubin/gfxdemo.elf $(BUILD)/ubin/ygfxtest.elf

$(BUILD)/ygfx/%.o: $(YGFX_DIR)/%.c $(YGFX_HDRS) $(SYSROOT)/.installed
	@mkdir -p $(dir $@)
	$(UCC_PIE) $(YGFX_CFLAGS) -c $< -o $@

# stb_truetype (third_party, neschimbat) e inclus in font.c: functiile lui nefolosite nu sunt erori
$(BUILD)/ygfx/font.o: YGFX_CFLAGS += -Wno-unused-function

$(YGFX_LIB): $(YGFX_OBJS)
	rm -f $@
	ar rcs $@ $(YGFX_OBJS)

$(YGFX_STAMP): $(YGFX_LIB) $(YGFX_DIR)/ygfx.h third_party/stb_truetype.h $(SYSROOT)/.installed
	cp $(YGFX_LIB) $(SYSROOT)/lib/
	cp $(YGFX_DIR)/ygfx.h third_party/stb_truetype.h $(SYSROOT)/include/
	touch $@

$(BUILD)/ubin/gfxdemo.elf: $(GFXDEMO_SRCS) $(wildcard $(YGFX_DIR)/demo/*.h) $(YGFX_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) -O2 -Wall -Wextra -Werror -I$(YGFX_DIR)/demo -o $@ $(GFXDEMO_SRCS) -lygfx

$(BUILD)/ubin/ygfxtest.elf: $(YGFX_DIR)/test/ygfxtest.c $(YGFX_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) -O2 -Wall -Wextra -Werror -o $@ $< -lygfx

# test=ygfx: /bin/ygfxtest (biblioteca + /dev/fb0) si /bin/gfxdemo -t split -s 0; kernelul verifica la sfarsit ca
# ecranul a revenit consolei (nicio mapare ramasa) si ca nu s-au pierdut pagini
$(BUILD)/panic-ygfx.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-ygfx: $(BUILD)/panic-ygfx.iso
	EXPECT_FILE=tests/expect-ygfx.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/ygfx.log
