# XDMA/PCIe 中断流程与中断风暴调试记录

日期：2026-10-21

平台：QEMU 11.1.2 `n2-ref`（Neoverse-N2 + xilinx AXI-PCIe host）

软件栈：ATF v2.13（`build/fvp/release`）+ EDK2 ArmSample UEFI + Linux 6.6
（使用栈内 `linux-system.axf` 预编译内核）

## 1. 结论摘要

- QEMU 侧 PCIe INTx → GIC **SPI 33（INTID 65）** 的直连是通的，GIC 能收到
  IRQ 65 并进入 `xilinx_pcie_intr_handler()`。
- PCIe 打开的 DTB 下，Linux 的 `xilinx-pcie` probe 会触发**中断风暴**：
  CPU 持续在中断上下文，RCU 每隔约 63 秒报 stall，probe 停在
  `xilinx_pcie_init_port()`。
- 风暴不是 GIC 连线错误，也不是默认 e1000e（`-nic none` 后仍然复现），
  而是栈内核 `pcie-xilinx` 驱动对 Xilinx INTx FIFO/IDR 的 **ack 不完整**。
- 使用 PCIe `disabled` 的 DTB 时，Linux 可以通过 UEFI/NVMe 正常启动到
  busybox `~ #`，说明 ATF/UEFI/NVMe/kernel 这条链本身是好的。

## 2. 测试环境与复现方法

### 2.1 QEMU 命令

```sh
-M n2-ref,secure=on,bootflash-size=8M,cx-wdt=on,cx-gpio=on,\
fvp-sysregs=on,fvp-smmu=on,fvp-mem-protect=on,highram-size=14G \
-cpu neoverse-n2 -smp 1 -m 2G \
-drive file=/tmp/n2ref-verify/nvme-linux.img,if=none,id=nvme0,format=raw \
-device nvme,serial=deadbeef,drive=nvme0,bus=pci.0 \
-bios atf_v2.7/build/fvp/release/bl1.bin \
-device loader,file=atf_v2.7/build/fvp/release/fip.bin,addr=0x00800000,force-raw=on
```

NVMe 测试盘（FAT32，64 MiB）内容：

| 文件 | 说明 |
|---|---|
| `\EFI\BOOT\BOOTAA64.EFI` | UEFI Shell |
| `\Image.efi` | 栈 1 的 Linux 6.6 EFI-stub 内核 |
| `\initrd.img` | busybox initramfs（2 MiB，含 `/init`） |
| `\n2-ref.dtb` | n2-ref 测试 DTB（PCIe 打开或关闭） |
| `\startup.nsh` | 用 EFI stub 直接加载内核，传 `initrd=` / `dtb=` / `rdinit=/init` |

`\startup.nsh` 内容：

```
@echo -off
fs0:\Image.efi console=ttyS0,115200 earlycon=uart8250,mmio32,0x0C000000 \
  initrd=\initrd.img dtb=\n2-ref.dtb rdinit=/init
```

### 2.2 两种 DTB

- **PCIe disabled**：栈内 `rdn2cfg1.dts`。Linux 启动到
  `Run /init as init process`，随后出现 busybox `~ #`。
- **PCIe enabled，无 GPIO，INTx 直连 SPI**：
  栈内 `rdn2cfg1-pcie-intx-nogpio.dts`。Linux 在 `xilinx-pcie` probe
  期间进入中断风暴。

### 2.3 关键日志

- `n2-ref-debug/stack2-linux3.log`：PCIe disabled，成功到 busybox shell。
- `n2-ref-debug/stack2-linux2.log`：PCIe enabled，中断风暴。
- `n2-ref-debug/stack2-pcie-nonic.log`：加 `-nic none` 去掉默认
  e1000e 后仍然风暴，排除 e1000e。

风暴日志特征：

```
PCIe Link is UP
rcu: INFO: rcu_sched detected stalls on CPUs/tasks:
...
xilinx_pcie_probe+0x17c/0x42c
xilinx_pcie_driver_init+0x18/0x20
```

`xilinx_pcie_probe+0x17c` 对应 `xilinx_pcie_init_port()` 中的
`pcie_read()`；CPU 实际是持续被 INTx 中断占用，probe 无法推进。

## 3. QEMU 模型侧中断路径

代码：`hw/pci-host/xilinx-pcie.c`

```
endpoint INTx 电平变化
  → xilinx_pcie_set_irq(pin, level)
       └─ xilinx_pcie_queue_intr()
            把 {pin, level, valid} 压入 16 项 FIFO
            设置 ROOTCFG_INTMASK_INTX
  → xilinx_pcie_update_intr()
       if (FIFO 非空) s->intr |= INTX
       level = !!(s->intr & s->intr_mask)
       qemu_set_irq(s->irq, level)     // interrupt_out
  → GIC SPI 33（level-high）
```

相关寄存器（root port config，ECAM 偏移）：

| 寄存器 | 偏移 | 语义 |
|---|---|---|
| `ROOTCFG_INTDEC` / IDR | `0x138` | pending；写 1 清 |
| `ROOTCFG_INTMASK` / IMR | `0x13c` | mask；写入后重新计算线电平 |
| `ROOTCFG_RPIFR1` | `0x158` | 读当前 FIFO 事件；写回 pop 一条 |
| `ROOTCFG_RPIFR2` | `0x15c` | MSI data |
| `ROOTCFG_RPSCR` | `0x148` | `BRIDGEEN` 等 |

`ROOTCFG_RPIFR1` 位定义（与驱动侧一致）：

| 位 | 含义 |
|---|---|
| 31 | `VALID` |
| 30 | `MSI` |
| 29 | `ASSERT`（1=assert，0=deassert） |
| 28:27 | INTx lane/pin |

**线拉低的必要条件**（模型语义）：

1. FIFO 为空；
2. IDR 中对应 pending 位被写 1 清掉；
3. IMR 没有屏蔽该位。

FIFO 非空时，`xilinx_pcie_update_intr()` 会重新置 `INTX`，即使 IDR 刚被清过。

## 4. 栈内核驱动处理流程

代码：`linux/drivers/pci/controller/pcie-xilinx.c`

### 4.1 初始化顺序

```c
xilinx_pcie_probe()
    xilinx_pcie_parse_dt()        /* devm_request_irq() 在这里 */
    xilinx_pcie_init_port()       /* 改 IMR、清 IDR、使能 bridge */
    xilinx_pcie_init_irq_domain() /* 这时 leg_domain/msi_domain 才建立 */
    pci_host_probe()              /* 枚举 */
```

`xilinx_pcie_init_port()` 的关键步骤：

```c
if (xilinx_pcie_link_up(pcie)) ...       /* 读 PSCR */
pcie_write(pcie, ~XILINX_PCIE_IDR_ALL_MASK, IMR);   /* IMR = 0 */
pcie_write(pcie, pcie_read(IDR) & IMR_ALL_MASK, IDR);
pcie_write(pcie, IMR_ENABLE_MASK, IMR);             /* IMR 使能 */
pcie_write(pcie, pcie_read(RPSC) | BEN, RPSC);      /* 使能 bridge */
```

### 4.2 中断 handler

```c
static irqreturn_t xilinx_pcie_intr_handler(int irq, void *data)
{
    xilinx_pcie_intx_ctrl_set(CTRL0, BIT0);      /* 写 0x0C005030 */

    val  = pcie_read(IDR);
    mask = pcie_read(IMR);
    status = val & mask;

    if (!status) {
        pr_info("chz: irq not valid");
        return IRQ_NONE;                          /* 不 ack */
    }

    if (status & (INTX | MSI)) {
        int retries = 100;

        do {
            val = pcie_read(RPIFR1);
            if (val & RPIFR1_VALID)
                break;
            cpu_relax();
        } while (retries--);

        if (!(val & RPIFR1_VALID)) {
            dev_warn(dev, "RP Intr FIFO1 read error\n");
            goto error;
        }

        /* 解 MSI 或 INTx pin */
        ...
        pcie_write(RPIFR1_ALL_MASK, RPIFR1);      /* pop 一条 */
        generic_handle_domain_irq(domain, val);
    }

error:
    xilinx_pcie_intx_ctrl_set(CTRL1, BIT0);      /* 写 0x0C005034 */
    pcie_write(status, IDR);                     /* 只清 status 的位 */
    return IRQ_HANDLED;
}
```

只有 `status` 里的错误位会做日志处理；其余路径见下一节。

## 5. 问题分析

### 5.1 已确认的问题

| # | 问题 | 影响 |
|---|---|---|
| 1 | `request_irq()` 早于 `init_port()` / `init_irq_domain()` | 初始化窗口内 handler 可运行；`leg_domain` 可能还是 NULL。`generic_handle_domain_irq(NULL, …)` 会回落到 `irq_default_domain`，存在误分发/无效调用风险 |
| 2 | `status == 0` 直接 `return IRQ_NONE` | 不弹 FIFO、不写 IDR；level 线保持高 → SPI 33 反复触发 |
| 3 | 每次中断只 pop 一条 FIFO | 模型只要 FIFO 非空就重新置 INTX；多条目时线持续高 |
| 4 | 不检查 bit 29 `ASSERT` | deassert 事件也被当成中断 dispatch，产生虚假中断 |
| 5 | 只把 `status`（`IDR & IMR`）写回 IDR | 被 mask 掉的 pending 位、读写之间新到的位不会被清 |
| 6 | FIFO 无效时 `goto error`，不 pop | 若 FIFO 仍有条目，模型继续保持 INTX |
| 7 | 入口/出口写 `0x0C005030/34` | direct-SPI 下 n2-ref 用 `cx-gpio` 空窗口，写是空操作；真正的 ack 是 FIFO+IDR |
| 8 | `IRQF_NO_THREAD` + GIC fasteoi，handler 期间不 mask | 源不拉低就立即重投；mask 也不能替代清源 |
| 9 | 到达 `error` 后仍返回 `IRQ_HANDLED` | 内核 spurious 检测不会禁用该 IRQ，风暴可以持续 |

### 5.2 为什么会导致风暴

1. 控制器把 INTx 的 **assert 和 deassert 都排进 FIFO**，并在 FIFO 非空时
   保持 `interrupt_out` 有效。
2. GIC SPI 33 是 **level-high**。handler 返回并 EOI 后，GIC 重新采样；
   线仍高就立刻再次投递。
3. 当前 handler 在以下任一情况下没有把线拉低：
   - `status == 0` 早退；
   - 一次只 pop 一条，FIFO 仍非空；
   - FIFO 无效路径没有 pop；
   - 只清 `status`，其余 IDR 位仍为 1。
4. 于是 GIC 反复投递，CPU 一直停在 handler，RCU kthread 得不到调度，
   出现 `rcu_sched detected stalls`，`xilinx_pcie_probe` 无法完成。

### 5.3 不能确定的部分

- 现场日志没有出现 `chz: irq not valid`，因此无法仅凭日志确认第一次风暴
  一定走的是 `status == 0` 分支；也可能是 FIFO 多条目/部分 IDR 位导致的
  循环。无论哪条路径，handler 的 ack 都不完整，结论不变。
- 使用的内核是 `linux-system.axf` 内嵌的 9 月 18 日构建；源码树是 9 月 22
  日版本，个别行号/打印可能略有差异。行号以当前源码树为准。

## 6. 与普通 GPIO 电平中断的异同

### 6.1 相同点

- 对 GIC 都是 level 触发；GIC 的 EOI 只 deactivate，不会清源端。
- handler 必须完成**源端 ack**；只 EOI GIC 不够。
- 源端未清 → GIC 反复投递 → 风暴。普通 GPIO 如果 handler 忘了写状态
  清除寄存器，也会出现同样的现象。

### 6.2 不同点

| 项目 | 普通 GPIO 电平中断 | 当前 PCIe direct-SPI |
|---|---|---|
| 电平来源 | GPIO 控制器状态/锁存 | xilinx-pcie 的 `interrupt_out`，由 IDR/IMR + FIFO 决定 |
| 事件语义 | 纯电平 | FIFO 同时记录 assert/deassert |
| 源端 ack | 一次寄存器写（清状态） | 两步：弹 RPIFR FIFO + 清 IDR |
| 分发 | GPIO IRQ domain → 子设备 | INTx domain → 设备 INTx handler |
| 线拉低条件 | 清掉锁存即可 | FIFO 空 **且** IDR 清 **且** IMR 不屏蔽 |

### 6.3 kernel 会不会先 mask GPIO 中断

- GPIO 电平中断通常走 `handle_level_irq`：handler 前 `mask_ack_irq()`，
  处理完 `unmask`。这只能防止 handler 执行期间重入；**unmask 后如果
  GPIO 线仍高，GIC 会立刻重新投递**。
- GICv3 的 SPI 走 `handle_fasteoi_irq`，默认不在 handler 前后 mask，
  只做 EOI/deactivate；线仍高就立即重投。
- 线程化/`IRQF_ONESHOT` 会在线程运行期间保持 mask，线程清源后才 unmask；
  如果线程也不清源，unmask 后仍会重投。
- 因此 mask 不能替代清源。当前 PCIe 驱动是 `IRQF_NO_THREAD` + fasteoi，
  本来就不 mask；即使 mask，xilinx 的 `interrupt_out` 仍由 FIFO/IDR
  保持，unmask 后线还是高。

## 7. 修复建议（软件层面）

### 7.1 调整初始化顺序

把 `devm_request_irq()` 从 `xilinx_pcie_parse_dt()` 移到
`xilinx_pcie_init_port()` 和 `xilinx_pcie_init_irq_domain()` 之后：

```c
/* parse_dt 只解析 IRQ */
pcie->irq = irq_of_parse_and_map(node, 0);

/* probe */
xilinx_pcie_init_port(pcie);
xilinx_pcie_init_irq_domain(pcie);
devm_request_irq(dev, pcie->irq, xilinx_pcie_intr_handler,
                 IRQF_SHARED | IRQF_NO_THREAD, "xilinx-pcie", pcie);
pci_host_probe(bridge);
```

这样 handler 不会在 IMR 重编程窗口或 domain 未建立时运行。

### 7.2 完整 ack handler

```c
#define XILINX_PCIE_RPIFR1_INTR_ASSERT  BIT(29)   /* 新增 */

static irqreturn_t xilinx_pcie_intr_handler(int irq, void *data)
{
    struct xilinx_pcie *pcie = data;
    u32 idr, imr, status, fifo;
    bool handled = false;
    int i;

    xilinx_pcie_intx_ctrl_set(pcie, CTRL0_OFF, BIT0);

    idr = pcie_read(pcie, XILINX_PCIE_REG_IDR);
    imr = pcie_read(pcie, XILINX_PCIE_REG_IMR);
    status = idr & imr;

    /* 原有错误位日志，使用 status */

    /*
     * 无条件排空 FIFO：status == 0 也要做。
     * 每条 assert 都 dispatch；deassert 只弹出。
     */
    for (i = 0; i < 32; i++) {
        fifo = pcie_read(pcie, XILINX_PCIE_REG_RPIFR1);
        if (!(fifo & XILINX_PCIE_RPIFR1_INTR_VALID))
            break;

        pcie_write(pcie, XILINX_PCIE_RPIFR1_ALL_MASK,
                   XILINX_PCIE_REG_RPIFR1);
        handled = true;

        if (!(fifo & XILINX_PCIE_RPIFR1_INTR_ASSERT))
            continue;

        if (fifo & XILINX_PCIE_RPIFR1_MSI_INTR) {
            u32 msg = pcie_read(pcie, XILINX_PCIE_REG_RPIFR2) &
                      XILINX_PCIE_RPIFR2_MSG_DATA;
            if (pcie->msi_domain)
                generic_handle_domain_irq(pcie->msi_domain->parent, msg);
        } else {
            u32 pin = (fifo & XILINX_PCIE_RPIFR1_INTR_MASK) >>
                      XILINX_PCIE_RPIFR1_INTR_SHIFT;
            if (pcie->leg_domain)
                generic_handle_domain_irq(pcie->leg_domain, pin);
        }
    }

    xilinx_pcie_intx_ctrl_set(pcie, CTRL1_OFF, BIT0);

    /*
     * 清所有当前 pending 位。重新读一次，避免把读和写之间
     * 新到的位漏掉。
     */
    pcie_write(pcie, pcie_read(pcie, XILINX_PCIE_REG_IDR),
               XILINX_PCIE_REG_IDR);

    return (status || handled) ? IRQ_HANDLED : IRQ_NONE;
}
```

要点：

- `status == 0` 也要排空 FIFO 并清 IDR。
- 一次处理完所有 FIFO 条目；循环上限 32（FIFO 深度 16）。
- 检查 `ASSERT` 位；deassert 不 dispatch。
- 清 IDR 用“重新读当前值再写回”，不要无脑写 `0xFFFFFFFF`，避免丢掉
  新到的 error/status 位。
- 不再依赖 `0x0C005030/34` 的 GPIO ack；direct-SPI 下真正的 ack 是
  FIFO + IDR。

## 8. 验证方法

1. 修改并重新编译栈内核。
2. 把新 `Image` 作为 `\Image.efi` 放回 NVMe 测试镜像。
3. 用 PCIe enabled 的 DTB 启动，预期：
   - `xilinx-pcie` probe 完成，不再 RCU stall；
   - 出现 NVMe 设备（如 `nvme nvme0: ...`）；
   - `/proc/interrupts` 中 IRQ 65（SPI 33）有计数；
   - 不再出现 `rcu_sched detected stalls`。
4. 回归 PCIe disabled 配置，确认仍能到 busybox `~ #`。

## 9. 附录：关键地址与代码位置

### 9.1 QEMU

- `hw/arm/n2-ref.c`：`n2_ref_create_pcie()`，
  `interrupt_out → qdev_get_gpio_in(gic, 33)`。
- `hw/arm/n2-ref.c`：FDT PCIe 节点，`interrupts = <SPI 33 LEVEL_HI>`，
  `interrupt-map` 四个 pin 均指向 SPI 33；不再生成 `msi-map`。
- `hw/pci-host/xilinx-pcie.c`：`xilinx_pcie_update_intr()`、
  `xilinx_pcie_queue_intr()`、`xilinx_pcie_set_irq()`、
  root config read/write。
- 平台 compat 选项：`cx-gpio=on` 在 `0x0C005000` 映射 4 KiB 空窗口，
  只用于避免 `0x0C005030/34` 访问中止，不参与 ack。

### 9.2 栈内核

- `drivers/pci/controller/pcie-xilinx.c`
  - `xilinx_pcie_intr_handler()`（约 line 385）
  - `xilinx_pcie_init_port()`（约 line 576）
  - `xilinx_pcie_parse_dt()`（约 line 615，`request_irq` 在 637 附近）
  - `xilinx_pcie_probe()`（约 line 655）
- 寄存器常量：
  - `IDR 0x138`、`IMR 0x13c`、`RPIFR1 0x158`、`RPIFR2 0x15c`
  - `RPIFR1_INTR_VALID BIT(31)`、`RPIFR1_MSI_INTR BIT(30)`、
    `RPIFR1_INTR_MASK GENMASK(28, 27)`
  - `ASSERT = BIT(29)`（当前未定义）
  - `XILINX_PCIE_INTX_CTRL_PHYS 0x0C005030`

### 9.3 测试产物

- `/tmp/n2ref-verify/nvme-linux.img`
- `n2-ref-debug/n2-ref-efi.dtb`（PCIe enabled）
- `n2-ref-debug/n2-ref-nopcie.dtb`（PCIe disabled）
- `n2-ref-debug/startup.nsh`
- `n2-ref-debug/stack2-linux3.log`（成功启动到 `~ #`）
- `n2-ref-debug/stack2-linux2.log`（PCIe 中断风暴）
- `n2-ref-debug/stack2-pcie-nonic.log`（`-nic none` 仍风暴）
- `n2-ref-debug/stack2-linux.log`、`n2-ref-debug/stack2-nvme2.log`（早期 bring-up）
- `n2-ref-debug/README.md`（测试镜像重建方法和 QEMU 命令）

NVMe 测试镜像（`nvme-linux.img`、`nvme-scratch.img`）是生成物，未纳入仓库；
重建方法见 `n2-ref-debug/README.md`。

## 10. 修复验证结果（2026-10-21）

已按第 7 节修改栈内
`drivers/pci/controller/pcie-xilinx.c`：

- `devm_request_irq()` 从 `xilinx_pcie_parse_dt()` 移到
  `xilinx_pcie_init_port()` + `xilinx_pcie_init_irq_domain()` 之后；
- handler 无条件排空 RPIFR1 FIFO，检查 bit29 `ASSERT`，只对 assert
  事件 dispatch；
- 排空后重新读 IDR 并写回，清掉所有当前 pending 位；
- 去掉 `xilinx_pcie_intx_ctrl_set()` 里的 `mdelay(3)`，避免 hardirq
  中每次中断 6 ms 的延迟；
- 新增 `XILINX_PCIE_RPIFR1_INTR_ASSERT BIT(29)`，移除未使用的
  `irq_cnt`。

构建：

```sh
make -C linux O=linux/build ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- Image -j"$(nproc)"
```

验证日志：`n2-ref-debug/stack2-fixed.log`

结果：

- `rcu_sched detected stalls` 数量为 **0**；
- `xilinx-pcie` probe 完成，`PCIe Link is UP`；
- NVMe 正常枚举，MSI/MSI-X 不可用后回退到 legacy INTx：
  `pci: using legacy INTx, irq=14`；
- `nvme0n1` 出现，`Run /init as init process`，进入 busybox `~ #`。

补丁：`n2-ref-debug/pcie-xilinx-intx-fix.patch`
（相对栈内当前已 staged 的版本，即不含之前 staged 的 GPIO/print 改动）。

结论：中断风暴由驱动 ack 不完整导致；按上述方式修复后，direct-SPI
的 INTx 路径可以稳定工作。

## 11. NVMe 读写验证（2026-10-21）

在启动盘之外额外挂载一个 32 MiB 的 scratch NVMe，避免写坏启动盘：

```sh
-drive file=/tmp/n2ref-verify/nvme-linux.img,if=none,id=nvme0,format=raw \
-device nvme,serial=boot,drive=nvme0,bus=pci.0 \
-drive file=/tmp/n2ref-verify/nvme-scratch.img,if=none,id=nvme1,format=raw \
-device nvme,serial=scratch,drive=nvme1,bus=pci.0
```

guest 中通过 `/sys/block/<dev>/size` 选择 32 MiB 的 `nvme1n1` 作为
scratch，测试结果：

| 测试 | 结果 |
|---|---|
| 64 KiB 随机数据写 → 读回 | md5 一致，`CMP_OK` |
| 4 MiB 随机数据写 → 读回 | md5 一致，`BIG_CMP_OK` |
| 整盘 32 MiB 读取 | `512+0 records`，约 43 MB/s |
| 主机侧 scratch 前 4 MiB | 4,178,049 字节非零，确认数据落盘 |
| `/proc/interrupts` | `14: 679 GICv3 65 Level xilinx-pcie, nvme0q0, nvme0q1, nvme1q0, nvme1q1` |
| RCU stall / panic | 无 |

说明：两个 NVMe 控制器共用 root port 的 INTx 线，`/proc/interrupts`
中的 IRQ 14 最终对应 GIC SPI 33（IRQ 65）；679 次中断说明 INTx 在
I/O 负载下工作正常。

日志：`n2-ref-debug/nvme-rw-serial.log`
