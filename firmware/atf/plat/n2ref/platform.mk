#
# Copyright (c) 2013-2026, Arm Limited and Contributors. All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Minimal TF-A port for the QEMU "n2-ref" machine (Neoverse-N2 + GIC-700 +
# 8250 UART).  It reuses the generic QEMU platform plumbing (FIP over the
# boot flash / semihosting, FDT fixups, PSCI) and only overrides the memory
# map, the GIC and the console.
#

PLAT_QEMU_PATH		:=	plat/n2ref
PLAT_QEMU_COMMON_PATH	:=	plat/qemu/common

SEPARATE_CODE_AND_RODATA := 1
ENABLE_STACK_PROTECTOR	 := 0

# Neoverse-N2 implements Armv9.0.
ARM_ARCH_MAJOR		:=	9
ARM_ARCH_MINOR		:=	0

# RD-N2 style GIC-700 (GICv3.1 with GICv4 extensions).
QEMU_USE_GIC_DRIVER	:=	QEMU_GICV3
GIC_ENABLE_V4_EXTN	:=	1
GIC_EXT_INTID		:=	1

include plat/qemu/common/common.mk

# The QEMU Neoverse-N2 model advertises FEAT_TRF but does not implement
# TRFCR_EL1; keep the trace filter disabled so TF-A does not touch it.
ENABLE_TRF_FOR_NS	:=	0

BL2_SOURCES		+=	common/uuid.c

include drivers/arm/gic/v3/gicv3.mk

QEMU_GICV3_SOURCES	:=	${GICV3_SOURCES}			\
				plat/common/plat_gicv3.c		\
				${PLAT_QEMU_COMMON_PATH}/qemu_gicv3.c

QEMU_GIC_SOURCES	:=	${QEMU_GICV3_SOURCES}

ifeq (${ARCH},aarch64)
BL31_SOURCES		+=	drivers/arm/pl061/pl061_gpio.c		\
				drivers/gpio/gpio.c			\
				plat/common/plat_hold_pen.c		\
				${PLAT_QEMU_COMMON_PATH}/qemu_pm.c	\
				${PLAT_QEMU_COMMON_PATH}/topology.c
endif

# The board has a 16550-compatible UART, not a PL011.
PLAT_BL_COMMON_SOURCES	:=	$(filter-out			\
				${PLAT_QEMU_COMMON_PATH}/qemu_console.c	\
				drivers/arm/pl011/${ARCH}/pl011_console.S, \
				${PLAT_BL_COMMON_SOURCES})
PLAT_BL_COMMON_SOURCES	+=	plat/n2ref/n2ref_console.c		\
				drivers/ti/uart/${ARCH}/16550_console.S

# Use a 16550 crash console too.
BL1_SOURCES		:=	$(filter-out				\
				${PLAT_QEMU_COMMON_PATH}/${ARCH}/plat_helpers.S, \
				${BL1_SOURCES})
BL1_SOURCES		+=	plat/n2ref/${ARCH}/plat_helpers.S

BL2_SOURCES		:=	$(filter-out				\
				${PLAT_QEMU_COMMON_PATH}/${ARCH}/plat_helpers.S, \
				${BL2_SOURCES})
BL2_SOURCES		+=	plat/n2ref/${ARCH}/plat_helpers.S

BL31_SOURCES		:=	$(filter-out				\
				${PLAT_QEMU_COMMON_PATH}/${ARCH}/plat_helpers.S, \
				${BL31_SOURCES})
BL31_SOURCES		+=	plat/n2ref/${ARCH}/plat_helpers.S

# No BL32 (OP-TEE) on this platform; keep the generic layout.
BL32_RAM_LOCATION	:=	tdram
ifeq (${BL32_RAM_LOCATION}, tsram)
  BL32_RAM_LOCATION_ID = SEC_SRAM_ID
else ifeq (${BL32_RAM_LOCATION}, tdram)
  BL32_RAM_LOCATION_ID = SEC_DRAM_ID
else
  $(error "Unsupported BL32_RAM_LOCATION value")
endif

$(eval $(call add_define,BL32_RAM_LOCATION_ID))

# Don't have the Linux kernel as a BL33 image by default
ARM_LINUX_KERNEL_AS_BL33	:=	0
$(eval $(call assert_boolean,ARM_LINUX_KERNEL_AS_BL33))
$(eval $(call add_define,ARM_LINUX_KERNEL_AS_BL33))

ARM_PRELOADED_DTB_BASE := PLAT_QEMU_DT_BASE
$(eval $(call add_define,ARM_PRELOADED_DTB_BASE))

# BL1 at offset 0, FIP at offset 0x40000 in the 4 MiB boot RAM.
qemu_fw.bios: bl1 fip
	$(s)echo "  DD      $@"
	$(q)cp ${BUILD_PLAT}/bl1.bin ${BUILD_PLAT}/$@
	$(q)dd if=${BUILD_PLAT}/fip.bin of=${BUILD_PLAT}/$@ bs=64k seek=4 status=none

ifneq (${BL33},)
all: qemu_fw.bios
endif
