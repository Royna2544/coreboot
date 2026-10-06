/* SPDX-License-Identifier: GPL-2.0-only */

#define __SIMPLE_DEVICE__

#include <arch/cpuid.h>
#include <bootstate.h>
#include <console/console.h>
#include <device/device.h>
#include <device/pci_ops.h>
#include <northbridge/intel/x4x/x4x.h>

#define EHCI PCI_DEV(0, 0x1d, 7)

void g31mx_smbios_init(struct device *dev);

static void mainboard_enable(struct device *dev)
{
	if (CONFIG(GENERATE_SMBIOS_TABLES))
		g31mx_smbios_init(dev);
}

/*
 * The vendor BIOS (awardext.rom table at cs:0x9a87) sets EHCI 0xfc bit 7 in
 * addition to the bits the common ICH7 EHCI init programs.
 */
static void g31mx_usb_final(void *unused)
{
	pci_or_config8(EHCI, 0xfc, 1 << 7);
}

BOOT_STATE_INIT_ENTRY(BS_DEV_INIT, BS_ON_EXIT, g31mx_usb_final, NULL);

/* Number of logical CPUs sharing the L2 cache, from CPUID leaf 4. */
static unsigned int l2_sharing(void)
{
	struct cpuid_result r;
	unsigned int i;

	for (i = 0; i < 8; i++) {
		r = cpuid_ext(4, i);
		if (!(r.eax & 0x1f))
			break;
		if (((r.eax >> 5) & 7) == 2)
			return ((r.eax >> 14) & 0xfff) + 1;
	}
	return 1;
}

static void mainboard_final(void *unused)
{
	const unsigned int cpus = dev_count_cpu();
	const unsigned int share = l2_sharing();
	unsigned int dies;

	/*
	 * The (G)MCH counts Stop-Grant cycles and forwards only the last one
	 * to the ICH7 during Sx entry (ICH7 datasheet 5.13.2.2); MCHBAR
	 * 0x40[5:3] holds that count minus one. The vendor BIOS writes the
	 * number of APs there (awardext.rom 0xa07d), but with coreboot's
	 * MSR_PKG_CST_CONFIG_CONTROL setup (bit 9 clear) each die issues a
	 * single Stop-Grant. With the vendor count, S5 entry stalls after
	 * STPCLK# and the board stays powered.
	 */
	if (!cpus || share > cpus || cpus % share)
		die("G31MX: %u CPUs do not divide into dies of %u\n", cpus, share);
	dies = cpus / share;
	if (dies > 8)
		die("G31MX: %u dies do not fit the Stop-Grant count\n", dies);
	mchbar_clrsetbits8(0x40, 0x38, (dies - 1) << 3);
}

struct chip_operations mainboard_ops = {
	.enable_dev = mainboard_enable,
	.final = mainboard_final,
};
