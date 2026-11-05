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

BIN_ELFS += $(BUILD)/ubin/ycomp.elf $(BUILD)/ubin/ywdemo.elf $(BUILD)/ubin/comptest.elf $(BUILD)/ubin/wmtest.elf

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

# T10.3: managerul de ferestre sta in user/bin/ycomp/wm/ (compilat in acelasi executabil)
YCOMP_SRCS := $(wildcard $(YCOMP_DIR)/*.c) $(wildcard $(YCOMP_DIR)/wm/*.c)
YCOMP_HDRS := $(wildcard $(YCOMP_DIR)/*.h) $(wildcard $(YCOMP_DIR)/wm/*.h)

$(BUILD)/ubin/ycomp.elf: $(YCOMP_SRCS) $(YCOMP_HDRS) $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -I$(YCOMP_DIR) -o $@ $(YCOMP_SRCS) -lygfx

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

# T10.3: managerul de ferestre (user/bin/ycomp/wm/*), testat de /bin/wmtest (test=wm, `make test-wm`): cinci ferestre,
# mutare / redimensionare / snap / minimizare / Alt+Tab / tiling / spatii de lucru prin input injectat
$(BUILD)/ubin/wmtest.elf: $(YWIN_DIR)/test/wmtest.c $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -o $@ $< -lywin -lygfx

$(BUILD)/panic-wm.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

.PHONY: test-wm
test-wm: $(BUILD)/panic-wm.iso
	EXPECT_FILE=tests/expect-wm.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/wm.log

# T10.4: bara de task-uri + meniul Start (user/bin/ypanel/*.c) si terminalul minimal (user/bin/yterm/*.c); ambele cu -lywin -lygfx,
# deci scoase din regula generica a directoarelor din user/bin. test=panel (/bin/paneltest, `make test-panel`)
BIN_DIRS := $(filter-out user/bin/ypanel user/bin/yterm,$(BIN_DIRS))
BIN_ELFS := $(filter-out $(BUILD)/ubin/ypanel.elf $(BUILD)/ubin/yterm.elf,$(BIN_ELFS))
BIN_ELFS += $(BUILD)/ubin/ypanel.elf $(BUILD)/ubin/yterm.elf $(BUILD)/ubin/paneltest.elf
YPANEL_SRCS := $(wildcard user/bin/ypanel/*.c)
YPANEL_HDRS := $(wildcard user/bin/ypanel/*.h)
YTERM_SRCS  := $(wildcard user/bin/yterm/*.c)

$(BUILD)/ubin/ypanel.elf: $(YPANEL_SRCS) $(YPANEL_HDRS) $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -Iuser/bin/ypanel -o $@ $(YPANEL_SRCS) -lywin -lygfx

$(BUILD)/ubin/yterm.elf: $(YTERM_SRCS) $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -o $@ $(YTERM_SRCS) -lywin -lygfx

$(BUILD)/ubin/paneltest.elf: $(YWIN_DIR)/test/paneltest.c $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -o $@ $< -lywin -lygfx

$(BUILD)/panic-panel.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

.PHONY: test-panel
test-panel: $(BUILD)/panic-panel.iso
	EXPECT_FILE=tests/expect-panel.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/panel.log

test: test-panel

# T10.5: aplicatii GUI (yterm cu vt.c peste pty, yfiles, ysettings) + yui.h (ajutoare de desen) si testul appstest (test=apps)
BIN_DIRS := $(filter-out user/bin/yfiles user/bin/ysettings,$(BIN_DIRS))
BIN_ELFS := $(filter-out $(BUILD)/ubin/yfiles.elf $(BUILD)/ubin/ysettings.elf,$(BIN_ELFS))
BIN_ELFS += $(BUILD)/ubin/yfiles.elf $(BUILD)/ubin/ysettings.elf $(BUILD)/ubin/appstest.elf
YTERM_HDRS  := $(wildcard user/bin/yterm/*.h)
YFILES_SRCS := $(wildcard user/bin/yfiles/*.c)
YSET_SRCS   := $(wildcard user/bin/ysettings/*.c)
YSET_HDRS   := $(wildcard user/bin/ysettings/*.h)

$(BUILD)/ubin/yterm.elf: $(YTERM_HDRS)

$(BUILD)/ubin/yfiles.elf: $(YFILES_SRCS) $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -o $@ $(YFILES_SRCS) -lywin -lygfx

$(BUILD)/ubin/ysettings.elf: $(YSET_SRCS) $(YSET_HDRS) $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -Iuser/bin/ysettings -o $@ $(YSET_SRCS) -lywin -lygfx

$(BUILD)/ubin/appstest.elf: $(YWIN_DIR)/test/appstest.c $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -o $@ $< -lywin -lygfx

$(BUILD)/panic-apps.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

.PHONY: test-apps
test-apps: $(BUILD)/panic-apps.iso
	EXPECT_FILE=tests/expect-apps.txt SMP=$(SMP) QEMU_EXTRA="$(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/apps.log

test: test-apps

# T10.6: virtio-gpu 2D (kernel/drivers/gpu/virtio_gpu.*, /dev/gpu0) + backend-ul ycomp user/bin/ycomp/backend_virtio.c (intra
# singur in YCOMP_SRCS). test=gpu: /bin/gputest; QEMU primeste -device virtio-gpu-pci pe langa VGA-ul de boot (GRUB / fbcon)
BIN_ELFS += $(BUILD)/ubin/gputest.elf

$(BUILD)/ubin/gputest.elf: $(YWIN_DIR)/test/gputest.c $(YWIN_STAMP)
	@mkdir -p $(dir $@)
	$(UCC_BIN) $(YWIN_CFLAGS) -o $@ $< -lywin -lygfx

$(BUILD)/panic-gpu.iso: $(KERNEL) $(INITRD) boot/grub-test.cfg

.PHONY: test-gpu
test-gpu: $(BUILD)/panic-gpu.iso
	EXPECT_FILE=tests/expect-gpu.txt SMP=$(SMP) QEMU_EXTRA="-device virtio-gpu-pci $(QEMU_EXTRA)" sh tests/run_qemu.sh $< $(BUILD)/gpu.log

test: test-gpu
