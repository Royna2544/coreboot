/* SPDX-License-Identifier: GPL-2.0-only */

#define __SIMPLE_DEVICE__

#include <bootstate.h>
#include <console/console.h>
#include <device/device.h>
#include <device/pci_ops.h>
#include <northbridge/intel/x4x/x4x.h>

#define EHCI PCI_DEV(0, 0x1d, 7)

/*
 * The vendor BIOS (awardext.rom table at cs:0x9a87) sets EHCI 0xfc bit 7 in
 * addition to the bits the common ICH7 EHCI init programs.
 */
static void g31mx_usb_final(void *unused)
{
	pci_or_config8(EHCI, 0xfc, 1 << 7);
}

BOOT_STATE_INIT_ENTRY(BS_DEV_INIT, BS_ON_EXIT, g31mx_usb_final, NULL);

static void mainboard_final(void *unused)
{
	const unsigned int cpus = dev_count_cpu();

	/*
	 * Award's late chipset hook (awardext.rom 0xa07d) writes the number
	 * of initialized APs to MCHBAR 0x40[5:3], after CPU discovery.
	 */
	if (!cpus || cpus > 8)
		die("G31MX: CPU count %u does not fit the chipset topology field\n", cpus);
	mchbar_clrsetbits8(0x40, 0x38, (cpus - 1) << 3);
}

struct chip_operations mainboard_ops = {
	.final = mainboard_final,
};
