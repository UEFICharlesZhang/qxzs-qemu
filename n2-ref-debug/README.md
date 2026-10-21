# n2-ref PCIe INTx debug artifacts

Captured 2026-10-21 while debugging the `xilinx-pcie` INTx interrupt storm
on the QEMU `n2-ref` machine.

## Kernel fix

- Kernel tree: `/home/charles/nvme2t/code/qxzs_001/arm-boot-flow-zh/code/linux`
- File: `drivers/pci/controller/pcie-xilinx.c`
- Branch: `n2-ref-intx-fix`
- Commit: `c75a289468f1`
- Build:

```sh
make -C linux O=linux/build ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- Image -j"$(nproc)"
```

Build output: `linux/build/arch/arm64/boot/Image`

## Files

| File | Description |
|---|---|
| `stack2-linux.log` | PCIe enabled, before the `cx-gpio` stub: data abort in the handler |
| `stack2-linux2.log` | PCIe enabled, after `cx-gpio`: interrupt storm / RCU stalls |
| `stack2-pcie-nonic.log` | Same with `-nic none`: storm persists, not the e1000e |
| `stack2-linux3.log` | PCIe disabled: Linux boots to the busybox shell |
| `stack2-fixed.log` | Fixed kernel, PCIe enabled: NVMe enumerated, busybox `~ #` |
| `stack2-nvme2.log` | UEFI Shell with the NVMe disk mapped as `FS0:` |
| `nvme-rw-serial.log` | NVMe read/write test output |
| `pcie-xilinx-intx-fix.patch` | Incremental driver fix relative to the previously staged state |
| `n2-ref-efi.dtb` | Test DTB, PCIe enabled, INTx direct to SPI 33 |
| `n2-ref-nopcie.dtb` | Test DTB, PCIe disabled |
| `startup.nsh` | UEFI Shell script loading the EFI-stub kernel with `dtb=` / `initrd=` |
| `nvme_rw_test.exp` | expect script used for the NVMe read/write test |

The kernel commit contains the full driver change (the previously staged
GPIO/print changes plus the INTx ack fix).  The patch file is only the
incremental fix on top of that staged state.

## NVMe test images (generated, not stored)

The NVMe images are large generated artifacts and are not kept in this
directory.  Recreate them as follows.

Boot image (64 MiB FAT32):

```sh
qemu-img create -f raw nvme-linux.img 64M
mkfs.vfat -F 32 -n NVMELINUX nvme-linux.img
mmd -i nvme-linux.img ::/EFI ::/EFI/BOOT
mcopy -i nvme-linux.img <UEFI Shell.efi> ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i nvme-linux.img <fixed Image>  ::/Image.efi
mcopy -i nvme-linux.img <ramdisk-busybox.img> ::/initrd.img
mcopy -i nvme-linux.img n2-ref-efi.dtb ::/n2-ref.dtb
mcopy -i nvme-linux.img startup.nsh   ::/startup.nsh
```

Scratch image used by the read/write test:

```sh
qemu-img create -f raw nvme-scratch.img 32M
```

## QEMU command

```sh
-M n2-ref,secure=on,bootflash-size=8M,cx-wdt=on,cx-gpio=on,\
fvp-sysregs=on,fvp-smmu=on,fvp-mem-protect=on,highram-size=14G \
-cpu neoverse-n2 -smp 1 -m 2G \
-serial unix:/tmp/n2ref-verify/serial.sock,server=on,wait=off \
-nic none \
-drive file=nvme-linux.img,if=none,id=nvme0,format=raw \
-device nvme,serial=boot,drive=nvme0,bus=pci.0 \
-drive file=nvme-scratch.img,if=none,id=nvme1,format=raw \
-device nvme,serial=scratch,drive=nvme1,bus=pci.0 \
-bios <ATF>/bl1.bin \
-device loader,file=<ATF>/fip.bin,addr=0x00800000,force-raw=on
```

The `startup.nsh` command uses `fs0:\Image.efi`; the UEFI Shell maps the
boot FAT partition as `FS0`.
