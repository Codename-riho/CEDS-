# CEDS LKM - build out-of-tree terhadap kernel yang SAMA dengan yang berjalan.
#
# Contoh (kernel Android, clang, hasil build di out/):
#   make -C /path/kernel O=out ARCH=arm64 LLVM=1 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
#   make KDIR=/path/kernel KOUT=/path/kernel/out \
#        KARGS="LLVM=1 CROSS_COMPILE=aarch64-linux-gnu-"
#
# Hasil: ceds_lkm.ko di folder ini. Gunakan toolchain, defconfig, dan commit
# kernel yang identik dengan boot image di perangkat, kalau tidak insmod
# akan menolak (vermagic / modversions).

obj-m := ceds_lkm.o

ifeq ($(KERNELRELEASE),)

KDIR ?= $(error Set KDIR=/path/ke/sumber/kernel)
KOUT ?= $(KDIR)/out
ARCH ?= arm64
KARGS ?=
PWD  := $(shell pwd)

modules:
	$(MAKE) -C $(KDIR) O=$(KOUT) M=$(PWD) ARCH=$(ARCH) $(KARGS) modules

clean:
	$(MAKE) -C $(KDIR) O=$(KOUT) M=$(PWD) ARCH=$(ARCH) $(KARGS) clean

.PHONY: modules clean

endif
