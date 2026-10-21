# n2-ref quick start

How to build QEMU for the `n2-ref` board and boot code on it, from a bare-metal
smoke test up to EL3 firmware.

All commands below are run from the QEMU source root unless stated otherwise.

## 1. Build QEMU

```sh
mkdir -p build
cd build
../configure --target-list=aarch64-softmmu
make -j"$(nproc)"
cd ..
```

The binary is `build/qemu-system-aarch64`.

If this tree is already configured, just run `make -C build -j"$(nproc)"`.

## 2. Board at a glance

| Region | Address | Size |
|---|---|---|
| Boot RAM (`secure=on` or `-bios`) | `0x0000_0000` | 4 MiB |
| Secure SRAM (`secure=on`) | `0x0400_0000` | 1 MiB |
| Non-secure SRAM (`secure=on`) | `0x0600_0000` | 512 KiB |
| 8250 UART | `0x0C00_0000` | 256 B (reg-shift 2) |
| PL031 RTC | `0x0C01_0000` | 4 KiB |
| GIC distributor | `0x0E00_0000` | 64 KiB |
| GIC ITS | `0x0E04_0000` | 128 KiB |
| GIC redistributors | `0x0E08_0000` | 2 MiB |
| PCIe ECAM | `0x4000_0000` | 256 MiB |
| PCIe MMIO/BAR window | `0x6000_0000` | 512 MiB |
| RAM | `0x8000_0000` | 2 GiB |
| High DRAM (opt-in via `highram-size`) | `0x9_0000_0000` | configured size |
| Boot flash (opt-in via `bootflash-size`) | `0x0080_0000` | configured size |

Interrupts: UART INTID 33 (SPI 1), PL031 RTC INTID 34 (SPI 2), PCIe INTx
INTID 65 (SPI 33). PCIe endpoints are wired through the xilinx-pcie
`interrupt_out` line to that SPI; the device tree does not advertise an MSI
map, so guests fall back to INTx instead of MSI/MSI-X. The PL031 is included
for UEFI runtime services, which
require an RTC arch protocol; it also gives Linux a working `/dev/rtc0`.

Limits: `-m` is at most 2 GiB and `-smp` at most 8. The default CPU is
`neoverse-n2`.

The reference platform's high DRAM window at `0x9_0000_0000` is opt-in
because it lies outside the 32-bit address space used by the low RAM and all
MMIO. Use `-M n2-ref,highram-size=14G` to map it. It is intended for firmware
that supplies its own device tree (the reference platform DTS describes the
window); QEMU's generated DT only covers the 2 GiB at `0x8000_0000`.

For firmware derived from the FVP/CX ports, `-M n2-ref,bootflash-size=8M` maps
a RAM-backed flash window at `0x0080_0000` (where their BL1 looks for the FIP),
`cx-wdt=on` maps a 4 KiB no-op watchdog at `0x0C00A000`, `cx-gpio=on` maps
the 4 KiB CX GPIO/INTx control block at `0x0C005000`, `fvp-sysregs=on`
maps a 4 KiB no-op system-register block at `0x1C010000`, `fvp-smmu=on`
maps a 64 KiB no-op SMMUv3 block at `0x0D000000`, and `fvp-mem-protect=on`
maps the flash memory-protect flag at `0x07FC0000` (reads as zero). The flash
window must stay below the secure SRAM at `0x0400_0000` (at most 56 MiB).

Two boot modes:

| Machine option | CPU entry | PSCI | GIC |
|---|---|---|---|
| `-M n2-ref` | EL2 | QEMU's PSCI, SMC conduit | single security state |
| `-M n2-ref,secure=on` | EL3 | firmware owns PSCI | secure + non-secure |

## 3. A minimal payload

Create `/tmp/n2-ref-demo/hello.S`:

```asm
/*
 * Writes "n2-ref: hello from EL<n>\n" to the 8250 UART at 0x0C00_0000.
 * The 16550 uses 32-bit registers (reg-shift 2); the data register is at
 * offset 0, so a byte store to the UART base works.
 */
    .section .text
    .global _start
_start:
    movz    x0, #0x0c00, lsl #16        /* x0 = 0x0C000000 */

    adr     x2, msg
1:  ldrb    w3, [x2], #1
    cbz     w3, 2f
    strb    w3, [x0]
    b       1b

2:  mrs     x1, CurrentEL
    lsr     x1, x1, #2
    add     w1, w1, #'0'
    strb    w1, [x0]

    mov     w3, #'\n'
    strb    w3, [x0]

3:  b       3b

msg:
    .asciz  "n2-ref: hello from EL"
```

Build it twice: once linked at `0x8000_0000` for direct boot, once linked at
`0x0` for the firmware reset vector.

```sh
mkdir -p /tmp/n2-ref-demo
cd /tmp/n2-ref-demo
CROSS=aarch64-linux-gnu-

# Direct-boot image (loaded by -kernel at the start of RAM).
${CROSS}as -o hello-el2.o hello.S
${CROSS}ld -Ttext=0x80000000 -o hello-el2.elf hello-el2.o
${CROSS}objcopy -O binary hello-el2.elf hello-el2.bin

# Firmware image (loaded by -bios at address 0).
${CROSS}as -o hello-el3.o hello.S
${CROSS}ld -Ttext=0x0 -o hello-el3.elf hello-el3.o
${CROSS}objcopy -O binary hello-el3.elf hello-el3.bin

cd -
```

Adjust `CROSS` if your toolchain uses a different prefix
(`aarch64-none-elf-`, for example).

## 4. Boot at EL2

`-kernel` with a raw binary is classified as a Linux image, so it enters at
EL2 under QEMU's PSCI:

```sh
./build/qemu-system-aarch64 \
    -M n2-ref -cpu neoverse-n2 -smp 1 -m 512M \
    -nographic -kernel /tmp/n2-ref-demo/hello-el2.bin
```

Expected output:

```text
n2-ref: hello from EL2
```

`-cpu neoverse-n2` is optional, it is the board default. `-nographic` routes
the 8250 to stdio.

## 5. Boot at EL3 (firmware / ATF mode)

Firmware enters at the AArch64 reset vector, `0x0`, which `-bios` populates:

```sh
./build/qemu-system-aarch64 \
    -M n2-ref,secure=on -cpu neoverse-n2 -smp 1 -m 512M \
    -nographic -bios /tmp/n2-ref-demo/hello-el3.bin
```

Expected output:

```text
n2-ref: hello from EL3
```

What `secure=on` changes:

- CPUs get `has_el3=true`; the guest starts at EL3.
- GIC gets security extensions (`GICD_CTLR.DS=0`, `GICD_TYPER.SecurityExtn=1`).
- QEMU's PSCI is disabled. The generated device tree has no `/psci` node and
  no `enable-method`, because firmware is expected to supply PSCI.
- 4 MiB of boot RAM is mapped at `0x0`, plus the two TF-A SRAM windows at
  `0x0400_0000` and `0x0600_0000`.
- Secure mode requires TCG; it refuses to start with KVM.

`-bios` also works without `secure=on`; the image then starts at EL2 with
QEMU's PSCI:

```sh
./build/qemu-system-aarch64 \
    -M n2-ref -smp 1 -m 512M -nographic \
    -bios /tmp/n2-ref-demo/hello-el3.bin
```

Expected output:

```text
n2-ref: hello from EL2
```

Passing both `-bios` and `-kernel` is rejected: this board has no fw_cfg to
hand a kernel to firmware.

## 6. Boot a Linux kernel directly

Non-secure mode can boot a Linux `Image` without firmware. The board supplies
the device tree; `-append` becomes `/chosen/bootargs`.

```sh
./build/qemu-system-aarch64 \
    -M n2-ref -cpu neoverse-n2 -smp 4 -m 2G \
    -nographic \
    -kernel /path/to/Image \
    -initrd /path/to/rootfs.cpio.gz \
    -append "console=ttyS0 earlycon=uart8250,mmio32,0x0C000000"
```

Use `earlycon=uart8250,mmio32,...` (not `mmio`) because the UART has 32-bit
registers. Do not add `secure=on` for direct Linux boot: without firmware there
is nothing to supply PSCI.

## 7. Boot TF-A + UEFI

The board is tested with TF-A v2.15.0 and EDK2 `edk2-stable202608`. The port
and the full build recipe live in [`firmware/README.md`](firmware/README.md).

The firmware sources shipped with this tree are:

- `firmware/atf/plat/n2ref` - TF-A platform port (copy to `plat/n2ref`)
- `firmware/edk2/ArmVirtPkg/ArmVirtN2Ref.dsc` and `.fdf` - EDK2 platform

Build TF-A with the EDK2 BL33 image:

```sh
make -C trusted-firmware-a \
    CROSS_COMPILE=aarch64-linux-gnu- \
    PLAT=n2ref DEBUG=1 \
    ENABLE_TRF_FOR_NS=0 \
    ARM_LINUX_KERNEL_AS_BL33=1 \
    BL33=/path/to/edk2/Build/ArmVirtN2Ref-AARCH64/RELEASE_GCC/FV/KVMTOOL_EFI.fd \
    -j"$(nproc)" all
```

Boot it:

```sh
./build/qemu-system-aarch64 \
    -M n2-ref,secure=on -cpu neoverse-n2 -smp 1 -m 2G \
    -bios /path/to/trusted-firmware-a/build/n2ref/debug/qemu_fw.bios \
    -display none -serial stdio
```

`-m 2G` is required: TF-A loads BL33 at `0xA000_0000`. At the UEFI Boot
Manager choose **Boot Manager** -> **UEFI Shell** to reach `Shell>`.

The device tree QEMU places at `0x8000_0000` describes the non-secure world;
TF-A passes it to BL33.

## 8. Inspect the generated device tree

```sh
./build/qemu-system-aarch64 \
    -M n2-ref,dumpdtb=/tmp/n2-ref.dtb -m 512M \
    -display none -serial none
dtc -I dtb -O dts /tmp/n2-ref.dtb
```

With `secure=on`, the `/psci` node and every `enable-method` are absent, as
described above.

`refer-sys.dts` in this tree is the reference platform description used to
check the address map; it is not a boot DTB.

## 9. Tests

```sh
cd build
QTEST_QEMU_BINARY=$PWD/qemu-system-aarch64 ./tests/qtest/n2-ref-test
QTEST_QEMU_BINARY=$PWD/qemu-system-aarch64 \
    ./tests/qtest/boot-serial-test -p /aarch64/boot-serial/n2-ref
make check-qtest-aarch64
```

## 10. Troubleshooting

- **Nothing on serial.** The console is an 8250 at `0x0C00_0000`, not a PL011
  at `0x0900_0000`. Use 32-bit register accesses (`reg-shift = 2`).
- **The EL3 payload prints `2`.** Add `secure=on`. A raw `-kernel` image always
  enters at EL2; use `-bios` for the EL3 reset vector.
- **Nothing runs from `-bios`.** The image is copied to `0x0` and must be
  linked for that address. It must also fit in the 4 MiB boot RAM, otherwise
  QEMU exits with a load error.
- **KVM refuses to start in secure mode.** Expected: KVM cannot provide EL3.
- **PCIe ECAM is 256 MiB.** That is QEMU's `pcie_host_mmcfg_init()` limit, so
  the reference's larger ECAM cannot be modelled.
- **Firmware does not see a PCIe endpoint.** `-device` endpoints default to
  the bus in front of the xilinx root port (`pcie0`). Firmware that scans
  behind the root port, such as UEFI, needs them on the downstream bus:
  `-device nvme,serial=...,drive=...,bus=pci.0`.
- **The GIC identifies as a GIC-500.** GIC-700 is approximated with GICv3
  revision 4 plus ITS support; `GICD_IIDR` is not a GIC-700 value.
