# ywin + ycomp (T10.2): compozitorul si biblioteca client. Inclus din Makefile dupa ygfx.mk.
#   user/lib/ywin/*.c -> build/ywin/libywin.a, instalat in sysroot (lib/libywin.a, include/ywin.h, include/ywin_proto.h)
#   user/bin/ycomp/*.c -> /bin/ycomp (compozitorul; scos din BIN_DIRS, are nevoie de -lygfx)
#   user/lib/ywin/demo/ywdemo.c -> /bin/ywdemo, user/lib/ywin/test/comptest.c -> /bin/comptest (test=comp, `make test-comp`)
# Aplicatii: tools/yokai-gcc prog.c -lywin -lygfx
YWIN_DIR     := user/lib/ywin
YWIN_SRCS    := $(wildcard $(YWIN_DIR)/*.c)
YWIN_HDRS    := $(wildcard $(YWIN_DIR)/*.h)
YWIN_OBJS    := $(patsubst $(YWIN_DIR)/%.c,$(BUILD)/ywin/%.o,$(YWIN_SRCS))
YWIN_LIB     := $(BUILD)/ywin/libywin.a
YWIN_STAMP   := $(SYSROOT)/.ywin
YCOMP_DIR    := user/bin/ycomp
YWIN_CFLAGS  := -O2 -Wall -Wextra -Werror

BIN_ELFS += $(BUILD)/ubin/ycomp.elf $(BUILD)/ubin/ywdemo.elf $(BUILD)/ubin/comptest.elf

$(BUILD)/ywin/%.o: $(YWIN_DIR)/%.c $(YWIN_HDRS) $(YGFX_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_PIE) $(YWIN_CFLAGS) -I$(YWIN_DIR) -c $< -o $@

$(YWIN_LIB): $(YWIN_OBJS)
	rm -f $@
	ar rcs $@ $(YWIN_OBJS)

$(YWIN_STAMP): $(YWIN_LIB) $(YWIN_HDRS) $(YGFX_STAMP)
	cp $(YWIN_LIB) $(SYSROOT)/lib/
	cp $(YWIN_HDRS) $(SYSROOT)/include/
	touch $@

$(BUILD)/ubin/ycomp.elf: $(wildcard $(YCOMP_DIR)/*.c) $(wildcard $(YCOMP_DIR)/*.h) $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -I$(YCOMP_DIR) -o $@ $(wildcard $(YCOMP_DIR)/*.c) -lygfx

$(BUILD)/ubin/ywdemo.elf: $(YWIN_DIR)/demo/ywdemo.c $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -o $@ $< -lywin -lygfx

$(BUILD)/ubin/comptest.elf: $(YWIN_DIR)/test/comptest.c $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -o $@ $< -lywin -lygfx

# test=comp: /bin/comptest (kernel: shm, AF_UNIX, poll, input, rt_stats; apoi ycomp cu doi clienti); kernelul verifica
# la sfarsit ecranul, captura de input, obiectele shm / AF_UNIX, procesele si paginile
$(BUILD)/panic-comp.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

test-comp: $(BUILD)/panic-comp.iso
	EXPECT_FILE=tests/expect-comp.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/comp.log
