/* SPDX-License-Identifier: GPL-2.0-only */

#include <device/device.h>
#include <device/pci.h>
#include <device/pci_ids.h>
#include <console/console.h>
#include <cbmem.h>
#include <bootmode.h>
#include <arch/io.h>
#include <delay.h>

#if CONFIG(G31MX_USE_IGFX)
#include <mainboard/foxconn/g31mx/stock_preinit.h>
#include <program_loading.h>
#endif

#include "registers/host_bridge.h"

#define G31_POST_GFX_HANDOFF_INVALID	0xe6
#define G31_BSM				0x5c
#define G31_MSAC			0x62

extern struct device *vga_pri;

#if CONFIG(G31MX_USE_IGFX)
/* Successful fresh Coreboot ROM return plus both read-only postflights. */
static struct device *g31_validated_owner;
static u8 g31_validated_msac;
#endif

static bool g31_stolen_valid(const struct device *host, const struct device *igd)
{
	/* Intel 317495-001 sections 5.1.14, 5.1.28, 5.1.32 through 5.1.35. */
	static const u16 gms_mib[] = { 0, 1, 4, 8, 16, 32, 48, 64, 128, 256 };
	static const u8 tseg_mib[] = { 1, 2, 8, 0 };
	const u16 ggc = pci_read_config16(host, D0F0_GGC);
	const unsigned int gms = (ggc >> 4) & 0xf;
	const unsigned int ggms = (ggc >> 8) & 3;
	const u8 smram = pci_read_config8(host, D0F0_SMRAM);
	const u8 esmramc = pci_read_config8(host, D0F0_ESMRAMC);
	const u32 tseg_size = (u32)tseg_mib[(esmramc >> 1) & 3] << 20;
	const u32 tolud = (u32)(pci_read_config16(host, D0F0_TOLUD) & 0xfff0) << 16;
	const u32 gbsm = pci_read_config32(host, D0F0_GBSM);
	const u32 bgsm = pci_read_config32(host, D0F0_BGSM);
	const u32 tseg = pci_read_config32(host, D0F0_TSEG);

	/* The OEM legacy path programs a GTT: GGMS zero is not this handoff. */
	/* Post-MP admission requires enabled, locked SMRAM with D_OPEN clear. */
	if ((ggc & (1U << 1)) || !gms || gms >= ARRAY_SIZE(gms_mib) || ggms != 1 ||
	    (smram & 0x58) != 0x18 || !(esmramc & 1) || !tseg_size ||
	    tolud < ((u32)gms_mib[gms] << 20) + (1U << 20) + tseg_size)
		return false;

	return gbsm == tolud - ((u32)gms_mib[gms] << 20) &&
		bgsm == gbsm - (1U << 20) && tseg == bgsm - tseg_size &&
		!(tseg & (tseg_size - 1)) && cbmem_top() && cbmem_top() <= tseg &&
		pci_read_config16(igd, D0F0_GGC) == ggc &&
		pci_read_config32(igd, G31_BSM) == gbsm;
}

static bool g31_overlaps(u64 a, u64 asize, u64 b, u64 bsize)
{
	if (!asize || !bsize)
		return false;
	if (asize > UINT64_MAX - a || bsize > UINT64_MAX - b)
		return true;
	return a < b + bsize && b < a + asize;
}

static bool g31_chipset_clear(const struct device *host, const struct device *lpc,
			     const struct resource *res)
{
	/* Intel 317495-001 chapter 3 and sections 5.1.12, .13, .18, .19. */
	const u32 ecam = pci_read_config32(host, D0F0_PCIEXBAR_LO);
	const unsigned int code = (ecam >> 1) & 3;
	if (code == 3)
		return false;
	const u32 ecam_size = (256U << 20) >> code;
	const u64 ecam_base = ((u64)pci_read_config32(host, D0F0_PCIEXBAR_HI) << 32) |
		(ecam & ~(ecam_size - 1));
	if ((ecam & 1) && g31_overlaps(res->base, res->size, ecam_base, ecam_size))
		return false;

	static const struct { u16 reg; u32 size; } host_windows[] = {
		{ D0F0_EPBAR_LO, 0x1000 },
		{ D0F0_MCHBAR_LO, 0x4000 },
		{ D0F0_DMIBAR_LO, 0x1000 },
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(host_windows); i++) {
		const u16 reg = host_windows[i].reg;
		const u32 size = host_windows[i].size;
		const u32 low = pci_read_config32(host, reg);
		const u64 base = ((u64)pci_read_config32(host, reg + 4) << 32) |
			(low & ~(size - 1));
		if ((low & 1) && g31_overlaps(res->base, res->size, base, size))
			return false;
	}
	/* ICH7 RCBA, 307013-003 section 10.1.34: 16 KiB, enable bit 0. */
	const u32 rcba = pci_read_config32(lpc, 0xf0);
	return !(rcba & 1) ||
		!g31_overlaps(res->base, res->size, rcba & ~0x3fffU, 0x4000);
}

static bool g31_bar_valid(const struct device *dev, unsigned int index,
			  u64 size, bool io, const struct device *host,
			  const struct device *lpc)
{
	const struct resource *res = probe_resource(dev, index);
	const unsigned long type = io ? IORESOURCE_IO : IORESOURCE_MEM;
	const unsigned long required = type | IORESOURCE_ASSIGNED | IORESOURCE_STORED;
	const unsigned long forbidden = IORESOURCE_READONLY | IORESOURCE_PCI64 |
		IORESOURCE_BRIDGE | IORESOURCE_SUBTRACTIVE;
	if (!res || (res->flags & required) != required || (res->flags & forbidden) ||
	    (res->flags & IORESOURCE_TYPE_MASK) != type || res->size != size ||
	    !size || res->base & (size - 1) || res->base > UINT32_MAX ||
	    size - 1 > UINT32_MAX - res->base || res->base + size - 1 > res->limit)
		return false;

	const u32 bar = pci_read_config32(dev, index);
	if (io) {
		if (res->base < 0x400 || res->base + size > 0x10000 ||
		    (bar & ~0xfff8U) != 1 || (bar & 0xfff8) != res->base ||
		    g31_overlaps(res->base, size, 0xcf8, 8))
			return false;
	} else {
		const u32 attr = res->flags & IORESOURCE_PREFETCH ? 8 : 0;
		const u32 tolud = (u32)(pci_read_config16(host, D0F0_TOLUD) & 0xfff0) << 16;
		/* Below IOAPIC, local APIC, chipset and high BIOS fixed ranges. */
		if ((bar & 0xf) != attr || (bar & ~0xfU) != res->base ||
		    res->base < tolud || res->base + size > 0xfec00000 ||
		    !g31_chipset_clear(host, lpc, res))
			return false;
	}

	/* Include fixed/assigned ownership, not encompassing bridge windows. */
	for (const struct device *other = all_devices; other; other = other->next) {
		for (const struct resource *r = other->resource_list; r; r = r->next) {
			if (r == res || !(r->flags & (IORESOURCE_ASSIGNED | IORESOURCE_FIXED)) ||
			    (r->flags & (IORESOURCE_BRIDGE | IORESOURCE_SUBTRACTIVE)) ||
			    (r->flags & IORESOURCE_TYPE_MASK) != type)
				continue;
			if (g31_overlaps(res->base, size, r->base, r->size))
				return false;
		}
	}
	return true;
}

static bool g31_resources_valid(const struct device *dev, const struct device *host,
				const struct device *secondary)
{
	const struct device *lpc = pcidev_on_root(31, 0);
	const struct resource *aperture = probe_resource(dev, PCI_BASE_ADDRESS_2);
	if (!lpc || !aperture || !(aperture->flags & IORESOURCE_PREFETCH) ||
	    (aperture->size != (128U << 20) && aperture->size != (256U << 20) &&
	     aperture->size != (512U << 20)) ||
	    !g31_bar_valid(dev, PCI_BASE_ADDRESS_0, 0x80000, false, host, lpc) ||
	    !g31_bar_valid(dev, PCI_BASE_ADDRESS_1, 8, true, host, lpc) ||
	    !g31_bar_valid(dev, PCI_BASE_ADDRESS_2, aperture->size, false, host, lpc) ||
	    !g31_bar_valid(dev, PCI_BASE_ADDRESS_3, 0x100000, false, host, lpc))
		return false;

	return !secondary || !secondary->enabled ||
		g31_bar_valid(secondary, PCI_BASE_ADDRESS_0, 0x80000, false, host, lpc);
}

static bool g31_reset_idle(const struct device *dev)
{
	/* GDRST is one byte. Never pulse reset or reassert an outstanding reset. */
	for (unsigned int i = 0; i < 1000; i++) {
		const u8 reset = pci_read_config8(dev, 0xc0);
		if (reset & 0x0c) /* GRDOM: only full reset (00) is defined for G31. */
			return false;
		if (!(reset & 1))
			return true;
		udelay(1);
	}
	return false;
}

static bool g31_pm_valid(const struct device *dev)
{
	/* The OEM ROM reads D31:F0 PMBASE and uses PMBASE + 8 as its timer. */
	const struct device *lpc = pcidev_on_root(31, 0);
	if (!lpc || pci_read_config32(lpc, PCI_VENDOR_ID) != 0x27b88086)
		return false;
	const u32 raw = pci_read_config32(lpc, 0x40);
	const u16 base = raw & 0xff80;
	const struct resource *io = probe_resource(dev, PCI_BASE_ADDRESS_1);
	if ((raw & ~0xff80U) != 1 || base < 0x400 ||
	    !(pci_read_config8(lpc, 0x44) & (1U << 7)) || !io ||
	    g31_overlaps(io->base, io->size, base, 0x80) ||
	    g31_overlaps(base, 0x80, 0xcf8, 8))
		return false;

	/* Read-only liveness check, not a calibration or a hardware timing guarantee. */
	const u32 first = inl(base + 8) & 0xffffff;
	for (unsigned int i = 0; i < 1000; i++) {
		if ((inl(base + 8) & 0xffffff) != first)
			return true;
		udelay(1);
	}
	return false;
}

static bool g31_gma_handoff_valid(struct device *dev)
{
	if (dev->path.type != DEVICE_PATH_PCI || !dev->upstream ||
	    dev->upstream->secondary || dev->upstream->segment_group ||
	    dev->path.pci.devfn != PCI_DEVFN(2, 0) || dev != vga_pri ||
	    dev->vendor != PCI_VID_INTEL || dev->device != 0x29c2 ||
	    dev->class != (PCI_CLASS_DISPLAY_VGA << 8) ||
	    pci_read_config32(dev, PCI_VENDOR_ID) != 0x29c28086 ||
	    (pci_read_config32(dev, PCI_CLASS_REVISION) >> 8) != dev->class)
		return false;

	const struct device *host = pcidev_on_root(0, 0);
	if (!host || pci_read_config32(host, PCI_VENDOR_ID) != 0x29c08086 ||
	    (pci_read_config32(host, D0F0_CAPID0 + 4) & (1U << 14)))
		return false;

	const u32 deven = pci_read_config32(host, D0F0_DEVEN);
	const u8 header = pci_read_config8(dev, PCI_HEADER_TYPE);
	if ((deven & (D0EN | D1EN | IGD0EN)) != (D0EN | IGD0EN) ||
	    pci_read_config32(dev, D0F0_DEVEN) != deven ||
	    (header & 0x7f) != PCI_HEADER_TYPE_NORMAL ||
	    !!(header & 0x80) != !!(deven & IGD1EN) || !g31_stolen_valid(host, dev))
		return false;

	const struct device *secondary = pcidev_on_root(2, 1);
	if (!g31_resources_valid(dev, host, secondary))
		return false;
	if (!(deven & IGD1EN)) {
		if (secondary && secondary->enabled)
			return false;
	} else {
		/* CAPID0[78] is a host word, not an IGD SCI register. */
		if ((pci_read_config16(host, D0F0_CAPID0 + 8) & (1U << 14)) ||
		    !secondary || !secondary->enabled || secondary->vendor != PCI_VID_INTEL ||
		    secondary->device != 0x29c3 ||
		    pci_read_config32(secondary, PCI_VENDOR_ID) != 0x29c38086 ||
		    !(pci_read_config16(secondary, PCI_COMMAND) & PCI_COMMAND_MEMORY))
			return false;
	}

	const u16 command = pci_read_config16(dev, PCI_COMMAND);
	return (command & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_INT_DISABLE)) ==
		(PCI_COMMAND_IO | PCI_COMMAND_MEMORY) &&
		pci_read_config8(dev, PCI_INTERRUPT_PIN) == 1 &&
		pci_read_config8(dev, 0xd0) == PCI_CAP_ID_PM &&
		!(pci_read_config16(dev, 0xd4) & 3) &&
		!pci_read_config32(dev, 0xfc) && g31_reset_idle(dev) && g31_pm_valid(dev);
}

static void g31_gma_init(struct device *dev)
{
#if CONFIG(G31MX_USE_IGFX)
	g31_validated_owner = NULL;
#endif
	if (!dev->enabled)
		return;
	if (!g31_gma_handoff_valid(dev))
		die_with_post_code(G31_POST_GFX_HANDOFF_INVALID,
				   "G31: invalid legacy graphics handoff\n");
	/* Without Coreboot execution, the payload (SeaBIOS) runs the VGA ROM. */
	if (CONFIG(NO_GFX_INIT) || !CONFIG(VGA_ROM_RUN))
		return;

	/* Reject a stale global receipt rather than mistake it for this ROM run. */
	if (gfx_get_init_done())
		die_with_post_code(G31_POST_GFX_HANDOFF_INVALID,
				   "G31: unexpected prior graphics initialization\n");
	/*
	 * Leave BME unchanged. Intel 317495-001 section 8.1.3 defines it for
	 * PCI-compliant mastering; the bounded OEM legacy entry establishes
	 * IO/MEM access, not a mandatory BME write. OS DMA ownership is separate.
	 */
	/* GMADR address-mask bits depend on MSAC[1:0] (317495-001 section 8.1.10). */
	const u8 msac = pci_read_config8(dev, G31_MSAC) & 3;
#if CONFIG(G31MX_USE_IGFX)
	g31mx_igfx_vc_check();
#endif
	pci_dev_init(dev);
#if CONFIG(G31MX_USE_IGFX)
	if (!gfx_get_init_done())
		die_with_post_code(G31_POST_GFX_HANDOFF_INVALID,
				   "G31: mandatory VGA ROM did not return\n");
	if ((pci_read_config8(dev, G31_MSAC) & 3) != msac ||
	    !g31_gma_handoff_valid(dev))
#else
	if ((pci_read_config8(dev, G31_MSAC) & 3) != msac ||
	    (gfx_get_init_done() && !g31_gma_handoff_valid(dev)))
#endif
		die_with_post_code(G31_POST_GFX_HANDOFF_INVALID,
				   "G31: ROM changed the validated graphics handoff\n");
#if CONFIG(G31MX_USE_IGFX)
	g31mx_igfx_vc_check();
	g31_validated_msac = msac;
	g31_validated_owner = dev;
#endif
	/* This cannot validate SeaBIOS's future execution or physical display. */
}

#if CONFIG(G31MX_USE_IGFX)
/* Existing ramstage hook: the last platform gate before architecture transfer. */
void platform_prog_run(struct prog *prog)
{
	if (prog_type(prog) != PROG_PAYLOAD || !is_dev_enabled(pcidev_on_root(2, 0)) ||
	    CONFIG(NO_GFX_INIT) || !CONFIG(VGA_ROM_RUN))
		return;
	struct device *dev = g31_validated_owner;

	if (!dev || dev != pcidev_on_root(2, 0) || !dev->enabled ||
	    !gfx_get_init_done() || (pci_read_config8(dev, G31_MSAC) & 3) !=
		g31_validated_msac || !g31_gma_handoff_valid(dev))
		die_with_post_code(G31_POST_GFX_HANDOFF_INVALID,
				   "G31: invalid graphics state at payload transfer\n");
	g31mx_igfx_vc_check();
}
#endif

static void g31_gma_disable(struct device *dev)
{
	struct device *host = pcidev_on_root(0, 0);

	/* Intel 317495-001 section 5.1.14: only D0:F0 owns writable GGC. */
	if (!host || pci_read_config32(host, PCI_VENDOR_ID) != 0x29c08086)
		die_with_post_code(G31_POST_GFX_HANDOFF_INVALID,
				   "G31: invalid host for VGA disable\n");
	pci_or_config16(host, D0F0_GGC, 1U << 1);
}

/* This legacy driver does not share G4x native initialization or OpRegion. */
static const char *g31_gma_acpi_name(const struct device *dev)
{
	return "GFX0";
}

static struct device_operations g31_gma_ops = {
	.read_resources		= pci_dev_read_resources,
	.set_resources		= pci_dev_set_resources,
	.enable_resources	= pci_dev_enable_resources,
#if CONFIG(HAVE_ACPI_TABLES)
	.write_acpi_tables	= pci_rom_write_acpi_tables,
	.acpi_fill_ssdt		= pci_rom_ssdt,
#endif
	.acpi_name		= g31_gma_acpi_name,
	.init			= g31_gma_init,
	.vga_disable		= g31_gma_disable,
	.ops_pci		= &pci_dev_ops_pci,
};

static const struct pci_driver g31_gma __pci_driver = {
	.ops = &g31_gma_ops,
	.vendor = PCI_VID_INTEL,
	.device = 0x29c2,
};

/* Function 1 has a separate MMADR view, not another VGA ROM target. */
static struct device_operations g31_gma_secondary_ops = {
	.read_resources		= pci_dev_read_resources,
	.set_resources		= pci_dev_set_resources,
	.enable_resources	= pci_dev_enable_resources,
	.ops_pci		= &pci_dev_ops_pci,
};

static const struct pci_driver g31_gma_secondary __pci_driver = {
	.ops = &g31_gma_secondary_ops,
	.vendor = PCI_VID_INTEL,
	.device = 0x29c3,
};
