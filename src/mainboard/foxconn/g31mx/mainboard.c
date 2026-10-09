/* SPDX-License-Identifier: GPL-2.0-only */

#define __SIMPLE_DEVICE__

#include <arch/cpuid.h>
#include <arch/io.h>
#include <arch/ioapic.h>
#include <bootstate.h>
#include <console/console.h>
#include <delay.h>
#include <device/device.h>
#include <device/mmio.h>
#include <device/pci_def.h>
#include <device/pci_ops.h>
#include <device/pnp.h>
#include <northbridge/intel/x4x/x4x.h>
#include <option.h>
#include <superio/hwm5_conf.h>
#include <superio/ite/common/env_ctrl.h>
#include <superio/ite/it8718f/it8718f.h>

#define EHCI PCI_DEV(0, 0x1d, 7)

void g31mx_smbios_init(struct device *dev);

static void mainboard_enable(struct device *dev)
{
	/*
	 * Give the ICH7 I/O APIC an ID above the CPUs' local APIC IDs
	 * (0 .. MAX_CPUS - 1), as the vendor MADT does (ID 4). The common
	 * GSI0 setup would program 0, which the boot CPU already uses.
	 * IOAPIC_USE_PRESET_ID keeps this ID.
	 */
	ioapic_setup_gsi0_id(IO_APIC_ADDR, CONFIG_MAX_CPUS);

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

#define EHCI_USBCMD		0x00
#define  EHCI_USBCMD_RS		(1 << 0)
#define  EHCI_USBCMD_HCRESET	(1 << 1)
#define EHCI_USBSTS		0x04
#define  EHCI_USBSTS_HCHALTED	(1 << 12)
#define UHCI_USBCMD		0x00
#define  UHCI_USBCMD_GRESET	(1 << 2)

/*
 * The USB ports and attached devices keep their state across a warm reset
 * (the ICH7 USB port logic is in the resume well). A device left configured
 * by the previous boot then does not answer the payload's port reset. The
 * vendor BIOS drives reset on all UHCI ports itself (awardeyt.rom 0x8d48).
 * Reset EHCI, which routes every port back to its UHCI companion, then
 * drive a USB global reset on all UHCI controllers for TDRSTR (50 ms).
 */
static void g31mx_usb_bus_reset(void *unused)
{
	static const pci_devfn_t uhci[] = {
		PCI_DEV(0, 0x1d, 0), PCI_DEV(0, 0x1d, 1),
		PCI_DEV(0, 0x1d, 2), PCI_DEV(0, 0x1d, 3),
	};
	const uintptr_t ehci = pci_read_config32(EHCI, PCI_BASE_ADDRESS_0) & ~0xf;
	uintptr_t op;
	unsigned int i;
	int t;

	if (ehci && (pci_read_config16(EHCI, PCI_COMMAND) & PCI_COMMAND_MEMORY)) {
		op = ehci + read8p(ehci);
		write32p(op + EHCI_USBCMD, read32p(op + EHCI_USBCMD) & ~EHCI_USBCMD_RS);
		for (t = 0; t < 20 && !(read32p(op + EHCI_USBSTS) & EHCI_USBSTS_HCHALTED); t++)
			udelay(100);
		write32p(op + EHCI_USBCMD, EHCI_USBCMD_HCRESET);
		for (t = 0; t < 250 && (read32p(op + EHCI_USBCMD) & EHCI_USBCMD_HCRESET); t++)
			mdelay(1);
	}

	for (i = 0; i < ARRAY_SIZE(uhci); i++)
		if (pci_read_config16(uhci[i], PCI_COMMAND) & PCI_COMMAND_IO)
			outw(UHCI_USBCMD_GRESET, (pci_read_config16(uhci[i],
				PCI_BASE_ADDRESS_4) & ~0x1f) + UHCI_USBCMD);
	mdelay(50);
	for (i = 0; i < ARRAY_SIZE(uhci); i++)
		if (pci_read_config16(uhci[i], PCI_COMMAND) & PCI_COMMAND_IO)
			outw(0, (pci_read_config16(uhci[i], PCI_BASE_ADDRESS_4) & ~0x1f) +
			     UHCI_USBCMD);
}

BOOT_STATE_INIT_ENTRY(BS_DEV_INIT, BS_ON_EXIT, g31mx_usb_bus_reset, NULL);

BOOT_STATE_INIT_ENTRY(BS_DEV_INIT, BS_ON_EXIT, g31mx_usb_final, NULL);

/*
 * Optional CPU_FAN (FAN_CTL2) curve on the CPU diode (TMPIN1), as the
 * vendor "Smart Fan" setup item programs it (E000:7cdf): start PWM 0x40 at
 * 40 C, slope 2 << 3, delta 3 C. The vendor BIOS never sets the full-speed
 * limit (0x6a stays 127 C); use 70 C. SYS_FAN stays fully on, as stock.
 */
static void g31mx_cpu_fan_curve(u16 base)
{
	pnp_write_hwm5_index(base, ITE_EC_FAN_CTL_TEMP_LIMIT_OFF(2), 0);
	pnp_write_hwm5_index(base, ITE_EC_FAN_CTL_TEMP_LIMIT_START(2), 40);
	pnp_write_hwm5_index(base, ITE_EC_FAN_CTL_TEMP_LIMIT_FULL(2), 70);
	pnp_write_hwm5_index(base, ITE_EC_FAN_CTL_PWM_START(2), 0x40);
	pnp_write_hwm5_index(base, ITE_EC_FAN_CTL_PWM_AUTO(2), 2 << 3);
	pnp_write_hwm5_index(base, ITE_EC_FAN_CTL_DELTA_TEMP(2), 3);
	pnp_write_hwm5_index(base, ITE_EC_FAN_CTL_PWM_CONTROL(2),
			     ITE_EC_FAN_CTL_PWM_MODE_AUTOMATIC | ITE_EC_FAN_CTL_TEMPIN(1));
	pnp_write_hwm5_index(base, ITE_EC_FAN_MAIN_CTL,
			     pnp_read_hwm5_index(base, ITE_EC_FAN_MAIN_CTL) |
			     ITE_EC_FAN_MAIN_CTL_SMART(2));
}

static void g31mx_ec_final(void *unused)
{
	struct device *ec = dev_find_slot_pnp(0x2e, IT8718F_EC);
	const struct resource *res;

	if (!is_dev_enabled(ec))
		return;

	/*
	 * The vendor SIO table makes PSON# follow PSIN (SLP_S3#) after AC
	 * power returns; keep it consistent with the ICH7 AFTERG3 option.
	 */
	ite_ec_set_power_state(ec);

	res = probe_resource(ec, PNP_IDX_IO0);
	if (res && get_uint_option("cpu_fan_mode", 0))
		g31mx_cpu_fan_curve(res->base);
}

BOOT_STATE_INIT_ENTRY(BS_DEV_INIT, BS_ON_EXIT, g31mx_ec_final, NULL);

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
