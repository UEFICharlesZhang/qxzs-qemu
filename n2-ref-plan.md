# Implementation plan: `n2-ref` — Neoverse-N2 + GIC-700 + MIG + 8250 + Xilinx AXI-PCIe

Status: proposed (not implemented). Target tree: QEMU 11.1.2, `aarch64-softmmu`.

## 0. Memory map status

The map is settled; there are no open conflicts.

- UART base is **`0x0c000000`** (8 hex digits = **`0x0C000000`** = 192 MiB), below
  RAM and below PCIe ECAM.
- GIC bases are `gicd=0x0E000000`, `its=0x0E040000`, `gicr=0x0E080000`. These are
  non-overlapping and 64 KiB-aligned (see §2).
- PCIe BAR window base is **`0x60000000`** and it ends at the RAM base, so the
  window is 512 MiB (see §2).
- RAM is one **memory region starting at `0x80000000`**, 2 GiB. Whether the
  guest calls that region "DDR", "MIG" or anything else is out of scope: QEMU
  maps `machine->ram` there and does not model a memory-controller register
  block (§4.6).

---

## 1. What already exists in this tree

| Requested component | Closest existing QEMU model | Gap |
|---|---|---|
| Neoverse-N2 CPU | `neoverse-n2` in `target/arm/tcg/cpu64.c` (`aarch64_neoverse_n2_initfn`, MIDR `0x410FD493`, TCG-only) | None. It is already the default CPU of `sbsa-ref`. |
| GIC-700 | `arm-gicv3` with `revision=4` (GICv4), plus `arm-gicv3-its` | GIC-700 is **GICv4.1**. QEMU has no vSGI/VCONFADDR/GICD_TYPER2 (see `hw/intc/arm_gicv3_dist.c`; `GICD_TYPER2` is read-zero with a comment that v4.1 is not emulated). GICv5 `x-5` is a different architecture, not GIC-700. |
| MIG (Xilinx DDR memory interface) | `hw/misc/xlnx-zynq-ddrc.c` only — Zynq DDRC, a different IP | **No MIG model.** The MIG is represented by the RAM region at `0x80000000`, so no device is needed (§4.6). A real register model is optional later work. |
| 8250 UART | `hw/char/serial-mm.c` (`TYPE_SERIAL_MM`, 16550A), created via `serial_mm_init()` | None functionally; no auto-map, board must call `serial_mm_init()`. |
| Xilinx AXI-PCIe host bridge ("XDMA") | `hw/pci-host/xilinx-pcie.c` (`TYPE_XILINX_PCIE_HOST`, `"xlnx,axi-pcie-host-1.00.a"`) | **Exact match, used as-is.** This is the AXI-to-PCIe root complex, not the XDMA DMA engine. `aspeed_xdma.c` is unrelated. Limits: no 64-bit BAR window, no built-in MSI controller, one INTx output. |

So the work is a **new board only**: every requested component maps onto an
existing device model. The MIG is just the RAM region at `0x80000000`, so it
needs no device at all (§4.6); no new device models are required for this
scope.

---

## 2. Target memory map

```
0x0000_0000  (unmapped; no reset vector — see §9 on -bios)
0x0C00_0000  8250 UART       0x0000_0008   (serial-mm maps 8 bytes at regshift=0)
0x0E00_0000  GICD            0x0001_0000   (64 KiB)
0x0E04_0000  GITS            0x0002_0000   (128 KiB)
0x0E08_0000  GICR            0x0020_0000   (2 MiB = 8 × 0x40000 GICv4 frames)
0x0E28_0000  (end of GICR; free)
0x4000_0000  PCIe ECAM       0x1000_0000   (256 MiB = 256 buses, max allowed)
0x6000_0000  PCIe BAR window 0x2000_0000   (512 MiB; ends exactly at RAM)
0x8000_0000  RAM             0x8000_0000   (2 GiB; ends at 0x1_0000_0000)
```

The GIC block sits at `0x0E00_0000`, above the UART (`0x0C00_0000`) and below
PCIe ECAM (`0x4000_0000`). Each region is 64 KiB-aligned:

- GICD `0x0E00_0000` + `0x1_0000` → `0x0E01_0000`
- GITS `0x0E04_0000` + `0x2_0000` → `0x0E06_0000`
- GICR `0x0E08_0000` + `0x20_0000` → `0x0E28_0000`

The gap between GICD and GITS (`0x0E01_0000`–`0x0E03_FFFF`, 192 KiB) and the gap
between GITS and GICR (`0x0E06_0000`–`0x0E07_FFFF`, 128 KiB) are unused and
harmless. The ITS frame size is
`ITS_SIZE = ITS_CONTROL_SIZE + ITS_TRANS_SIZE = 0x20000`
(`include/hw/intc/arm_gicv3_its_common.h`).

ECAM ends at `0x5000_0000` and the BAR window starts at `0x6000_0000`, leaving a
256 MiB free gap (`0x5000_0000`–`0x5FFF_FFFF`). That is deliberate headroom, not
an overlap: the BAR window is sized to end exactly at the RAM base.

**No separate MIG register window.** The MIG is the RAM region at `0x80000000`
(see §0), so it is represented by `machine->ram` itself, not by a device. There
is therefore no MIG SPI below; SPI 2 is left reserved and unused.

Interrupt map. The board spec numbers lines by **INTID**; the FDT and the GIC
qdev input lines are both indexed by **SPI** (`INTID - 32`):

| SPI | INTID | Device |
|---|---|---|
| 1 | 33 | 8250 UART |
| 2 | 34 | (reserved; unused) |
| 33 | 65 | Xilinx PCIe `interrupt_out` (single shared INTx line) |

SPIs 3–32 are left unused. The PCIe line is **INTID 65** (SPI 33, `0x21`); in
the FDT it appears as `<GIC_SPI 33 ...>` and in C as
`qdev_get_gpio_in(s->gic, 33)`. Both encodings are SPI-based, so writing `65`
in either place silently selects INTID 97 instead.

`num-irq = 256 + 32 = 288` (must be a multiple of 32). That covers SPI 0–255,
so SPI 33 is in range with plenty of headroom.

Rationale for the PCIe window sizes:

- `pcie_host_mmcfg_init()` **asserts** ECAM size is a power of two, `>= 1 MiB`,
  `<= 256 MiB` (`include/hw/pci/pcie_host.h`). 256 MiB is the largest legal
  value and is exactly 256 buses.
- `xilinx-pcie` programs the root bridge's 32-bit memory base/limit from
  `mmio_base`/`mmio_size`, truncated to 16 bits of 64 KiB units. With
  `mmio_base=0x60000000` and `mmio_size=0x20000000`, base=`0x6000`,
  limit=`0x7FF0`, i.e. the window ends at `0x80000000`.
- `mmio_base` must be 64 KiB-aligned and `mmio_base + mmio_size <= 4 GiB`.

---

## 3. Files to add / modify

New:

- `hw/arm/n2-ref.c` — the machine
- `tests/qtest/n2-ref-test.c` — device-level qtest
- `docs/system/arm/n2-ref.rst` — board documentation

Modified:

- `hw/arm/meson.build` — one `arm_common_ss.add(when: 'CONFIG_N2_REF', ...)` line
- `hw/arm/Kconfig` — `config N2_REF`
- `tests/qtest/meson.build` — conditional entry in `qtests_aarch64`
- `docs/system/target-arm.rst` — TOC entry (add `arm/n2-ref` after `arm/sbsa`)
- `MAINTAINERS` — new section (QEMU convention for a new machine)

Optional:

- `configs/devices/aarch64-softmmu/default.mak` — comment showing
  `# CONFIG_N2_REF=n` (matches the existing sbsa/npcm pattern)

---

## 4. Board implementation

### 4.1 Machine type

Use `DEFINE_MACHINE_AARCH64` (`include/hw/arm/machines-qom.h`): the machine then
appears only in `qemu-system-aarch64`, which is correct for an AArch64-only N2
board. (`DEFINE_MACHINE_ARM` would expose it in `qemu-system-arm` too, where
`neoverse-n2` does not exist.)

```c
#define TYPE_N2_REF_MACHINE MACHINE_TYPE_NAME("n2-ref")
OBJECT_DECLARE_SIMPLE_TYPE(N2RefMachineState, N2_REF_MACHINE)

/* Memory map (§2). All values are CPU physical addresses. */
#define N2_REF_RAM_BASE       0x80000000ULL   /* RAM region, 2 GiB max */
#define N2_REF_UART_BASE      0x0C000000ULL
#define N2_REF_GICD_BASE      0x0E000000ULL
#define N2_REF_GICD_SIZE      0x00010000ULL
#define N2_REF_GITS_BASE      0x0E040000ULL
#define N2_REF_GITS_SIZE      0x00020000ULL   /* ITS_SIZE */
#define N2_REF_GICR_BASE      0x0E080000ULL
#define N2_REF_GICR_SIZE      0x00200000ULL   /* 8 x GICV4_REDIST_SIZE */
#define N2_REF_PCIE_ECAM_BASE 0x40000000ULL
#define N2_REF_PCIE_ECAM_SIZE 0x10000000ULL   /* 256 MiB = 256 buses */
#define N2_REF_PCIE_MMIO_BASE 0x60000000ULL
#define N2_REF_PCIE_MMIO_SIZE 0x20000000ULL   /* ends exactly at RAM */

/*
 * GIC interrupt IDs. The GIC's SPI input lines are indexed by SPI number
 * (0..255) in both qdev_get_gpio_in() and the FDT, and INTID = 32 + SPI.
 * The board is specified in INTIDs, so define those and derive the rest.
 */
#define N2_REF_UART_INTID     33              /* SPI 1  */
#define N2_REF_PCIE_INTID     65              /* SPI 33 */
#define N2_REF_UART_SPI       (N2_REF_UART_INTID - 32)
#define N2_REF_PCIE_SPI       (N2_REF_PCIE_INTID - 32)

#define N2_REF_GIC_NUM_IRQ    288             /* 256 SPIs + 32 PPIs/SGIs */

struct N2RefMachineState {
    MachineState parent;
    struct arm_boot_info bootinfo;
    DeviceState *gic;
    DeviceState *pcie;
    uint32_t gic_phandle;
    uint32_t its_phandle;
    int fdt_size;
};
```

Includes needed in `hw/arm/n2-ref.c` (copy the set from `sbsa-ref.c` and drop
the SMMU/PCIe-GPEX/flash ones):

```c
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/device_tree.h"
#include "system/numa.h"              /* numa_cpu_pre_plug() */
#include "system/system.h"
#include "exec/hwaddr.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"              /* timer/PMU/PPI INTID constants */
#include "hw/arm/fdt.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/char/serial-mm.h"       /* serial_mm_init(), SerialMM */
#include "hw/intc/arm_gicv3_common.h"/* GICV4_REDIST_SIZE, gicv3_class_name() */
#include "hw/intc/arm_gicv3_its_common.h"
#include "hw/pci/pci.h"              /* pci_init_nic_devices() */
#include "hw/pci-host/xilinx-pcie.h" /* TYPE_XILINX_PCIE_HOST */
#include "hw/core/qdev-properties.h"
#include "net/net.h"
#include "qobject/qlist.h"
#include "qom/object.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
```

`ARM_CPU_TYPE_NAME()` lives in `target/arm/cpu-qom.h`; `GICV4_REDIST_SIZE` and
`ITS_SIZE` come from the two `hw/intc/..._common.h` headers.

### 4.2 Class init

```c
static void n2_ref_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("neoverse-n2"),
        NULL,
    };

    mc->init = n2_ref_init;
    mc->desc = "Neoverse-N2 reference board (GIC-700, MIG, 8250, Xilinx PCIe)";
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("neoverse-n2");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_id = "n2-ref.ram";
    mc->default_ram_size = 2 * GiB;
    mc->max_cpus = 8;                 /* GICR region capacity */
    mc->default_cpus = 4;
    mc->minimum_page_bits = 12;
    mc->possible_cpu_arch_ids = n2_ref_possible_cpu_arch_ids;
    mc->default_nic = "e1000e";
    /* BARs start at 0x60000000, so pci_allow_0_address is not needed */
}
```

`possible_cpu_arch_ids` is **not optional**: `machine_query_hotpluggable_cpus()`
and the NUMA paths call it unconditionally. Implement it like
`sbsa_ref_possible_cpu_arch_ids()`:

```c
static const CPUArchIdList *n2_ref_possible_cpu_arch_ids(MachineState *ms)
{
    unsigned int max_cpus = ms->smp.max_cpus;

    if (ms->possible_cpus) {
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }
    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (int n = 0; n < max_cpus; n++) {
        ms->possible_cpus->cpus[n].type = ms->cpu_type;
        ms->possible_cpus->cpus[n].arch_id =
            arm_build_mp_affinity(n, ARM_DEFAULT_CPUS_PER_CLUSTER);
        ms->possible_cpus->cpus[n].props.has_thread_id = true;
        ms->possible_cpus->cpus[n].props.thread_id = n;
    }
    return ms->possible_cpus;
}
```

The `props` fields are what make `-smp N,threads=1` and
`query-hotpluggable-cpus` report a coherent topology; `sbsa-ref` sets them and
`numa_cpu_pre_plug()` in the CPU loop reads them. Do **not** set
`cpu_index_to_instance_props`/`get_default_cpu_node_id` in v1: that allocates
`numa_state` and enables NUMA paths you have not tested.

The CPU creation loop must also call
`numa_cpu_pre_plug(&possible_cpus->cpus[n], DEVICE(cpuobj), &error_fatal)` and
set `cs->cpu_index = n` (see §4.3), otherwise the props above are never
validated and `-numa` is silently ignored.

### 4.3 `n2_ref_init()` ordering

The order matters; this is the sequence:

1. **Validate** `machine->ram_size <= 2 GiB` and `smp.cpus <= 8` (error out,
   like `sbsa-ref` does).
2. **Map RAM**: `memory_region_add_subregion(get_system_memory(),
   N2_REF_RAM_BASE, machine->ram)`. `machine->ram` is already created by
   `machine_run_board_init()` before `mc->init` is called.
3. **Create CPUs before the GIC**. `arm_gicv3_common_realize()` and
   `gicv3_init_cpuif()` call `qemu_get_cpu(first_cpu_idx + i)`; the CPUs must
   exist and be realized.
   - `possible_cpus = mc->possible_cpu_arch_ids(machine)` first
   - `object_new(possible_cpus->cpus[n].type)`
   - `mp-affinity` = `possible_cpus->cpus[n].arch_id`
   - `cs->cpu_index = n`, then
     `numa_cpu_pre_plug(&possible_cpus->cpus[n], DEVICE(cpuobj), &error_fatal)`
   - `cntfrq` = 1 GHz (N2/Armv8.6 mandate; sbsa uses `SBSA_GTIMER_HZ`)
   - **`has_el3 = false`**, `has_el2 = true` — see §9 for why
   - `memory` link = system memory
   - `qdev_realize()`, then `object_unref(cpuobj)`
4. **GIC** (`arm-gicv3`, revision 4).
5. **ITS** (`arm-gicv3-its`), linked to the GIC.
6. **UART** (`serial_mm_init`).
7. **RAM/MIG** — nothing to do: the RAM mapped in step 2 *is* the region at
   `0x80000000` (§4.6). No device, no MMIO, no SPI.
8. **PCIe host bridge**.
9. **Build the FDT**.
10. **`arm_load_kernel()`**.

### 4.4 GIC — the easy-to-miss details

```c
static void n2_ref_create_gic(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    uint32_t smp_cpus = ms->smp.cpus;
    uint32_t capacity = N2_REF_GICR_SIZE / GICV4_REDIST_SIZE; /* 0x200000/0x40000 = 8 */
    QList *redist_region_count = qlist_new();

    s->gic = qdev_new(gicv3_class_name());   /* not TYPE_ARM_GICV3 */
    qdev_prop_set_uint32(s->gic, "revision", 4);
    qdev_prop_set_uint32(s->gic, "num-cpu", smp_cpus);
    qdev_prop_set_uint32(s->gic, "num-irq", N2_REF_GIC_NUM_IRQ);
    qdev_prop_set_bit(s->gic, "has-security-extensions", false);
    qdev_prop_set_bit(s->gic, "has-lpi", true);
    qlist_append_int(redist_region_count, MIN(smp_cpus, capacity));
    qdev_prop_set_array(s->gic, "redist-region-count", redist_region_count);
    object_property_set_link(OBJECT(s->gic), "sysmem",
                             OBJECT(get_system_memory()), &error_fatal);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), 0, N2_REF_GICD_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), 1, N2_REF_GICR_BASE);

    /* PPIs + IRQ/FIQ/VIRQ/VFIQ, copied from virt's gic_connect_ppis() */
}
```

**Gotcha 1 — redistributor capacity.** `gicv3_redist_size()` returns
`GICV4_REDIST_SIZE` (`0x40000`) only when the redistributor has
`GICR_TYPER.VLPIS`, which is set when `has-lpi` **and** `revision > 3`. If you
compute capacity with `GICV3_REDIST_SIZE` (`0x20000`) while using revision 4 +
`has-lpi`, realize fails with *"Capacity of the redist regions does not match
the number of vcpus"*. Use `GICV4_REDIST_SIZE` here.

**Gotcha 2 — `has-lpi` requires the `sysmem` link** before realize, otherwise:
*"Redist-ITS: Guest 'sysmem' reference link not set"*.

**Gotcha 3 — the mapped GICR region is smaller than the FDT window.** The
redistributor `MemoryRegion` is created with size
`redist_region_count[i] * gicv3_redist_size(s)`
(`arm_gicv3_common.c`), i.e. `smp_cpus * 0x40000`, while the FDT `reg` in §5
describes the full 2 MiB. That is the same thing `virt` does and is safe only
because the last redistributor of the region has `GICR_TYPER.Last` set, so Linux
stops enumerating there. Do not assume the whole 2 MiB is backed by MMIO: with
`-smp 4` only `0x0E080000`–`0x0E180000` is. A qtest that reads beyond
`count * 0x40000` gets the generic unassigned-memory handler, which returns 0
silently (`system/memory.c`, `unassigned_mem_read`) — so such a check passes
vacuously and proves nothing. Probe the first redistributor only.

ITS:

```c
DeviceState *its = qdev_new(its_class_name());   /* not TYPE_ARM_GICV3_ITS */
object_property_set_link(OBJECT(its), "parent-gicv3", OBJECT(s->gic), &error_abort);
sysbus_realize_and_unref(SYS_BUS_DEVICE(its), &error_fatal);
sysbus_mmio_map(SYS_BUS_DEVICE(its), 0, N2_REF_GITS_BASE);   /* ITS_SIZE = 0x20000 */
```

Use `its_class_name()` (`hw/intc/arm_gicv3_its_common.h`) like both `virt` and
`sbsa-ref` do, not the literal `TYPE_ARM_GICV3_ITS`: it returns `arm-its-kvm`
when the KVM irqchip is in use and `arm-gicv3-its` otherwise. The literal name
would break an accelerated build. Since this board is `depends on TCG`, the
literal happens to work, but using the helper costs nothing and matches the
tree. (Same story for `gicv3_class_name()`: `kvm-arm-gicv3`, `whpx-arm-gicv3`,
`hvf-arm-gicv3`, or `arm-gicv3`.)

`arm-gicv3-its` sets `msi_nonbroken = true`
(`hw/intc/arm_gicv3_its_common.c`), which MSI/MSI-X need.

PPI wiring (per CPU, `intidbase = 256 + i * 32`). The PPI numbers come from
`include/hw/arm/bsa.h`; note they are **not** contiguous, so do not guess:

- generic timer outputs → `ARCH_TIMER_NS_EL1_IRQ` (30), `ARCH_TIMER_VIRT_IRQ`
  (27), `ARCH_TIMER_NS_EL2_IRQ` (26), `ARCH_TIMER_NS_EL2_VIRT_IRQ` (28),
  `ARCH_TIMER_S_EL1_IRQ` (29), `ARCH_TIMER_S_EL2_IRQ` (20),
  `ARCH_TIMER_S_EL2_VIRT_IRQ` (19) — use the same `timer_irq[]` table as
  `sbsa-ref`.
- `gicv3-maintenance-interrupt` → `ARCH_GIC_MAINT_IRQ` (25)
- `pmu-interrupt` → `VIRTUAL_PMU_IRQ` (23)
- GIC IRQ/FIQ/VIRQ/VFIQ outputs → CPU inputs (`i`, `i+smp`, `i+2*smp`,
  `i+3*smp`).

**SPI vs INTID.** `qdev_get_gpio_in(s->gic, n)` for `n < num_irq - 32` means
**SPI `n`**, not INTID `n` (`gicv3_set_irq()` adds `GIC_INTERNAL` internally,
`hw/intc/arm_gicv3.c`). Likewise the DT's `<GIC_SPI n>` encodes SPI `n`. So the
PCIe line specified as INTID 65 is SPI 33 in both places; using 65 in either
would land on INTID 97 instead.

**PPI range.** The GIC's input GPIO array is `[0..num_spis-1]` for SPIs,
then 32 PPIs per CPU (`arm_gicv3_common.c`, `qdev_init_gpio_in`). With
`num_irq = 288` that is SPIs at 0–255 and CPU *i*'s PPIs at `256 + i*32` …
`256 + i*32 + 31`. So `intidbase + const` only lands on a PPI when
`const < 32`. A constant `>= 32` silently wires that line to the *next CPU's*
PPI block (and for the last CPU runs past the end of the GPIO array, which
trips the `assert(n < gpio_list->num_in)` in `qdev_get_gpio_in_named()`). Keep
every constant above `< 32`.
With `has_el3 = false` the secure-timer lines have no source, but connecting
them is harmless (they simply never assert) and keeps the table identical to
`sbsa-ref`.

### 4.5 8250 UART

```c
serial_mm_init(get_system_memory(), N2_REF_UART_BASE, 0,   /* regshift */
               qdev_get_gpio_in(s->gic, N2_REF_UART_SPI),
               115200, serial_hd(0), DEVICE_LITTLE_ENDIAN);
```

`regshift=0` means true 8250 byte-spaced registers. The device creates its own
MMIO region, sized `8 << regshift` bytes (`hw/char/serial-mm.c`), so with
`regshift=0` only 8 bytes are mapped. Two consistent options:

- **Declare what is actually there:** DT `reg = <0x0c000000 0x8>`. Simple and
  honest, but unusual for a real SoC and some tooling assumes a page-sized
  window.
- **Keep a 4 KiB DT window:** wrap the device region in an alias, the way
  `virt` does for PCIe (`memory_region_init_alias()` + add at the base), so the
  DT can say `0x1000` and the extra bytes are unassigned rather than faulting.
  The alias target is `sysbus_mmio_get_region(SYS_BUS_DEVICE(smm), 0)`; keep the
  `SerialMM *` returned by `serial_mm_init()` to reach it.

Either way, accesses past the 8 real bytes read as 0 through the generic
unassigned handler, so they will not abort.

If your hardware puts the 16550 on a 32-bit AXI4-Lite bus, use `regshift=2`
(32-byte region) and set `reg-shift = <2>; reg-io-width = <4>;` in the DT.

`serial_mm_init()` is a helper that does the `qdev_new(TYPE_SERIAL_MM)` /
property setup / `sysbus_realize` / `memory_region_add_subregion()` for you and
returns the `SerialMM *`. The board call is mandatory — no other code creates
the UART. (Only the inner `TYPE_SERIAL` child is `user_creatable = false`; the
sysbus `TYPE_SERIAL_MM` wrapper is what the helper instantiates.)

### 4.6 RAM / MIG

The MIG is not modelled as a device. The requested `mig=0x80000000` is simply
the base of the RAM region, so the board maps `machine->ram` there and stops:

```c
memory_region_add_subregion(get_system_memory(), N2_REF_RAM_BASE, machine->ram);
```

Consequences:

- `machine->ram` occupies `0x80000000`–`0x1_0000_0000`; `ram_size` must be
  `<= 2 GiB`.
- There is no MIG register block, so no `xlnx,mig-*` register window exists and
  no MIG SPI is wired; SPI 2 stays reserved.
- Do not emit a `xlnx,mig-*` DT node: there is no device behind it, and Linux
  would try to probe a driver for absent registers.
- If firmware insists on a MIG register block, add one later at a free
  64 KiB-aligned slot below `0x4000_0000` (for example `0x0E80_0000`); the RAM
  region stays exactly where it is.

This is deliberately the simplest reading: "a memory region starting at
`0x80000000`", with no memory-controller model behind it.

### 4.7 Xilinx PCIe host

```c
static void n2_ref_create_pcie(N2RefMachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_XILINX_PCIE_HOST);
    MemoryRegion *cfg, *mmio;

    qdev_prop_set_uint32(dev, "bus_nr", 0);
    qdev_prop_set_uint64(dev, "cfg_base", N2_REF_PCIE_ECAM_BASE);
    qdev_prop_set_uint64(dev, "cfg_size", N2_REF_PCIE_ECAM_SIZE);
    qdev_prop_set_uint64(dev, "mmio_base", N2_REF_PCIE_MMIO_BASE);
    qdev_prop_set_uint64(dev, "mmio_size", N2_REF_PCIE_MMIO_SIZE);
    qdev_prop_set_bit(dev, "link_up", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    cfg  = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
    mmio = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 1);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        N2_REF_PCIE_ECAM_BASE, cfg, 0);
    memory_region_add_subregion_overlap(get_system_memory(), 0, mmio, 0);

    qdev_connect_gpio_out_named(dev, "interrupt_out", 0,
                                qdev_get_gpio_in(s->gic, N2_REF_PCIE_SPI));
    s->pcie = dev;
}
```

This mirrors `hw/mips/boston.c::xilinx_pcie_init()` exactly. Note that
`cfg_base` is a dead property in the model (`pcie_host_mmcfg_init()` only uses
`cfg_size`), so the board must do the ECAM `memory_region_add_subregion()`
itself — which the code above does.

If you want `-nic`/the default NIC to work, add after realize (needs
`#include "net/net.h"`):

```c
PCIHostState *pci = PCI_HOST_BRIDGE(dev);
MachineClass *mc = MACHINE_GET_CLASS(s);

pci_init_nic_devices(pci->bus, mc->default_nic);
```

as `virt` and `sbsa-ref` do. Without it `mc->default_nic` is never consulted
and the board comes up with no NIC.

Three properties of this model drive the design:

1. **It is not user-creatable** (`dc->user_creatable = false`), so it must be
   instantiated in board code; `-device xilinx-pcie-host` will fail.
2. **The MMIO container is mapped at address 0 and starts disabled**
   (`memory_region_set_enabled(&s->mmio, false)`). Guest BARs become visible only
   after firmware writes `ROOTCFG_RPSCR.BRIDGEEN` (offset `0x148`, bit 0).
   Linux's `pcie-xilinx` driver does this; a bootloader that skips it will see
   BARs read as zero.
3. **There is one INTx output**, fed from an internal 16-entry FIFO
   (`ROOTCFG_RPIFR1/2`). That is why all four PCI pins should map to the single
   line INTID 65 (SPI 33) in the DT; the guest's handler demultiplexes via the
   FIFO registers.

Because the bus memory region is the same `s->mmio` container, BAR addresses are
absolute and 1:1 with CPU addresses. Keep every BAR inside
`0x60000000`–`0x7FFFFFFF`; the root bridge advertises only that window, and a BAR
below it would shadow other MMIO once the bridge is enabled.

### 4.8 `bootinfo` and `arm_load_kernel()`

Fill these fields; omitting `loader_start` or `psci_conduit` is the most common
way to get a silently broken boot:

```c
static void *n2_ref_get_dtb(const struct arm_boot_info *info, int *size)
{
    N2RefMachineState *s = container_of(info, N2RefMachineState, bootinfo);
    MachineState *ms = MACHINE(s);

    *size = s->fdt_size;      /* set when the FDT is built, see §5 */
    return ms->fdt;
}

...
s->bootinfo.ram_size = machine->ram_size;
s->bootinfo.board_id = -1;
s->bootinfo.loader_start = N2_REF_RAM_BASE;
s->bootinfo.get_dtb = n2_ref_get_dtb;      /* returns ms->fdt */
s->bootinfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;   /* see §9 */
arm_load_kernel(ARM_CPU(first_cpu), machine, &s->bootinfo);
```

`loader_start = 0x80000000` is what makes the kernel land at the RAM base and
what `earlycon`/`/chosen` addresses are derived from.

**Do not set `skip_dtb_autoload = true` unless you also call `arm_load_dtb()`
yourself.** `arm_load_kernel()` writes the DT into guest RAM, fills in
`/memory`, `/chosen` (bootargs, initrd) and `/psci`, and applies the
`psci-conduit` CPU property — all inside this block:

```c
if (!info->skip_dtb_autoload && have_dtb(info)) {
    arm_load_dtb(info->dtb_start, info, info->dtb_limit, as, ms, cpu);
}
```

Setting the flag without a replacement call produces a boot with no device tree
in memory at all. `virt` sets it *and* calls `arm_load_dtb()` from a
`machine_done` notifier (`virt_machine_done()`), because it adds PCI/platform
nodes after `mc->init`. This board builds the whole DT in `mc->init`, so leave
the flag `false` and let `arm_load_kernel()` do the load — that ordering also
guarantees `/psci` reflects the conduit actually programmed into the CPUs.

If you later need late FDT fixups, switch to virt's pattern: set
`skip_dtb_autoload = true`, add a `MachineClass::machine_done` notifier, and call
`arm_load_dtb(info->dtb_start, info, info->dtb_limit, as, ms, cpu)` there.

---

## 5. FDT

Build it in the board (`create_device_tree()`), store it in `MachineState::fdt`
(virt's pattern), and return it via `bootinfo.get_dtb`. Assigning `ms->fdt`
directly is what makes `-machine dumpdtb=` work at all: `qmp_dumpdtb()` only
dumps `current_machine->fdt` (`system/device_tree.c`), and it runs *after*
`mc->init`, so with the autoload left enabled (§4.8) the dumped blob already
contains the `/memory`, `/chosen` and `/psci` nodes that `arm_load_dtb()` added.
Do not add those three nodes yourself.

Keep the size too: `ms->fdt = create_device_tree(&s->fdt_size);` — the
`get_dtb` callback in §4.8 must return that size, and it is the *pre*-fixup
size, which is fine because `arm_load_dtb()` recomputes
`fdt_totalsize()` itself after adding its nodes.

Required nodes:

```
/                       compatible = "qemu,n2-ref"; #address-cells=2; #size-cells=2
                        interrupt-parent = <gic_ph>   /* for SPI users */
/cpus                   #address-cells=1; #size-cells=0   (or 2/0, see note)
/cpus/cpu@N             device_type="cpu"; compatible=<armcpu->dtb_compatible>;
                        reg=<mpidr>; enable-method="psci" (if conduit != disabled && smp>1)
/timer                  compatible="arm,armv8-timer","arm,armv7-timer"; always-on;
                        interrupts = 4 (or 5) PPIs, level-high, in this order:
                          PPI 13 (INTID 29, S_EL1), PPI 14 (30, NS_EL1),
                          PPI 11 (27, VIRT),     PPI 10 (26, NS_EL2)
                        (DT wants PPI = INTID - 16, i.e. INTID_TO_PPI)
                        `neoverse-n2` sets ID_AA64MMFR1.VH=1 (FEAT_VHE), so
                        `virt` would emit a 5th entry for the NS-EL2 virtual
                        timer: PPI 12 (INTID 28). §4.4 already wires
                        ARCH_TIMER_NS_EL2_VIRT_IRQ, so emit the 5th entry too;
                        if you drop the PPI from the wiring, drop the entry as
                        well or the timer never fires.
/pmu                    optional: compatible="arm,armv8-pmuv3";
                        interrupts = <PPI 7 LEVEL_HI>  /* INTID 23 -> PPI 7 */
                        (only if you wire `pmu-interrupt`; see §4.4)
/intc@0e000000          compatible="arm,gic-v3"; interrupt-controller;
                        #interrupt-cells=3; #address-cells=2; #size-cells=2; ranges;
                        #redistributor-regions = <1>;
                        /* 2 addr + 2 size cells per entry: */
                        reg = <0x0 0x0E000000 0x0 0x00010000>,
                              <0x0 0x0E080000 0x0 0x00200000>;
                        interrupts = <PPI 9 LEVEL_HI>;   /* INTID 25 -> PPI 9 */
                        phandle = <gic_ph>
/intc@0e000000/its@0e040000
                        compatible="arm,gic-v3-its"; msi-controller;
                        #msi-cells=1; phandle=<its_ph>
                        reg = <0x0 0x0E040000 0x0 0x00020000>;
/serial@0c000000        compatible="ns16550a"; reg=<0x0 0x0c000000 0x0 0x1000>;
                        interrupts=<SPI 1 LEVEL_HI>; clock-frequency=<100000000>
                        (reg-shift=<2>; reg-io-width=<4> if regshift=2)
                        NOTE: serial_mm_init() maps only 8 bytes at regshift=0,
                        so either declare reg size 0x8 here or wrap the device
                        region in a 4 KiB alias (see §4.5).
/pci@40000000           see below
/memory@80000000        added automatically by arm_load_dtb()
/chosen                 added automatically; /psci added automatically
```

Two details copied from `virt`'s `fdt_add_gic_node()` that are easy to miss:
`#redistributor-regions = <1>` (otherwise Linux assumes one region but may
mis-size it), and `/`'s `interrupt-parent` must point at `gic_ph`.

`/cpus` needs `#address-cells`/`#size-cells` that match the `reg` encoding.
`arm_build_mp_affinity(idx, 8)` (`target/arm/cpu.c`) packs only Aff0/Aff1, so
with `max_cpus = 8` every MPIDR is `0..7` and fits in one 32-bit cell. Two
consistent choices: `#address-cells = <1>` + `qemu_fdt_setprop_cell()` (what
`virt` uses for small MPIDRs), or `#address-cells = <2>` +
`qemu_fdt_setprop_u64()` (what `sbsa-ref` uses). Pick one and use the matching
setter; a mismatch silently writes the wrong property length.

PCI node — **copy the shape from `hw/mips/boston.c::fdt_create_pcie()`**,
because Linux's `pcie-xilinx` driver requires the synthetic child
interrupt-controller node, not a direct pointer to the GIC:

```
/pci@40000000
    compatible = "xlnx,axi-pcie-host-1.00.a"
    device_type = "pci"
    #address-cells = <3>; #size-cells = <2>; #interrupt-cells = <1>
    /* / has #address-cells=2, #size-cells=2, so reg is 4 cells: */
    reg = <0x0 0x40000000 0x0 0x10000000>
    /* ranges entry = child addr (3) + parent addr (2) + size (2) = 7 cells: */
    ranges = <0x02000000 0x0 0x60000000  0x0 0x60000000  0x0 0x20000000>
    bus-range = <0x00 0xff>
    linux,pci-domain = <0>               /* optional, matches virt */
    interrupt-parent = <gic_ph>
    interrupts = <SPI 33 LEVEL_HI>       /* INTID 65 */
    msi-map = <0 its_ph 0 0x10000>       /* devid == BDF */
    dma-coherent
    interrupt-map-mask = <0 0 0 7>
    interrupt-map = 4 entries: <0 0 0 pin  intc_ph pin>
    /pci@40000000/interrupt-controller
        interrupt-controller; #address-cells=<0>; #interrupt-cells=<1>; phandle=<intc_ph>
```

The cell counts above are the classic trap: `boston.c` can write `ranges` as
6 cells because its root node uses `#address-cells = <1>`, but this board's
root uses `<2>`, so a copy-pasted 6-cell `ranges` is malformed and Linux will
fail to parse the window. Use `qemu_fdt_setprop_sized_cells()` (or count the
cells explicitly) rather than `qemu_fdt_setprop_cells()` with borrowed
arguments. The same applies to `reg`: 4 cells here, not 2.

The `msi-map` is what makes QEMU's ITS accept MSI writes: `pci_msi_trigger()`
tags the write with `pci_requester_id(dev)` (the BDF), and the ITS translation
region uses `attrs.requester_id` as the device ID. With `bus_nr=0` and a 16-bit
map length, every BDF on the hierarchy maps 1:1. Add the same `msi-map` to the
PCI node; virt's `create_pcie()` is the reference.

---

## 6. Kconfig and meson

`hw/arm/Kconfig`:

```
config N2_REF
    bool
    default y
    depends on TCG && AARCH64
    imply PCI_DEVICES
    select ARM_GIC              # selects ARM_GICV3 under TCG, MSI_NONBROKEN
    select SERIAL_MM            # selects SERIAL
    select PCI_EXPRESS_XILINX   # selects PCI_EXPRESS -> PCI
    select DEVICE_TREE          # needs FDT
```

`hw/arm/meson.build` (next to the other board lines):

```meson
arm_common_ss.add(when: 'CONFIG_N2_REF', if_true: files('n2-ref.c'))
```

Because the board is AArch64-only, `default y` only takes effect in
`aarch64-softmmu`; `arm-softmmu` does not define `CONFIG_AARCH64`.

`DEVICE_TREE` `depends on FDT` (`hw/core/Kconfig`), so a build configured
without libfdt silently leaves the symbol off; `select DEVICE_TREE` cannot
force it. That is fine — the board is unusable without an FDT anyway, so the
failure mode is "machine not registered", not a link error. libfdt resolution
is in `meson.build` (`fdt` option, default `auto`): it prefers the system
library, falls back to the vendored `subprojects/dtc` when downloads are
allowed, and needs libfdt ≥ 1.5.1.

---

## 7. Build and run

```sh
mkdir -p build && cd build
../configure --target-list=aarch64-softmmu
make -j"$(nproc)"
```

Direct kernel boot:

```sh
./qemu-system-aarch64 -M n2-ref -cpu neoverse-n2 -m 2G -smp 4 \
  -kernel arch/arm64/boot/Image \
  -append "console=ttyS0,115200 earlycon=uart8250,mmio,0x0c000000 root=/dev/vda rw" \
  -drive if=none,file=rootfs.img,format=raw,id=hd0 \
  -device virtio-blk-pci,drive=hd0 \
  -nographic
```

Inspect the generated DT without booting:

```sh
./qemu-system-aarch64 -M n2-ref -m 2G -smp 4 -machine dumpdtb=n2.dtb -nographic
dtc -I dtb -O dts n2.dtb
```

`-M n2-ref,help` and `./qemu-system-aarch64 -M help | grep n2-ref` should both
work.

---

## 8. Tests

### 8.1 New qtest: `tests/qtest/n2-ref-test.c`

Register conditionally in `tests/qtest/meson.build` inside `qtests_aarch64`.
That variable is a chain of `(condition ? [...] : []) + \` fragments ending in
the literal list `['arm-cpu-features', ...]`; add a fragment *before* that final
list:

```meson
  (config_all_devices.has_key('CONFIG_N2_REF') ? ['n2-ref-test'] : []) + \
  ['arm-cpu-features',
   'numa-test',
   ...
```

The test source is picked up automatically as `<name>.c`. Test with
`qtest_init("-machine n2-ref -m 2G -smp 4 -display none -serial none")`
(`qtest_start()` does not exist; the helper is `qtest_init()`/`qtest_initf()`):

| Check | Address / expectation |
|---|---|
| RAM | write/read `0x80000000` and `0xFFFFFFFF` (last byte) via `qtest_memwrite/read` |
| PCIe ECAM root port | `readl(0x40000000) == 0x702110ee` (vendor `0x10ee`, device `0x7021`) |
| GICD_TYPER | `(readl(0x0E000004) & 0x1f) == 8` (288/32 − 1) |
| ITS present | `readl(0x0E040004) != 0` (GITS_IIDR, `gicv3_iidr()` = `0x43b`) |
| GICR mapped | `(readl(0x0E080008) & 3) == 3` (GICR_TYPER PLPIS/VLPIS, low word) |
| UART mapped | `(readb(0x0C000000 + 5) & 0x20) != 0` (LSR.THRE, set at reset) |

The ECAM read works **without** `BRIDGEEN`: the config region is the
`PCIExpressHost::mmio` region, and only the separate BAR container
(`XilinxPCIEHost::mmio`, mapped at 0) is gated by `ROOTCFG_RPSCR.BRIDGEEN`.
Testing a BAR therefore needs a guest that first sets `BRIDGEEN`, which is a
job for the functional test, not this qtest.

The INTx numbering is easiest to check outside qtest, from the dumped DTB:

```sh
./qemu-system-aarch64 -M n2-ref -m 2G -smp 4 -machine dumpdtb=n2.dtb -nographic
dtc -I dtb -O dts n2.dtb | grep -A2 'pci@40000000'
```

Expect `interrupts = <0x00 0x21 0x04>` on the PCI node (`0x21` = 33 = SPI,
`0x04` = level-high) and `<0x00 0x01 0x04>` on the UART. A `0x41` in the PCI
node means INTID 65 was written where SPI 33 belonged.

Run:

```sh
pyvenv/bin/meson test qtest-aarch64/n2-ref-test
```

### 8.2 Smoke boot (optional but cheap)

Add a `testdef` to `tests/qtest/boot-serial-test.c` with an AArch64 payload that
writes `'T'` to `0x0C000000`:

```c
static const uint8_t kernel_n2ref[] = {
    0x02, 0x80, 0xa1, 0xd2,   /* movz x2, #0x0c00, lsl #16  -> 0x0C000000 */
    0x81, 0x0a, 0x80, 0x52,   /* movz w1, #'T' */
    0x41, 0x00, 0x00, 0x39,   /* strb w1, [x2] */
    0x00, 0x00, 0x00, 0x14,   /* b . */
};
...
{ "aarch64", "n2-ref", "", "T", sizeof(kernel_n2ref), kernel_n2ref },
```

The 8250 needs no init sequence for polled output — writing THR (`offset 0`) is
enough, as the existing `kernel_aarch64` entry for `virt` shows.

Note where the payload actually runs: `load_aarch64_image()` loads a raw image
(no `ARM\x64` magic) at `loader_start + KERNEL64_LOAD_ADDR`
(`hw/arm/boot.c`, `KERNEL64_LOAD_ADDR = 0x80000`), i.e. `0x80080000`, and
`bootloader_aarch64` is written at `loader_start` and branches to that entry.
So the payload does *not* start at `loader_start`; it is still inside RAM and
runs at EL2 (Linux-style direct boot), which is fine for this smoke test.
`boot-serial-test` only waits for the `"T"` on the serial line, so the exact
entry address does not matter as long as it is in RAM.

Caveat: `boot-serial-test.c` is compiled without per-device `CONFIG_*`, but its
`main()` already guards every entry with `qtest_has_machine(tests[i].machine)`,
so an unconditional table entry is safe on builds with `CONFIG_N2_REF=n`.

### 8.3 Functional test (optional, phase 5)

`tests/functional/aarch64/test_n2ref.py` booting a real distro image is the only
way to exercise the PCIe/ITS/MIG stack end to end, but it is expensive. Register
it in `tests/functional/aarch64/meson.build` under
`tests_aarch64_system_thorough` when it exists.

---

## 9. Design decisions and traps

**EL3 / PSCI.** `neoverse-n2` sets `ARM_FEATURE_EL3` and `ARM_FEATURE_EL2`, and
the CPU properties `has_el3`/`has_el2` both default to `true`
(`target/arm/cpu.c`). Leaving `has_el3=true` gives the guest a secure world it
has no firmware for, so set `has_el3 = false` and keep `has_el2 = true` in CPU
realize. That also matches `has-security-extensions = false` on the GIC.

Then the **conduit must be SMC, not HVC**. `arm_load_kernel()` computes
`boot_el` from the CPU features (2 when EL2 exists) and *silently disables* a
conduit that targets the same or a lower EL:

```c
if ((info->psci_conduit == QEMU_PSCI_CONDUIT_HVC && boot_el >= 2) ||
    (info->psci_conduit == QEMU_PSCI_CONDUIT_SMC && boot_el == 3)) {
    info->psci_conduit = QEMU_PSCI_CONDUIT_DISABLED;
}
```

(`hw/arm/boot.c`). So `has_el2=true` + HVC yields no PSCI at all: no
`/psci` node, secondary CPUs never start, and SMP boot fails. Use
`QEMU_PSCI_CONDUIT_SMC`, exactly as `virt` does when `virtualization=on`. SMC
without EL3 is legal here because PSCI-via-SMC makes QEMU's emulation behave as
an ersatz EL3 (`target/arm/tcg/op_helper.c`, `helper_exception_with_syndrome`).
The alternative is `has_el2 = false` + HVC, but that removes EL2 from the
guest.

Set `QEMU_PSCI_CONDUIT_DISABLED` when `-bios`/firmware is used (the firmware
supplies PSCI). The GIC must then use `has-security-extensions = false`,
because there is no secure state.

**No `-bios` support in phase 1.** `arm_setup_firmware_boot()` does not load the
firmware image itself; the board must. Firmware boot also expects the reset
vector at `0x0` (QEMU leaves PC at reset for non-Linux firmware boot), and this
map has nothing at `0x0`. If firmware boot is required, add a ROM/pflash window
at `0x0000_0000` (e.g. 64 MiB) and load `machine->firmware` there, like
`sbsa-ref` does with pflash. Until then, `-bios` is silently ignored — document
that.

**ECAM base alignment.** `pcie_host_mmcfg_init()` does not check base alignment,
but the ECAM decoding is offset-based; `0x40000000` is 256 MiB-aligned, so it is
safe.

**GICv4 vs GIC-700.** `revision=4` gives GICv4 vLPI support but not GICv4.1.
`GICD_TYPER2` is implemented as read-zero with an explicit comment that v4.1 is
not emulated. Guests that gate on GICv4.1 (e.g. KVM vSGI, VCONFADDR) will not
work. There is no way to make `arm-gicv3` claim GIC-700; the ID registers will
read as a GIC-500-style r0p0 (`GICD_IIDR`). Document this as a fidelity limit,
not a bug.

**Redistributor count vs `max_cpus`.** `max_cpus=8` is dictated by the 2 MiB GICR
window at `0x0E080000`–`0x0E280000` and the 256 KiB GICv4 frame. If you raise
`max_cpus`, grow the GICR window in 2 MiB steps and update the FDT `reg`; do not
just bump the property. GICR ends at `0x0E28_0000` and the next occupied region
is PCIe ECAM at `0x4000_0000`, so there is about 797 MiB of headroom for
additional redistributor frames.

**MIG is just RAM.** `mig=0x80000000` is the base of the RAM region (§0, §4.6).
There is no memory-controller register block and no MIG SPI; if firmware needs
one, add it at a separate base later.

**PCIe INTx line.** The RC's single `interrupt_out` is wired to **INTID 65**
(SPI 33). Three places must agree: `qdev_get_gpio_in(s->gic, 33)` in C, the
PCI node's `interrupts = <GIC_SPI 33 ...>` in the FDT, and the `interrupt-map`
entries (which route all four PCI pins to the synthetic interrupt-controller
child, so the SPI number does not appear there). Writing 65 instead of 33 in
either C or the FDT is a silent off-by-32: the line still exists, so nothing
fails at realize time, but the guest never receives the interrupt.

**BAR window.** The root bridge only advertises `0x60000000`–`0x7FFFFFFF`. Any
BAR the guest places elsewhere either fails allocation or, if below the window,
can shadow other MMIO once `BRIDGEEN` is set. Do not add a 64-bit prefetch
window — the model has no support for it.

**XDMA scope (resolved).** "XDMA" here means the **Xilinx AXI-to-PCIe host
bridge**, so `xilinx-pcie-host` is the correct model and the board work in this
plan is complete. The separate Xilinx XDMA PCIe DMA *endpoint* (`xlnx,xdma-*`
with descriptor rings, H2C/C2H engines, BAR0/1, MSI-X) is **out of scope** and
has no model in this tree; do not add a `xlnx,xdma-*` node to the FDT, because
Linux would probe a driver for a device QEMU does not instantiate.

Consequence: the guest sees a PCIe root complex with one root port
(`xilinx-pcie-root`, vendor `0x10ee` device `0x7021`) and whatever endpoint the
user attaches with `-device`. There is no DMA engine behind it, so a guest
driver that expects to find an XDMA function will not bind.

---

## 10. Suggested phases

1. **Skeleton boots** — machine, RAM at `0x80000000`, N2 CPU, UART, GIC rev 3,
   minimal FDT, `-kernel` payload prints to serial. Validate with `dumpdtb` +
   `dtc`. While the GIC is rev 3 (no `has-lpi`), `gicv3_redist_size()` is
   `GICV3_REDIST_SIZE` (`0x20000`), so the capacity calculation and the mapped
   GICR window are half of §4.4's; phase 2 doubles both when it switches to
   rev 4 + `has-lpi`.
2. **GICv4 + ITS** — revision 4, `has-lpi`, `sysmem`, GICR capacity fix, GICD at
   `0x0E000000`, ITS at `0x0E040000`, GICR at `0x0E080000`, `msi-map` in FDT.
   Validate GICD/GITS/GICR reads in qtest.
3. **PCIe (AXI-PCIe host bridge)** — `xilinx-pcie-host` at ECAM `0x40000000` /
   BAR `0x60000000`, INTID 65, `e1000e`/`virtio-blk-pci` enumeration,
   `BRIDGEEN` visibility. Add the ECAM qtest. This completes the "XDMA"
   requirement; no DMA-engine model is in scope.
4. **MIG** — no code: the RAM mapped in phase 1 is the region at `0x80000000`.
   Only document the absence of a register block. Add a real model later only if
   firmware needs one, at a base other than `0x80000000`.
5. **Hardening** — optional `-bios` ROM at `0x0`, functional test with a real
   rootfs, optional real MIG register model, docs + MAINTAINERS.

Each phase should end with `scripts/checkpatch.pl --terse -f hw/arm/n2-ref.c`
clean (warnings on copied code are acceptable, errors are not) and
`make check-qtest-aarch64`.

---

## Open questions

1. ~~UART address~~ — **resolved: `0x0C000000` (192 MiB)**. No map change needed.
2. ~~"XDMA" scope~~ — **resolved: AXI-PCIe host bridge**, modelled by
   `xilinx-pcie-host`. No DMA-engine model required.
3. ~~GICD/GICR/ITS bases~~ — **resolved: GICD `0x0E000000`, ITS `0x0E040000`,
   GICR `0x0E080000`**, all 64 KiB-aligned and non-overlapping (§2).
4. ~~MIG base meaning~~ — **resolved: `0x80000000` is just the RAM region
   base.** No MIG device, no register block, no MIG SPI (§0, §4.6).
