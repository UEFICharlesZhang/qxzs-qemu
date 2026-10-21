@echo -off
fs0:\Image.efi console=ttyS0,115200 earlycon=uart8250,mmio32,0x0C000000 initrd=\initrd.img dtb=\n2-ref.dtb rdinit=/init
