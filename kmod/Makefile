# SPDX-License-Identifier: MIT
#
# Two modes:
#   - inside the DDK image (CI): KERNEL_SRC / CLANG_DIR come from the image
#   - locally: pass KERNEL_SRC= and CLANG_DIR=

obj-m += adbhide_kmod.o
adbhide_kmod-y := adbhide_main.o adbhide_uids.o adbhide_probes.o adbhide_props.o
adbhide_kmod-y += adbhide_netseq.o adbhide_vma.o

ifeq ($(KERNELRELEASE),)

ARCH ?= arm64
KERNEL_SRC ?= $(if $(KDIR),$(KDIR),/opt/ddk/kdir/android15-6.6)
CLANG_DIR ?= $(shell d=$$(ls -d /opt/ddk/clang/clang-r* 2>/dev/null | tail -1); [ -x "$$d/bin/clang" ] && echo "$$d/bin" || echo "$$d")

CC      := $(CLANG_DIR)/clang
LD      := $(CLANG_DIR)/ld.lld
AR      := $(CLANG_DIR)/llvm-ar
NM      := $(CLANG_DIR)/llvm-nm
OBJCOPY := $(CLANG_DIR)/llvm-objcopy
OBJDUMP := $(CLANG_DIR)/llvm-objdump
STRIP   := $(CLANG_DIR)/llvm-strip

MAKE_ARGS := -C $(KERNEL_SRC) M=$(CURDIR) \
	ARCH=$(ARCH) LLVM=1 LLVM_IAS=1 \
	CC=$(CC) LD=$(LD) AR=$(AR) NM=$(NM) \
	OBJCOPY=$(OBJCOPY) OBJDUMP=$(OBJDUMP) STRIP=$(STRIP)

.PHONY: all strip clean check-env

all check-env:
	@test -d "$(KERNEL_SRC)" || { echo "KERNEL_SRC not found: $(KERNEL_SRC)"; exit 1; }
	@test -x "$(CC)" || { echo "clang not found: $(CC)"; exit 1; }
	$(MAKE) $(MAKE_ARGS) modules

strip: all
	$(STRIP) -d $(CURDIR)/adbhide_kmod.ko

clean:
	$(MAKE) $(MAKE_ARGS) clean

endif
