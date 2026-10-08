/* SPDX-License-Identifier: GPL-2.0-only */

#include <bootblock_common.h>
#include <console/console.h>
#include <device/pci_ops.h>
#include <northbridge/intel/x4x/x4x.h>
#include <superio/ite/common/ite.h>
#include <superio/ite/it8718f/it8718f.h>
#include "stock_preinit.h"

#define SERIAL_DEV PNP_DEV(0x2e, IT8718F_SP1)

void bootblock_mainboard_early_init(void)
{
	ite_enable_serial(SERIAL_DEV, CONFIG_TTYS0_BASE);
}

void mb_pre_raminit_setup(int s3_resume)
{
	if (CONFIG(G31MX_USE_IGFX) &&
	    (pci_read_config32(HOST_BRIDGE, D0F0_CAPID0 + 4) & (1U << 14)))
		die_with_post_code(0xc8, "G31MX: integrated graphics unavailable\n");

	g31mx_stock_preinit();

	/*
	 * The vendor MRC runs with the IGD and PEG both enabled (DEVEN 0x1b);
	 * the unused one is switched off after RAM init.
	 */
	if (CONFIG(G31MX_USE_IGFX) &&
	    ((pci_read_config16(HOST_BRIDGE, D0F0_GGC) & 0x3f2) != 0x130 ||
	     pci_read_config32(PCI_DEV(0, 2, 0), 0) != 0x29c28086))
		die_with_post_code(0xc9, "G31MX: integrated graphics control failed\n");
}

void mb_get_spd_map(u8 spd_map[4])
{
	spd_map[0] = 0x50;
	spd_map[2] = 0x52;
}
