/*
 * Neoverse-N2 reference board emulation
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Board layout (CPU physical addresses):
 *
 *   0x0000_0000  boot RAM (4 MiB, only with secure=on or -bios)
 *   0x0400_0000  secure SRAM (1 MiB, secure=on only)
 *   0x0600_0000  non-secure SRAM (512 KiB, secure=on only)
 *   0x0C00_0000  8250 UART (0x100 bytes, regshift 2)
 *   0x0C01_0000  PL031 RTC (4 KiB)
 *   0x0E00_0000  GIC distributor
 *   0x0E04_0000  GIC ITS
 *   0x0E08_0000  GIC redistributors (8 x GICv4 frames)
 *   0x4000_0000  PCIe ECAM (256 MiB)
 *   0x6000_0000  PCIe MMIO/BAR window (512 MiB)
 *   0x8000_0000  RAM (2 GiB)
 *
 * Interrupts (the FDT and the GIC qdev input lines are both indexed by
 * SPI, i.e. INTID - 32):
 *
 *   SPI 1   INTID 33   8250 UART
 *   SPI 33  INTID 65   Xilinx AXI-PCIe INTx
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/cutils.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/cpus.h"
#include "system/device_tree.h"
#include "system/kvm.h"
#include "system/numa.h"
#include "system/system.h"
#include "exec/hwaddr.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/fdt.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/intc/arm_gicv3_its_common.h"
#include "hw/pci-host/xilinx-pcie.h"
#include "hw/pci/pci.h"
#include "hw/rtc/pl031.h"
#include "net/net.h"
#include "qobject/qlist.h"
#include "qom/object.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-features.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"

#define TYPE_N2_REF_MACHINE MACHINE_TYPE_NAME("n2-ref")
OBJECT_DECLARE_SIMPLE_TYPE(N2RefMachineState, N2_REF_MACHINE)

/* Memory map. */
#define N2_REF_BOOTRAM_BASE   0x00000000ULL
#define N2_REF_BOOTRAM_SIZE   (4 * MiB)
#define N2_REF_SRAM_BASE      0x04000000ULL
#define N2_REF_SRAM_SIZE      (1 * MiB)
#define N2_REF_NSRAM_BASE     0x06000000ULL
#define N2_REF_NSRAM_SIZE     (512 * KiB)
#define N2_REF_RAM_BASE       0x80000000ULL
#define N2_REF_RAM_SIZE_MAX   (2 * GiB)

/* Optional high DRAM window described by the reference platform DTS. */
#define N2_REF_HIGHRAM_BASE   0x900000000ULL
#define N2_REF_UART_BASE      0x0C000000ULL
#define N2_REF_UART_SIZE      0x00000100ULL
#define N2_REF_UART_REGSHIFT  2
#define N2_REF_RTC_BASE       0x0C010000ULL
#define N2_REF_RTC_SIZE       0x00001000ULL
#define N2_REF_GICD_BASE      0x0E000000ULL
#define N2_REF_GICD_SIZE      0x00010000ULL
#define N2_REF_GITS_BASE      0x0E040000ULL
#define N2_REF_GITS_SIZE      0x00020000ULL
#define N2_REF_GICR_BASE      0x0E080000ULL
#define N2_REF_GICR_SIZE      0x00200000ULL
#define N2_REF_PCIE_ECAM_BASE 0x40000000ULL
#define N2_REF_PCIE_ECAM_SIZE 0x10000000ULL
#define N2_REF_PCIE_MMIO_BASE 0x60000000ULL
#define N2_REF_PCIE_MMIO_SIZE 0x20000000ULL

/*
 * GIC interrupt IDs.  The board spec numbers lines by INTID; both the
 * device tree and qdev_get_gpio_in() index SPIs, which is INTID - 32.
 */
#define N2_REF_UART_INTID     33
#define N2_REF_RTC_INTID      34
#define N2_REF_PCIE_INTID     65
#define N2_REF_UART_SPI       (N2_REF_UART_INTID - 32)
#define N2_REF_RTC_SPI        (N2_REF_RTC_INTID - 32)
#define N2_REF_PCIE_SPI       (N2_REF_PCIE_INTID - 32)

#define N2_REF_GIC_NUM_SPI    256   /* SPIs 0..255 */
#define N2_REF_GIC_NUM_IRQ    (N2_REF_GIC_NUM_SPI + GIC_INTERNAL)
#define N2_REF_MAX_CPUS       8     /* N2_REF_GICR_SIZE / GICV4_REDIST_SIZE */

/* Armv8.6 mandates a 1 GHz generic timer. */
#define N2_REF_GTIMER_HZ      1000000000

struct N2RefMachineState {
    MachineState parent;

    struct arm_boot_info bootinfo;
    DeviceState *gic;
    DeviceState *pcie;
    uint32_t gic_phandle;
    uint32_t its_phandle;
    int fdt_size;
    bool secure;
    uint64_t highram_size;
    MemoryRegion bootram;
    MemoryRegion sram;
    MemoryRegion nsram;
    MemoryRegion highram;
};

static uint64_t n2_ref_cpu_mp_affinity(int idx)
{
    return arm_build_mp_affinity(idx, ARM_DEFAULT_CPUS_PER_CLUSTER);
}

/* ---- Machine properties ----------------------------------------------- */

static bool n2_ref_get_secure(Object *obj, Error **errp)
{
    return N2_REF_MACHINE(obj)->secure;
}

static void n2_ref_set_secure(Object *obj, bool value, Error **errp)
{
    N2_REF_MACHINE(obj)->secure = value;
}

static void n2_ref_get_highram_size(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    uint64_t value = N2_REF_MACHINE(obj)->highram_size;

    visit_type_size(v, name, &value, errp);
}

static void n2_ref_set_highram_size(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    N2_REF_MACHINE(obj)->highram_size = value;
}

/* ---- GIC and ITS ------------------------------------------------------ */

static void n2_ref_create_its(N2RefMachineState *s)
{
    DeviceState *dev = qdev_new(its_class_name());

    object_property_set_link(OBJECT(dev), "parent-gicv3", OBJECT(s->gic),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, N2_REF_GITS_BASE);
}

static void n2_ref_create_gic(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    unsigned int smp_cpus = ms->smp.cpus;
    SysBusDevice *gicbusdev;
    QList *redist_region_count;
    uint32_t capacity;
    int i;

    /*
     * GIC-700 is a GICv4.1 implementation.  QEMU models GICv4 (revision 4)
     * plus vLPI support; the v4.1-only features (vSGI, VCONFADDR,
     * GICD_TYPER2) are not emulated.
     */
    s->gic = qdev_new(gicv3_class_name());
    qdev_prop_set_uint32(s->gic, "revision", 4);
    qdev_prop_set_uint32(s->gic, "num-cpu", smp_cpus);
    qdev_prop_set_uint32(s->gic, "num-irq", N2_REF_GIC_NUM_IRQ);
    qdev_prop_set_bit(s->gic, "has-security-extensions", s->secure);
    qdev_prop_set_bit(s->gic, "has-lpi", true);

    /*
     * With has-lpi and revision > 3 the redistributor frame is
     * GICV4_REDIST_SIZE (256 KiB), so the 2 MiB window holds 8 CPUs.
     */
    capacity = N2_REF_GICR_SIZE / GICV4_REDIST_SIZE;
    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, MIN(smp_cpus, capacity));
    qdev_prop_set_array(s->gic, "redist-region-count", redist_region_count);

    object_property_set_link(OBJECT(s->gic), "sysmem",
                             OBJECT(get_system_memory()), &error_fatal);

    gicbusdev = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, N2_REF_GICD_BASE);
    sysbus_mmio_map(gicbusdev, 1, N2_REF_GICR_BASE);

    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        /*
         * The GIC's input GPIO array is [0..num_spis-1] for SPIs, followed
         * by 32 PPIs per CPU.  The base is therefore num_spis, not num_irq.
         */
        int intidbase = N2_REF_GIC_NUM_SPI + i * GIC_INTERNAL;
        int irq;

        /*
         * The timer PPI numbers are not contiguous, so use the same table
         * as sbsa-ref.  The secure lines have no source because EL3 is
         * disabled, but connecting them is harmless.
         */
        const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
            [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
            [GTIMER_S_EL2_PHYS] = ARCH_TIMER_S_EL2_IRQ,
            [GTIMER_S_EL2_VIRT] = ARCH_TIMER_S_EL2_VIRT_IRQ,
        };

        for (irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(s->gic,
                                                   intidbase + timer_irq[irq]));
        }

        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(s->gic,
                                                     intidbase
                                                     + ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(s->gic,
                                                     intidbase
                                                     + VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }

    n2_ref_create_its(s);
}

/* ---- Devices ---------------------------------------------------------- */

static void n2_ref_create_uart(N2RefMachineState *s)
{
    /*
     * The reference platform puts the 16550 on a 32-bit AXI4-Lite bus, so
     * registers are 4 bytes apart (reg-shift 2, reg-io-width 4).  That also
     * matches the reference bootargs, which use
     * earlycon=uart8250,mmio32,0x0C000000.
     */
    serial_mm_init(get_system_memory(), N2_REF_UART_BASE,
                   N2_REF_UART_REGSHIFT,
                   qdev_get_gpio_in(s->gic, N2_REF_UART_SPI),
                   115200, serial_hd(0), DEVICE_LITTLE_ENDIAN);
}

static void n2_ref_create_rtc(N2RefMachineState *s)
{
    /*
     * UEFI's runtime services require an RTC arch protocol.  The PL031 is
     * a simple MMIO device and is already supported by the ArmVirtPkg
     * PL031 FDT client library used by the EDK2 build.
     */
    sysbus_create_simple(TYPE_PL031, N2_REF_RTC_BASE,
                         qdev_get_gpio_in(s->gic, N2_REF_RTC_SPI));
}

static void n2_ref_create_pcie(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    DeviceState *dev = qdev_new(TYPE_XILINX_PCIE_HOST);
    MemoryRegion *cfg, *mmio;
    PCIHostState *pci;

    qdev_prop_set_uint32(dev, "bus_nr", 0);
    qdev_prop_set_uint64(dev, "cfg_base", N2_REF_PCIE_ECAM_BASE);
    qdev_prop_set_uint64(dev, "cfg_size", N2_REF_PCIE_ECAM_SIZE);
    qdev_prop_set_uint64(dev, "mmio_base", N2_REF_PCIE_MMIO_BASE);
    qdev_prop_set_uint64(dev, "mmio_size", N2_REF_PCIE_MMIO_SIZE);
    qdev_prop_set_bit(dev, "link_up", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    cfg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        N2_REF_PCIE_ECAM_BASE, cfg, 0);

    /*
     * The BAR container is mapped at address 0 and stays disabled until
     * the guest sets ROOTCFG_RPSCR.BRIDGEEN; the model's MMIO region is
     * absolute-addressed, so this is a 1:1 mapping.
     */
    mmio = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 1);
    memory_region_add_subregion_overlap(get_system_memory(), 0, mmio, 0);

    qdev_connect_gpio_out_named(dev, "interrupt_out", 0,
                                qdev_get_gpio_in(s->gic, N2_REF_PCIE_SPI));

    pci = PCI_HOST_BRIDGE(dev);
    pci_init_nic_devices(pci->bus, mc->default_nic);

    s->pcie = dev;
}

/* ---- Device tree ------------------------------------------------------ */

static void n2_ref_fdt_add_gic(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    void *fdt = ms->fdt;
    char *gic_name, *its_name;

    s->gic_phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_setprop_cell(fdt, "/", "interrupt-parent", s->gic_phandle);

    gic_name = g_strdup_printf("/intc@%" PRIx64,
                               (uint64_t)N2_REF_GICD_BASE);
    qemu_fdt_add_subnode(fdt, gic_name);
    qemu_fdt_setprop_string(fdt, gic_name, "compatible", "arm,gic-v3");
    qemu_fdt_setprop(fdt, gic_name, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, gic_name, "#interrupt-cells", 3);
    qemu_fdt_setprop_cell(fdt, gic_name, "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, gic_name, "#size-cells", 2);
    qemu_fdt_setprop(fdt, gic_name, "ranges", NULL, 0);
    /*
     * "#redistributor-regions" is optional and defaults to 1; leave it out
     * so firmware that does not implement multiple GICR regions is happy.
     */
    qemu_fdt_setprop_sized_cells(fdt, gic_name, "reg",
                                 2, (uint64_t)N2_REF_GICD_BASE,
                                 2, (uint64_t)N2_REF_GICD_SIZE,
                                 2, (uint64_t)N2_REF_GICR_BASE,
                                 2, (uint64_t)N2_REF_GICR_SIZE);
    qemu_fdt_setprop_cells(fdt, gic_name, "interrupts",
                           GIC_FDT_IRQ_TYPE_PPI,
                           INTID_TO_PPI(ARCH_GIC_MAINT_IRQ),
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(fdt, gic_name, "phandle", s->gic_phandle);

    s->its_phandle = qemu_fdt_alloc_phandle(fdt);
    its_name = g_strdup_printf("%s/its@%" PRIx64, gic_name,
                               (uint64_t)N2_REF_GITS_BASE);
    qemu_fdt_add_subnode(fdt, its_name);
    qemu_fdt_setprop_string(fdt, its_name, "compatible", "arm,gic-v3-its");
    qemu_fdt_setprop(fdt, its_name, "msi-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, its_name, "#msi-cells", 1);
    qemu_fdt_setprop_sized_cells(fdt, its_name, "reg",
                                 2, (uint64_t)N2_REF_GITS_BASE,
                                 2, (uint64_t)N2_REF_GITS_SIZE);
    qemu_fdt_setprop_cell(fdt, its_name, "phandle", s->its_phandle);

    g_free(its_name);
    g_free(gic_name);
}

static void n2_ref_fdt_add_cpus(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    void *fdt = ms->fdt;
    unsigned int smp_cpus = ms->smp.cpus;
    int cpu;

    /*
     * arm_build_mp_affinity() only packs Aff0/Aff1, and with max_cpus = 8
     * every MPIDR is 0..7, so a single address cell is enough.
     */
    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);

    for (cpu = smp_cpus - 1; cpu >= 0; cpu--) {
        char *nodename = g_strdup_printf("/cpus/cpu@%d", cpu);
        ARMCPU *armcpu = ARM_CPU(qemu_get_cpu(cpu));
        CPUState *cs = CPU(armcpu);

        qemu_fdt_add_subnode(fdt, nodename);
        qemu_fdt_setprop_string(fdt, nodename, "device_type", "cpu");
        qemu_fdt_setprop_string(fdt, nodename, "compatible",
                                armcpu->dtb_compatible);

        if (s->bootinfo.psci_conduit != QEMU_PSCI_CONDUIT_DISABLED &&
            smp_cpus > 1) {
            qemu_fdt_setprop_string(fdt, nodename, "enable-method", "psci");
        }

        qemu_fdt_setprop_cell(fdt, nodename, "reg",
                              n2_ref_cpu_mp_affinity(cpu));

        if (ms->possible_cpus->cpus[cs->cpu_index].props.has_node_id) {
            qemu_fdt_setprop_cell(fdt, nodename, "numa-node-id",
                ms->possible_cpus->cpus[cs->cpu_index].props.node_id);
        }

        g_free(nodename);
    }
}

static void n2_ref_fdt_add_timer(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    void *fdt = ms->fdt;
    ARMCPU *armcpu = ARM_CPU(qemu_get_cpu(0));
    const char compat[] = "arm,armv8-timer\0arm,armv7-timer";
    uint32_t irqflags = GIC_FDT_IRQ_FLAGS_LEVEL_HI;

    qemu_fdt_add_subnode(fdt, "/timer");
    qemu_fdt_setprop(fdt, "/timer", "compatible", compat, sizeof(compat));
    qemu_fdt_setprop(fdt, "/timer", "always-on", NULL, 0);

    /*
     * Neoverse-N2 advertises FEAT_VHE, so the EL2 virtual timer exists and
     * must be described too -- the board wires it up in n2_ref_create_gic().
     */
    if (arm_feature(&armcpu->env, ARM_FEATURE_EL2) &&
        cpu_isar_feature(aa64_vh, armcpu)) {
        qemu_fdt_setprop_cells(fdt, "/timer", "interrupts",
                               GIC_FDT_IRQ_TYPE_PPI,
                               INTID_TO_PPI(ARCH_TIMER_S_EL1_IRQ), irqflags,
                               GIC_FDT_IRQ_TYPE_PPI,
                               INTID_TO_PPI(ARCH_TIMER_NS_EL1_IRQ), irqflags,
                               GIC_FDT_IRQ_TYPE_PPI,
                               INTID_TO_PPI(ARCH_TIMER_VIRT_IRQ), irqflags,
                               GIC_FDT_IRQ_TYPE_PPI,
                               INTID_TO_PPI(ARCH_TIMER_NS_EL2_IRQ), irqflags,
                               GIC_FDT_IRQ_TYPE_PPI,
                               INTID_TO_PPI(ARCH_TIMER_NS_EL2_VIRT_IRQ),
                               irqflags);
    } else {
        qemu_fdt_setprop_cells(fdt, "/timer", "interrupts",
                               GIC_FDT_IRQ_TYPE_PPI,
                               INTID_TO_PPI(ARCH_TIMER_S_EL1_IRQ), irqflags,
                               GIC_FDT_IRQ_TYPE_PPI,
                               INTID_TO_PPI(ARCH_TIMER_NS_EL1_IRQ), irqflags,
                               GIC_FDT_IRQ_TYPE_PPI,
                               INTID_TO_PPI(ARCH_TIMER_VIRT_IRQ), irqflags,
                               GIC_FDT_IRQ_TYPE_PPI,
                               INTID_TO_PPI(ARCH_TIMER_NS_EL2_IRQ), irqflags);
    }
}

static void n2_ref_fdt_add_pmu(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    void *fdt = ms->fdt;
    ARMCPU *armcpu = ARM_CPU(first_cpu);
    const char compat[] = "arm,armv8-pmuv3";

    if (!arm_feature(&armcpu->env, ARM_FEATURE_PMU)) {
        return;
    }

    qemu_fdt_add_subnode(fdt, "/pmu");
    qemu_fdt_setprop(fdt, "/pmu", "compatible", compat, sizeof(compat));
    qemu_fdt_setprop_cells(fdt, "/pmu", "interrupts",
                           GIC_FDT_IRQ_TYPE_PPI,
                           INTID_TO_PPI(VIRTUAL_PMU_IRQ),
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
}

static void n2_ref_fdt_add_uart(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    void *fdt = ms->fdt;
    char *nodename = g_strdup_printf("/serial@%" PRIx64,
                                     (uint64_t)N2_REF_UART_BASE);

    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_string(fdt, nodename, "compatible", "ns16550a");
    /*
     * serial_mm_init() maps 8 << regshift bytes, i.e. 32 bytes here, but
     * the reference platform reserves a 256-byte page for the UART, so
     * describe that.  Accesses past the mapped 32 bytes fall through to
     * the unassigned-memory handler and read as zero.
     */
    qemu_fdt_setprop_sized_cells(fdt, nodename, "reg",
                                 2, (uint64_t)N2_REF_UART_BASE,
                                 2, (uint64_t)N2_REF_UART_SIZE);
    qemu_fdt_setprop_cell(fdt, nodename, "reg-shift",
                          N2_REF_UART_REGSHIFT);
    qemu_fdt_setprop_cell(fdt, nodename, "reg-io-width", 4);
    qemu_fdt_setprop_cells(fdt, nodename, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, N2_REF_UART_SPI,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(fdt, nodename, "clock-frequency", 100000000);
    qemu_fdt_setprop_string(fdt, nodename, "status", "okay");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", nodename);

    g_free(nodename);
}

static void n2_ref_fdt_add_rtc(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    void *fdt = ms->fdt;
    char *nodename = g_strdup_printf("/rtc@%" PRIx64,
                                     (uint64_t)N2_REF_RTC_BASE);

    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_string(fdt, nodename, "compatible", "arm,pl031");
    qemu_fdt_setprop_sized_cells(fdt, nodename, "reg",
                                 2, (uint64_t)N2_REF_RTC_BASE,
                                 2, (uint64_t)N2_REF_RTC_SIZE);
    qemu_fdt_setprop_cells(fdt, nodename, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, N2_REF_RTC_SPI,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_string(fdt, nodename, "status", "okay");

    g_free(nodename);
}

static void n2_ref_fdt_add_pcie(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    void *fdt = ms->fdt;
    char *nodename;
    uint32_t interrupt_map[4][10];
    int pin, i;

    nodename = g_strdup_printf("/pci@%" PRIx64,
                               (uint64_t)N2_REF_PCIE_ECAM_BASE);
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_string(fdt, nodename, "compatible",
                            "xlnx,axi-pcie-host-1.00.a");
    qemu_fdt_setprop_string(fdt, nodename, "device_type", "pci");
    qemu_fdt_setprop_cell(fdt, nodename, "#address-cells", 3);
    qemu_fdt_setprop_cell(fdt, nodename, "#size-cells", 2);
    qemu_fdt_setprop_cell(fdt, nodename, "#interrupt-cells", 1);
    qemu_fdt_setprop_cell(fdt, nodename, "linux,pci-domain", 0);

    qemu_fdt_setprop_sized_cells(fdt, nodename, "reg",
                                 2, (uint64_t)N2_REF_PCIE_ECAM_BASE,
                                 2, (uint64_t)N2_REF_PCIE_ECAM_SIZE);

    /*
     * ranges: child address (3 cells), parent address (2 cells, because
     * / has #address-cells = 2), size (2 cells) = 7 cells.
     */
    qemu_fdt_setprop_cells(fdt, nodename, "ranges",
                           FDT_PCI_RANGE_MMIO,
                           (uint32_t)(N2_REF_PCIE_MMIO_BASE >> 32),
                           (uint32_t)N2_REF_PCIE_MMIO_BASE,
                           (uint32_t)(N2_REF_PCIE_MMIO_BASE >> 32),
                           (uint32_t)N2_REF_PCIE_MMIO_BASE,
                           (uint32_t)(N2_REF_PCIE_MMIO_SIZE >> 32),
                           (uint32_t)N2_REF_PCIE_MMIO_SIZE);

    qemu_fdt_setprop_cells(fdt, nodename, "bus-range", 0, 0xff);
    qemu_fdt_setprop(fdt, nodename, "dma-coherent", NULL, 0);

    qemu_fdt_setprop_cell(fdt, nodename, "interrupt-parent", s->gic_phandle);
    qemu_fdt_setprop_cells(fdt, nodename, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, N2_REF_PCIE_SPI,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);

    /* Required for MSI/MSI-X: the ITS uses the BDF as the device ID. */
    qemu_fdt_setprop_cells(fdt, nodename, "msi-map",
                           0, s->its_phandle, 0, 0x10000);

    /*
     * Downstream INTx lines.  The xilinx-pcie model collapses all four
     * root-bus pins onto its single interrupt_out line, so every entry
     * routes to the same GIC SPI.  Each entry is 10 cells: 3 child
     * address cells, 1 PCI pin, the GIC phandle, 2 GIC address cells
     * (the GIC node has #address-cells = 2) and the 3-cell GIC
     * interrupt specifier.  This matches the reference platform DTS
     * and hw/arm/virt.c::create_pcie_irq_map().
     */
    for (pin = 0; pin < 4; pin++) {
        uint32_t map[] = {
            0, 0, 0,                        /* devfn 0, any function */
            pin + 1,                        /* INTA..INTD */
            s->gic_phandle, 0, 0,           /* parent: the GIC */
            GIC_FDT_IRQ_TYPE_SPI, N2_REF_PCIE_SPI,
            GIC_FDT_IRQ_FLAGS_LEVEL_HI,
        };

        for (i = 0; i < ARRAY_SIZE(map); i++) {
            interrupt_map[pin][i] = cpu_to_be32(map[i]);
        }
    }

    qemu_fdt_setprop_cells(fdt, nodename, "interrupt-map-mask", 0, 0, 0, 7);
    qemu_fdt_setprop(fdt, nodename, "interrupt-map",
                     interrupt_map, sizeof(interrupt_map));

    g_free(nodename);
}

static void n2_ref_create_fdt(N2RefMachineState *s)
{
    MachineState *ms = MACHINE(s);
    void *fdt = create_device_tree(&s->fdt_size);

    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }

    ms->fdt = fdt;

    qemu_fdt_setprop_string(fdt, "/", "compatible", "qemu,n2-ref");
    qemu_fdt_setprop_string(fdt, "/", "model",
                            "QEMU Neoverse-N2 reference board");
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 2);
    qemu_fdt_setprop(fdt, "/", "dma-coherent", NULL, 0);

    /*
     * /chosen must exist before arm_load_kernel() runs; /memory, the
     * bootargs and /psci are added by arm_load_dtb() later.
     */
    qemu_fdt_add_subnode(fdt, "/chosen");

    n2_ref_fdt_add_cpus(s);
    n2_ref_fdt_add_timer(s);
    n2_ref_fdt_add_pmu(s);
    n2_ref_fdt_add_gic(s);
    n2_ref_fdt_add_uart(s);
    n2_ref_fdt_add_rtc(s);
    n2_ref_fdt_add_pcie(s);

    /*
     * Note: arm_load_dtb() discards every /memory* node and regenerates
     * them from machine->ram, so the high DRAM window cannot be described
     * here.  It is mapped for firmware that supplies its own device tree
     * (as the reference platform firmware does); the generated QEMU DT
     * only covers machine->ram.
     */
}

static void *n2_ref_get_dtb(const struct arm_boot_info *info, int *size)
{
    N2RefMachineState *s = container_of(info, N2RefMachineState, bootinfo);

    *size = s->fdt_size;
    return MACHINE(s)->fdt;
}

/* ---- Machine ---------------------------------------------------------- */

static const CPUArchIdList *n2_ref_possible_cpu_arch_ids(MachineState *ms)
{
    unsigned int max_cpus = ms->smp.max_cpus;
    int n;

    if (ms->possible_cpus) {
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (n = 0; n < ms->possible_cpus->len; n++) {
        ms->possible_cpus->cpus[n].type = ms->cpu_type;
        ms->possible_cpus->cpus[n].arch_id = n2_ref_cpu_mp_affinity(n);
        ms->possible_cpus->cpus[n].props.has_thread_id = true;
        ms->possible_cpus->cpus[n].props.thread_id = n;
    }
    return ms->possible_cpus;
}

static void n2_ref_init(MachineState *machine)
{
    N2RefMachineState *s = N2_REF_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    MemoryRegion *sysmem = get_system_memory();
    const CPUArchIdList *possible_cpus;
    unsigned int smp_cpus = machine->smp.cpus;
    int n;

    if (machine->ram_size > N2_REF_RAM_SIZE_MAX) {
        error_report("n2-ref: cannot model more than %s of RAM",
                     size_to_str(N2_REF_RAM_SIZE_MAX));
        exit(1);
    }

    if (machine->smp.max_cpus > N2_REF_MAX_CPUS) {
        error_report("n2-ref: number of CPUs (%u) exceeds the %d supported "
                     "by the GIC redistributor window",
                     machine->smp.max_cpus, N2_REF_MAX_CPUS);
        exit(1);
    }

    if (s->secure && kvm_enabled()) {
        error_report("n2-ref: secure mode requires TCG (KVM cannot "
                     "provide EL3 to the guest)");
        exit(1);
    }

    /* RAM: one region at 0x8000_0000. */
    memory_region_add_subregion(sysmem, N2_REF_RAM_BASE, machine->ram);

    /*
     * The reference platform also has a high DRAM window at 0x9_0000_0000.
     * It is opt-in because it lies outside the 32-bit address space used by
     * the default 2 GiB RAM region and by all the MMIO devices.
     */
    if (s->highram_size) {
        memory_region_init_ram(&s->highram, NULL, "n2-ref.high-ram",
                               s->highram_size, &error_fatal);
        memory_region_add_subregion(sysmem, N2_REF_HIGHRAM_BASE,
                                    &s->highram);
    }

    /*
     * EL3 firmware enters at the AArch64 reset vector, which is 0x0, so
     * secure mode needs memory there.  The same window also gives a plain
     * -bios image an entry point when the machine is not in secure mode.
     */
    if (s->secure || machine->firmware) {
        memory_region_init_ram(&s->bootram, NULL, "n2-ref.boot-ram",
                               N2_REF_BOOTRAM_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, N2_REF_BOOTRAM_BASE, &s->bootram);
    }

    /*
     * The TF-A rdn2 port links BL1's read-write data and BL2/BL31 at the
     * trusted SRAM window and uses a separate non-secure SRAM.  Both
     * windows are free in this memory map, so provide them in secure mode.
     */
    if (s->secure) {
        memory_region_init_ram(&s->sram, NULL, "n2-ref.secure-sram",
                               N2_REF_SRAM_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, N2_REF_SRAM_BASE, &s->sram);

        memory_region_init_ram(&s->nsram, NULL, "n2-ref.ns-sram",
                               N2_REF_NSRAM_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, N2_REF_NSRAM_BASE, &s->nsram);
    }

    if (machine->firmware) {
        g_autofree char *fname = qemu_find_file(QEMU_FILE_TYPE_BIOS,
                                                machine->firmware);

        if (!fname) {
            error_report("n2-ref: could not find firmware '%s'",
                         machine->firmware);
            exit(1);
        }
        if (load_image_targphys(fname, N2_REF_BOOTRAM_BASE,
                                N2_REF_BOOTRAM_SIZE, NULL) < 0) {
            error_report("n2-ref: could not load firmware '%s' at 0x%" PRIx64,
                         machine->firmware, (uint64_t)N2_REF_BOOTRAM_BASE);
            exit(1);
        }
    }

    /* CPUs must exist and be realized before the GIC is created. */
    possible_cpus = mc->possible_cpu_arch_ids(machine);
    for (n = 0; n < possible_cpus->len; n++) {
        Object *cpuobj;
        CPUState *cs;

        if (n >= smp_cpus) {
            break;
        }

        cpuobj = object_new(possible_cpus->cpus[n].type);
        object_property_set_int(cpuobj, "mp-affinity",
                                possible_cpus->cpus[n].arch_id, NULL);

        cs = CPU(cpuobj);
        cs->cpu_index = n;

        numa_cpu_pre_plug(&possible_cpus->cpus[cs->cpu_index], DEVICE(cpuobj),
                          &error_fatal);

        object_property_set_int(cpuobj, "cntfrq", N2_REF_GTIMER_HZ,
                                &error_abort);

        /*
         * Secure mode gives the guest EL3 so external firmware (TF-A) can
         * run.  Without it the guest starts at EL2 and uses QEMU's PSCI.
         */
        object_property_set_bool(cpuobj, "has_el3", s->secure, &error_abort);
        object_property_set_bool(cpuobj, "has_el2", true, &error_abort);

        object_property_set_link(cpuobj, "memory", OBJECT(sysmem),
                                 &error_abort);

        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        object_unref(cpuobj);
    }

    /*
     * In secure mode the firmware at EL3 owns PSCI, so QEMU must not trap
     * SMC.  Leaving the conduit at SMC for a non-secure guest makes QEMU's
     * PSCI implementation behave as an ersatz EL3.
     */
    s->bootinfo.psci_conduit = s->secure ? QEMU_PSCI_CONDUIT_DISABLED
                                         : QEMU_PSCI_CONDUIT_SMC;
    s->bootinfo.firmware_loaded = machine->firmware != NULL;

    n2_ref_create_gic(s);
    n2_ref_create_uart(s);
    n2_ref_create_rtc(s);
    n2_ref_create_pcie(s);
    n2_ref_create_fdt(s);

    s->bootinfo.ram_size = machine->ram_size;
    s->bootinfo.board_id = -1;
    s->bootinfo.loader_start = N2_REF_RAM_BASE;
    s->bootinfo.get_dtb = n2_ref_get_dtb;
    arm_load_kernel(ARM_CPU(first_cpu), machine, &s->bootinfo);
}

static void n2_ref_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("neoverse-n2"),
        NULL,
    };

    mc->init = n2_ref_init;
    mc->desc = "Neoverse-N2 reference board (GIC-700, 8250, Xilinx PCIe)";
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("neoverse-n2");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_id = "n2-ref.ram";
    mc->default_ram_size = N2_REF_RAM_SIZE_MAX;
    mc->max_cpus = N2_REF_MAX_CPUS;
    mc->default_cpus = 4;
    mc->minimum_page_bits = 12;
    mc->possible_cpu_arch_ids = n2_ref_possible_cpu_arch_ids;
    mc->default_nic = "e1000e";

    object_class_property_add_bool(oc, "secure", n2_ref_get_secure,
                                   n2_ref_set_secure);
    object_class_property_set_description(oc, "secure",
        "Enable EL3 and GIC security extensions for external firmware; "
        "QEMU's PSCI implementation is disabled");

    object_class_property_add(oc, "highram-size", "size",
                              n2_ref_get_highram_size,
                              n2_ref_set_highram_size, NULL, NULL);
    object_class_property_set_description(oc, "highram-size",
        "Size of the optional high DRAM region at 0x9_0000_0000, for "
        "firmware that supplies its own device tree (for example 14G)");
}

static const TypeInfo n2_ref_machine_typeinfo = {
    .name = TYPE_N2_REF_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = n2_ref_class_init,
    .instance_size = sizeof(N2RefMachineState),
    .interfaces = aarch64_machine_interfaces,
};

static void n2_ref_machine_init(void)
{
    type_register_static(&n2_ref_machine_typeinfo);
}

type_init(n2_ref_machine_init);
