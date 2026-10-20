# `n2-ref` firmware: TF-A + EDK2

This directory contains the firmware sources that boot on the QEMU `n2-ref`
machine described by `../n2-ref-usage.md`.

Tested versions:

| Component | Version |
|---|---|
| QEMU | 11.1.2 (this tree) |
| TF-A | v2.15.0 |
| EDK2 | `edk2-stable202608` |
| Toolchain | `aarch64-linux-gnu-` GCC |

The result is a single `qemu_fw.bios` image that `-bios` loads at address 0.
TF-A runs BL1 (EL3), BL2, BL31 (EL3 runtime), then EDK2 as BL33. EDK2 gets
the QEMU device tree through `x0`, initialises its 16550 console, and reaches
the UEFI Boot Manager and the UEFI Shell.

## 1. Board assumptions

| Resource | Address |
|---|---|
| Boot RAM / BL1 ROM | `0x0000_0000` (4 MiB) |
| FIP | `0x0004_0000` |
| Secure SRAM | `0x0400_0000` (1 MiB) |
| Non-secure SRAM | `0x0600_0000` (512 KiB) |
| 8250 UART | `0x0C00_0000`, reg-shift 2, 1843200 Hz, 115200 baud |
| PL031 RTC | `0x0C01_0000` |
| GICD / GITS / GICR | `0x0E00_0000` / `0x0E04_0000` / `0x0E08_0000` |
| PCIe ECAM / MMIO | `0x4000_0000` / `0x6000_0000` |
| DRAM | `0x8000_0000`, 2 GiB |
| BL33 load address | `0xA000_0000` |

`-m 2G` is required because TF-A places BL33 at `0xA000_0000`.

## 2. Build EDK2

Clone EDK2 with its submodules and initialise the build environment:

```sh
git clone --recursive -b edk2-stable202608 \
    https://github.com/tianocore/edk2.git
cd edk2
git submodule update --init --recursive
make -C BaseTools
export WORKSPACE=$PWD
export PACKAGES_PATH=$PWD
export GCC_AARCH64_PREFIX=aarch64-linux-gnu-
source edksetup.sh
```

`iasl` is required. Install the ACPICA package, or download it and add its
`bin` directory to `PATH`:

```sh
export PATH=/path/to/acpica/usr/bin:$PATH
```

Copy the platform description and flash layout into the EDK2 tree:

```sh
cp /path/to/qemu/firmware/edk2/ArmVirtPkg/ArmVirtN2Ref.dsc \
   /path/to/qemu/firmware/edk2/ArmVirtPkg/ArmVirtN2Ref.fdf \
   ArmVirtPkg/
```

The `.dsc` sets the board-specific values:

- `PcdSerialRegisterStride = 4`
- `PcdSerialClockRate = 1843200`
- `PcdSerialBaudRate = 115200`
- `PcdDeviceTreeInitialBaseAddress = 0x80000000`
- `PcdPL031RtcBase` (filled in from the FDT by
  `ArmVirtPL031FdtClientLib`)
- `RealTimeClockRuntimeDxe` with the PL031 FDT client, instead of the
  PC/AT RTC (the board has no MC146818)

### 2.1 EDK2 source patches

Two DynamicTablesPkg patches are required. They are general fixes for
platforms whose UART is a 32-bit 16550 and whose CPU is a Neoverse-N2.

`DynamicTablesPkg/Library/FdtHwInfoParserLib/Arm/Gic/ArmGicCParser.c`:
add the N2 compatible string to `CpuCompatibleStr[]`:

```c
STATIC CONST COMPATIBILITY_STR  CpuCompatibleStr[] = {
  { "arm,arm-v7"      },
  { "arm,arm-v8"      },
  { "arm,armv8"       },
  { "arm,cortex-a15"  },
  { "arm,cortex-a7"   },
  { "arm,cortex-a57"  },
  { "arm,neoverse-n2" }
};
```

`DynamicTablesPkg/Library/FdtHwInfoParserLib/Serial/SerialPortParser.c`:
the DT `reg-io-width` is a byte count, while the ACPI GAS access size is an
encoding (`1=byte, 2=word, 3=dword, 4=qword`). Replace the direct copy with:

```c
      AccessSize = Fdt32ToCpu (*(UINT32 *)Data);
      switch (AccessSize) {
        case 1:
          AccessSize = EFI_ACPI_6_3_BYTE;
          break;
        case 2:
          AccessSize = EFI_ACPI_6_3_WORD;
          break;
        case 4:
          AccessSize = EFI_ACPI_6_3_DWORD;
          break;
        case 8:
          AccessSize = EFI_ACPI_6_3_QWORD;
          break;
        default:
          ASSERT (0);
          return EFI_INVALID_PARAMETER;
      }

      SerialPortInfo->AccessSize = AccessSize;
```

Without the second patch, the SPCR generator rejects the board UART because
`reg-io-width = <4>` is interpreted as a QWORD access.

Build:

```sh
build -a AARCH64 -t GCC -p ArmVirtPkg/ArmVirtN2Ref.dsc -b RELEASE -n "$(nproc)"
```

The BL33 image is:

```text
Build/ArmVirtN2Ref-AARCH64/RELEASE_GCC/FV/KVMTOOL_EFI.fd
```

A `DEBUG` build also works. It additionally needs the assert dead-loop
disabled, because the single-UART board makes the optional DBG2 table
generator return `EFI_NOT_FOUND`; set the DEBUG value in
`ArmVirtPkg/ArmVirt.dsc.inc` to `0x0f`:

```diff
 !if $(TARGET) == RELEASE
   gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x21
 !else
-  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2f
+  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x0f
 !endif
```

## 3. Build TF-A

```sh
git clone -b v2.15.0 https://git.trustedfirmware.org/TF-A/trusted-firmware-a.git
cp -a /path/to/qemu/firmware/atf/plat/n2ref trusted-firmware-a/plat/
cd trusted-firmware-a
make CROSS_COMPILE=aarch64-linux-gnu- \
    PLAT=n2ref \
    DEBUG=1 \
    ENABLE_TRF_FOR_NS=0 \
    ARM_LINUX_KERNEL_AS_BL33=1 \
    BL33=/path/to/edk2/Build/ArmVirtN2Ref-AARCH64/RELEASE_GCC/FV/KVMTOOL_EFI.fd \
    -j"$(nproc)" all
```

The output is:

```text
build/n2ref/debug/qemu_fw.bios
```

Use `DEBUG=0` for a release TF-A build (`build/n2ref/release/qemu_fw.bios`).

Why these options:

- `PLAT=n2ref` selects the port in `firmware/atf/plat/n2ref`. It reuses
  `plat/qemu/common` and overrides only the memory map, GIC and console.
- `ENABLE_TRF_FOR_NS=0`: QEMU's Neoverse-N2 advertises FEAT_TRF but does not
  implement `TRFCR_EL1`.
- `ARM_LINUX_KERNEL_AS_BL33=1`: BL33 is entered with `x0` pointing at the
  device tree at `0x8000_0000`. Without it EDK2's PrePi treats the PCIe ECAM
  address as the DTB and crashes.
- The port sets `GIC_ENABLE_V4_EXTN=1` and `GIC_EXT_INTID=1` because the board
  GIC reports PIDR2 architecture revision 4.

## 4. Boot

```sh
./build/qemu-system-aarch64 \
    -M n2-ref,secure=on \
    -cpu neoverse-n2 \
    -smp 1 \
    -m 2G \
    -bios /path/to/trusted-firmware-a/build/n2ref/debug/qemu_fw.bios \
    -display none \
    -serial file:/tmp/n2-ref.log
```

Use `-nographic` or `-serial stdio` to interact with the console. The boot
sequence is:

```text
NOTICE:  BL1: v2.15.0(...)
NOTICE:  BL2: v2.15.0(...)
NOTICE:  BL31: v2.15.0(...)
INFO:    GICv4 without legacy support detected.
INFO:    ARM GICv4 driver initialized in EL3
INFO:    Maximum SPI INTID supported: 287
UEFI firmware (version ...)
```

After the UEFI Boot Manager appears, select **Boot Manager** -> **UEFI Shell**
to get the `Shell>` prompt.

## 5. Current limitations

- **GIC-700** is modelled as QEMU's GICv4 (`revision=4`) plus an ITS. GICv4.1
  features (vSGI, `GICD_TYPER2`, VCONFADDR) are not emulated.
- **ACPI table generation is incomplete**: the DBG2 generator wants a second
  (debug) serial port, but the board has one UART, so it returns
  `EFI_NOT_FOUND` and the DynamicTableManager aborts the ACPI table
  installation. A RELEASE build continues to the UEFI Shell without ACPI
  tables. In a DEBUG build this failure is an assert, hence the
  `PcdDebugPropertyMask=0x0f` workaround above. A future fix is to drop the
  DBG2 table from the platform or expose a second UART.
- **PCIe ACPI**: the Xilinx AXI-PCIe host bridge is not enumerated by EDK2's
  generic `PciHostBridgeDxe` (it reports `Unsupported`), so no MCFG/PCI SSDT
  is produced. Linux should use the device tree for PCIe.
- **RTC**: the board provides a PL031 at `0x0C01_0000`. The PC/AT RTC driver
  is intentionally not included.
