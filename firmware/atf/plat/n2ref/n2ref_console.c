/*
 * Copyright (c) 2026, n2-ref port
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <platform_def.h>

#include <drivers/console.h>
#include <drivers/ti/uart/uart_16550.h>

static console_t console;

void qemu_console_init(void)
{
	(void)console_16550_register(PLAT_QEMU_BOOT_UART_BASE,
				     PLAT_QEMU_BOOT_UART_CLK_IN_HZ,
				     PLAT_QEMU_CONSOLE_BAUDRATE, &console);

	console_set_scope(&console, CONSOLE_FLAG_BOOT |
			  CONSOLE_FLAG_RUNTIME);
}
