/*
 * QTest testcases for the n2-ref machine
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

/* Memory map, mirrored from hw/arm/n2-ref.c. */
#define RAM_BASE        0x80000000ULL
#define RAM_LAST_BYTE   (RAM_BASE + 2 * GiB - 1)
#define UART_BASE       0x0C000000ULL
#define GICD_BASE       0x0E000000ULL
#define GITS_BASE       0x0E040000ULL
#define GICR_BASE       0x0E080000ULL
#define PCIE_ECAM_BASE  0x40000000ULL
#define PCIE_MMIO_BASE  0x60000000ULL
#define HIGHRAM_BASE    0x900000000ULL
#define HIGHRAM_SIZE    (16 * MiB)

#define MACHINE_CMD "-machine n2-ref -m 2G -smp 4 -display none -serial none"
#define SECURE_MACHINE_CMD \
    "-machine n2-ref,secure=on -m 2G -smp 4 -display none -serial none"
#define HIGHRAM_MACHINE_CMD \
    "-machine n2-ref,highram-size=16M -m 2G -smp 4 -display none -serial none"

static void test_ram(void)
{
    QTestState *qts = qtest_init(MACHINE_CMD);

    qtest_writeq(qts, RAM_BASE, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, RAM_BASE), ==, 0x0123456789abcdefULL);

    /* Last byte of the 2 GiB region. */
    qtest_writeb(qts, RAM_LAST_BYTE, 0xa5);
    g_assert_cmphex(qtest_readb(qts, RAM_LAST_BYTE), ==, 0xa5);

    qtest_quit(qts);
}

static void test_uart(void)
{
    QTestState *qts = qtest_init(MACHINE_CMD);

    /*
     * The UART uses reg-shift 2 (32-bit register spacing), so LSR is at
     * offset 5 * 4 = 0x14.  Bit 5 (THRE) is set at reset, and the divisor
     * latch is not selected.
     */
    g_assert_cmphex(qtest_readb(qts, UART_BASE + (5 << 2)) & 0x20, ==, 0x20);

    /* Register 0 (THR/RBR) must be writable without aborting. */
    qtest_writeb(qts, UART_BASE, 'A');

    qtest_quit(qts);
}

static void test_gic(void)
{
    QTestState *qts = qtest_init(MACHINE_CMD);

    /* GICD_TYPER.ITLinesNumber for 288 IRQs: 288 / 32 - 1 = 8. */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x4) & 0x1f, ==, 8);

    /* GITS_IIDR is nonzero once the ITS is realized. */
    g_assert_cmphex(qtest_readl(qts, GITS_BASE + 0x4), !=, 0);

    /*
     * GICR_TYPER low word: PLPIS (bit 0) and VLPIS (bit 1) must both be
     * set, which is what makes the 256 KiB GICv4 frame size correct.
     */
    g_assert_cmphex(qtest_readl(qts, GICR_BASE + 0x8) & 0x3, ==, 0x3);

    qtest_quit(qts);
}

static void test_secure(void)
{
    QTestState *qts = qtest_init(MACHINE_CMD);

    /*
     * Without secure=on the GIC has a single security state:
     * GICD_CTLR.DS is set and GICD_TYPER.SecurityExtn is zero.
     */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE) & (1 << 6), ==, 1 << 6);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x4) & (1 << 10), ==, 0);

    qtest_quit(qts);

    qts = qtest_init(SECURE_MACHINE_CMD);

    /*
     * With EL3 enabled the GIC supports both security states: the
     * non-secure view of GICD_CTLR has DS clear and GICD_TYPER reports
     * SecurityExtn.
     */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE) & (1 << 6), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x4) & (1 << 10), ==, 1 << 10);

    /*
     * Secure mode maps the 4 MiB boot RAM at 0x0 for the reset vector and
     * the TF-A SRAM windows at 0x04000000 and 0x06000000.
     */
    qtest_writeq(qts, 0x0, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, 0x0), ==, 0x0123456789abcdefULL);
    qtest_writeq(qts, 0x04000000, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, 0x04000000), ==, 0x0123456789abcdefULL);
    qtest_writeq(qts, 0x06000000, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, 0x06000000), ==, 0x0123456789abcdefULL);

    qtest_quit(qts);
}

static void test_highram(void)
{
    QTestState *qts = qtest_init(HIGHRAM_MACHINE_CMD);

    qtest_writeq(qts, HIGHRAM_BASE, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, HIGHRAM_BASE), ==,
                    0x0123456789abcdefULL);

    qtest_writeq(qts, HIGHRAM_BASE + HIGHRAM_SIZE - 8,
                 0xa5a5a5a5a5a5a5a5ULL);
    g_assert_cmphex(qtest_readq(qts, HIGHRAM_BASE + HIGHRAM_SIZE - 8), ==,
                    0xa5a5a5a5a5a5a5a5ULL);

    qtest_quit(qts);
}

static void test_pcie(void)
{
    QTestState *qts = qtest_init(MACHINE_CMD);

    /*
     * Vendor/device ID of the xilinx-pcie root port (10ee:7021).  ECAM is
     * readable without BRIDGEEN; only the BAR container is gated.
     */
    g_assert_cmphex(qtest_readl(qts, PCIE_ECAM_BASE), ==, 0x702110ee);

    /*
     * The BAR window is not backed until the guest enables the bridge, so
     * reads there return 0 through the unassigned-memory handler.
     */
    g_assert_cmphex(qtest_readl(qts, PCIE_MMIO_BASE), ==, 0);

    qtest_quit(qts);
}

static void test_cpus(void)
{
    QTestState *qts = qtest_init(MACHINE_CMD);
    QDict *resp;
    QList *cpus;

    resp = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert(qdict_haskey(resp, "return"));
    cpus = qdict_get_qlist(resp, "return");
    g_assert_cmpint(qlist_size(cpus), ==, 4);
    qobject_unref(resp);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("n2-ref/ram", test_ram);
    qtest_add_func("n2-ref/uart", test_uart);
    qtest_add_func("n2-ref/gic", test_gic);
    qtest_add_func("n2-ref/secure", test_secure);
    qtest_add_func("n2-ref/highram", test_highram);
    qtest_add_func("n2-ref/pcie", test_pcie);
    qtest_add_func("n2-ref/cpus", test_cpus);

    return g_test_run();
}
