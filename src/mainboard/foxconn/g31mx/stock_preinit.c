/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Cold-boot chipset program the vendor Award boot block (773F1P14) runs
 * before it enters its memory reference code, in the same order. It was
 * recovered by executing the boot block from the reset vector to the MRC
 * entry (reference/bbtrace/), then transcribed from the executed code:
 * reference/bbtrace/award-coldboot-premrc.lst. CPU microcode, MTRR, AP
 * start-up and CMOS bookkeeping are left to Coreboot. The vendor PM base
 * 0x400 and SMBus base 0x500 are translated to the Coreboot bases.
 */

#include <arch/io.h>
#include <delay.h>
#include <device/mmio.h>
#include <device/pci_def.h>
#include <device/pci_ops.h>
#include <northbridge/intel/x4x/x4x.h>
#include <southbridge/intel/common/rcba.h>
#include <southbridge/intel/i82801gx/i82801gx.h>
#include <types.h>
#include "stock_preinit.h"

#define LPC		PCI_DEV(0, 0x1f, 0)
#define PEG_ECAM	(CONFIG_ECAM_MMCONF_BASE_ADDRESS + (1 << 15))
#define SIO_IDX		0x2e
#define SIO_DAT		0x2f
#define SIO_GPIO_BASE	0x800
#define SMBUS		CONFIG_FIXED_SMBUS_IO_BASE

static u16 pmbase(void)
{
	return pci_read_config16(LPC, PMBASE) & 0xff80;
}

static void ecam_rmw32(uintptr_t addr, u32 and, u32 or)
{
	write32p(addr, (read32p(addr) & and) | or);
}

static void poll_clear(uintptr_t addr, u32 mask)
{
	int i;

	for (i = 0; i < 100 && (read32p(addr) & mask); i++)
		;
}

/* 0xff600: byte read/modify/write table on ICH7 functions. */
static void lpc_table(void)
{
	static const struct {
		u8 dev, fn, reg, and, or;
	} t[] = {
		{ 30, 0, 0x1c, 0x00, 0x10 }, { 30, 0, 0x04, 0x00, 0x01 },
		{ 31, 3, 0x40, 0x00, 0x01 }, { 31, 3, 0x04, 0x00, 0x03 },
		{ 31, 0, 0x44, 0x00, 0x80 },
		{ 31, 0, 0x4c, 0x00, 0x10 }, { 31, 0, 0x64, 0x00, 0xc0 },
		{ 31, 0, 0xd9, 0x00, 0xc0 }, { 31, 0, 0xdc, 0x00, 0x00 },
		{ 31, 0, 0xb8, 0x00, 0x55 }, { 31, 0, 0xb9, 0x00, 0x55 },
		{ 31, 0, 0xba, 0x00, 0x55 }, { 31, 0, 0xbb, 0x00, 0x55 },
		{ 31, 0, 0x85, 0x00, 0x08 }, { 31, 0, 0x84, 0x00, 0x01 },
		{ 31, 0, 0x86, 0x00, 0x3f }, { 31, 0, 0x89, 0x00, 0x02 },
		{ 31, 0, 0x88, 0x00, 0x91 }, { 31, 0, 0x8a, 0x00, 0x3f },
		{ 31, 0, 0x82, 0x00, 0x08 }, { 31, 0, 0x83, 0x00, 0x34 },
		{ 31, 0, 0x81, 0xef, 0x00 },
	};
	unsigned int i;

	/* PMBASE (0x41), GPIOBASE (0x48) and the SMBus base stay Coreboot's. */
	for (i = 0; i < ARRAY_SIZE(t); i++) {
		const pci_devfn_t dev = PCI_DEV(0, t[i].dev, t[i].fn);

		pci_write_config8(dev, t[i].reg,
				  (pci_read_config8(dev, t[i].reg) & t[i].and) | t[i].or);
	}
	/* The vendor table leaves COM1 undecoded; the console needs it. */
	pci_or_config16(LPC, LPC_EN, COMA_LPC_EN);
}

/* 0xf6369: GEN_PMCON_3. */
static void pmcon3(void)
{
	const u8 v = pci_read_config8(LPC, GEN_PMCON_3);

	if (v & (1 << 1))
		outb(v & 0xe3, pmbase() + 5);
	pci_write_config8(LPC, GEN_PMCON_3, v & ~(1 << 1));
}

/* 0xf731f: ICH7 GPIO. */
static void ich_gpio(void)
{
	const u16 g = DEFAULT_GPIOBASE;

	outl((inl(g + 0x00) | 0x101200) & ~0x3c, g + 0x00);
	outl(inl(g + 0x04) | 0x1200, g + 0x04);
	outl((inl(g + 0x0c) | 0x100000) & ~0x3c, g + 0x0c);
	outl(inl(g + 0x2c) | 0x3900, g + 0x2c);
	outl(inl(g + 0x30) | 0xc6, g + 0x30);
	outl((inl(g + 0x34) | 0x46) & ~0x80, g + 0x34);
	outl(inl(g + 0x38) | 0x80, g + 0x38);
}

/* 0xf64f9: ICH7 chipset initialization registers in RCBA. */
static void rcba_cir(void)
{
	static const struct {
		u16 off;
		u32 and, or;
	} t[] = {
		{ 0x3410, 0xfffffffb, 0x00000004 }, { 0x3400, 0xfffffffb, 0x00000004 },
		{ 0x0088, 0x00000000, 0x0011d000 }, { 0x01fc, 0xffff0000, 0x0000060f },
		{ 0x01f4, 0x00000000, 0x86000040 }, { 0x0214, 0x00000000, 0x10030549 },
		{ 0x0218, 0x00000000, 0x00020504 }, { 0x0220, 0xffffff00, 0x000000c5 },
		{ 0x3410, 0xffffffbf, 0x00000040 }, { 0x3430, 0xfffffffc, 0x00000001 },
		{ 0x3418, 0xfffffffe, 0x00000001 }, { 0x0200, 0xffff0000, 0x00002008 },
		{ 0x2027, 0xffffff00, 0x0000000d }, { 0x2034, 0xfffffff0, 0x00000002 },
		{ 0x3e08, 0xffffff7f, 0x00000080 }, { 0x3e48, 0xffffff7f, 0x00000080 },
		{ 0x3e0e, 0xffffff7f, 0x00000080 }, { 0x3e4e, 0xffffff7f, 0x00000080 },
	};
	unsigned int i;

	/* The vendor code accesses these, unaligned ones included, as dwords. */
	for (i = 0; i < ARRAY_SIZE(t); i++)
		RCBA32(t[i].off) = (RCBA32(t[i].off) & t[i].and) | t[i].or;
}

/* 0xf6de0: egress port, DMI and ICH7 virtual channel set-up. */
static void vc_setup(void)
{
	static const u32 ep_arb[8] = {
		0x01000001, 0x00040000, 0x00001000, 0x00000040,
		0x01000001, 0x00040000, 0x00001000, 0x00000040,
	};
	static const u8 rcba_arb[64] = {
		0x0f, 0, 0, 0, 0, 0, 0x0f, 0, 0, 0, 0, 0, 0xf0, 0, 0, 0,
		0, 0, 0, 0x0f, 0, 0, 0, 0, 0, 0xf0, 0, 0, 0, 0, 0, 0,
		0x0f, 0, 0, 0, 0, 0, 0x0f, 0, 0, 0, 0, 0, 0xf0, 0, 0, 0,
		0, 0, 0, 0x0f, 0, 0, 0, 0, 0, 0xf0, 0, 0, 0, 0, 0, 0,
	};
	unsigned int i;

	RCBA8(0x341c) |= 1;

	epbar_clrbits32(0x14, 0xfe);
	epbar_clrsetbits32(0x04, 0x07, 0x01);
	epbar_clrsetbits32(0x0c, 0x0e, 0x02);
	dmibar_setbits32(0x210, 0x09);
	epbar_setbits32(0x20, 1 << 24);
	epbar_clrsetbits32(0x20, 0xfe, 0x80);
	for (i = 0; i < ARRAY_SIZE(ep_arb); i++)
		epbar_write32(0x100 + 4 * i, ep_arb[i]);
	epbar_setbits32(0x20, 1 << 16);
	epbar_setbits32(0x20, 1 << 16);
	poll_clear(CONFIG_FIXED_EPBAR_MMIO_BASE + 0x24, 1 << 16);
	epbar_setbits32(0x20, 1 << 31);
	poll_clear(CONFIG_FIXED_EPBAR_MMIO_BASE + 0x24, 1 << 17);

	dmibar_clrbits32(0x14, 0xfe);
	dmibar_clrsetbits32(0x04, 0x07, 0x01);
	dmibar_setbits32(0x20, 1 << 24);
	dmibar_clrsetbits32(0x20, 0xfe, 0x80);
	dmibar_setbits32(0x20, 1 << 31);
	poll_clear(CONFIG_FIXED_DMIBAR_MMIO_BASE + 0x24, 1 << 17);

	mchbar_clrsetbits32(0x48, 0x300, 0x200);
	dmibar_setbits32(0xf4, 1 << 16);
	dmibar_clrsetbits32(0xfc, 0x70000000, 0x80e00000);
	dmibar_setbits32(0xfc, 1 << 18);
	mchbar_clrsetbits32(0x30, 0x30000, 0x20000);
	dmibar_write32(0x2c, 0x86000040);
	poll_clear(CONFIG_FIXED_DMIBAR_MMIO_BASE + 0x30, 1 << 17);

	dmibar_clrbits32(0x200, 1 << 21);
	dmibar_clrbits32(0x204, 0xc00);
	dmibar_clrsetbits32(0xec8, 0xe00, 0xc00);
	dmibar_clrsetbits32(0xedc, 0xe00, 0xc00);
	dmibar_clrsetbits32(0xef0, 0xe00, 0xc00);
	dmibar_clrsetbits32(0xf04, 0xe00, 0xc00);
	dmibar_setbits32(0xeb4, 0x1000000e);
	dmibar_clrbits32(0xeb4, 1);
	dmibar_setbits32(0xeb4, 1);

	RCBA32(0x20) |= 1 << 24;
	RCBA32(0x20) = (RCBA32(0x20) & ~0xfe) | 0x80;
	RCBA32(0x14) &= ~0xfe;
	RCBA32(0x1c) = (RCBA32(0x1c) & 0xff80ffff) | 0x120000;
	RCBA32(0x20) |= 1 << 31;
	poll_clear(CONFIG_FIXED_RCBA_MMIO_BASE + 0x18, 1 << 17);
	poll_clear(CONFIG_FIXED_RCBA_MMIO_BASE + 0x24, 1 << 17);
	RCBA32(0x20) = (RCBA32(0x20) & 0xfff1ffff) | 0x80000;
	for (i = 0; i < ARRAY_SIZE(rcba_arb); i++)
		RCBA8(0x30 + i) = rcba_arb[i];
	RCBA32(0x20) |= 1 << 16;
	poll_clear(CONFIG_FIXED_RCBA_MMIO_BASE + 0x24, 1 << 16);

	pci_update_config8(PCI_DEV(0, 1, 0), 0xec, 0xf8, 0x01);
}

/* 0xf65de: DMI error status, root complex topology, PEG and ICH7 ports. */
static void root_complex(void)
{
	static const u16 relock[] = { 0x84, 0x308, 0x314, 0x324, 0x328, 0x334, 0x338 };
	unsigned int i;
	int fn;

	dmibar_write32(0x1c4, 0xffffffff);
	dmibar_write32(0x1d0, 0xffffffff);
	epbar_clrsetbits32(0x44, 0x00ff0000, 0x00010000);
	epbar_clrsetbits32(0x50, 0x00ff0000, 0x00010001);
	epbar_write32(0x58, CONFIG_FIXED_DMIBAR_MMIO_BASE);
	epbar_clrsetbits32(0x60, 0x00ff0000, 0x00010001);
	dmibar_clrsetbits32(0x44, 0x00ff0000, 0x00010000);
	dmibar_write32(0x50, 0x00020001);
	dmibar_write32(0x58, CONFIG_FIXED_RCBA_MMIO_BASE);
	dmibar_clrsetbits32(0x60, 0x00ff0000, 0x00010001);
	dmibar_write32(0x68, CONFIG_FIXED_EPBAR_MMIO_BASE);
	for (i = 0; i < ARRAY_SIZE(relock); i++)
		dmibar_write32(relock[i], dmibar_read32(relock[i]));

	if (pci_read_config8(HOST_BRIDGE, D0F0_DEVEN) & (1 << 1)) {
		ecam_rmw32(PEG_ECAM + 0x144, 0xff00ffff, 0x00010000);
		write32p(PEG_ECAM + 0x158, CONFIG_FIXED_EPBAR_MMIO_BASE);
		ecam_rmw32(PEG_ECAM + 0x150, 0xff00ffff, 0x00010001);
		write32p(PEG_ECAM + 0x308, read32p(PEG_ECAM + 0x308));
		write32p(PEG_ECAM + 0x314, read32p(PEG_ECAM + 0x314));
		/* The vendor code stores the 0x324 value to 0x144. */
		write32p(PEG_ECAM + 0x144, read32p(PEG_ECAM + 0x324));
		write32p(PEG_ECAM + 0x328, read32p(PEG_ECAM + 0x328));
		/* Secondary bus reset pulse, then 200 ms and 500 us. */
		write32p(PEG_ECAM + 0x3e, read32p(PEG_ECAM + 0x3e) | 0x40);
		udelay(50);
		write32p(PEG_ECAM + 0x3e, read32p(PEG_ECAM + 0x3e) & ~0x40);
		mdelay(200);
		udelay(500);
	}

	for (fn = 0; fn <= 5; fn++) {
		const uintptr_t rp = CONFIG_ECAM_MMCONF_BASE_ADDRESS + (28 << 15) + (fn << 12);

		write16p(rp + 0x314, (read16p(rp + 0x314) & 0xfe3f) | 0x40);
		write8p(rp + 0x43, read8p(rp + 0x43) | 1);
		if ((read8p(rp + 0x5a) & 0x40) && (read16p(rp + 0x5a) & 0x08)) {
			const uintptr_t ep = CONFIG_ECAM_MMCONF_BASE_ADDRESS + (5 << 20);

			write32p(rp + 0x18, 0x00050500);
			if ((read32p(ep) == 0x108b8086 || read32p(ep) == 0x108c8086) &&
			    read8p(ep + 8) < 3) {
				write8p(rp + 0x3e, read8p(rp + 0x3e) | 0x40);
				write8p(rp + 0x3e, read8p(rp + 0x3e) & ~0x40);
			}
		}
		write32p(rp + 0x18, 0);
	}
}

/* 0xfe33a: keyboard controller self test and command byte. */
static void kbc(void)
{
	int i;

	outb(0xaa, 0x64);
	for (i = 0; i < 0x800 && !(inb(0x64) & 1); i++)
		;
	inb(0x60);
	outb(0xcb, 0x64);
	for (i = 0; i < 0x800 && (inb(0x64) & 2); i++)
		;
	outb(0x01, 0x60);
	for (i = 0; i < 0x800 && (inb(0x64) & 2); i++)
		;
	outb(0x60, 0x64);
	for (i = 0; i < 0x800 && (inb(0x64) & 2); i++)
		;
	outb(0x45, 0x60);
	for (i = 0; i < 0x800 && (inb(0x64) & 2); i++)
		;
	outb(0xae, 0x64);
	for (i = 0; i < 0x800 && !(inb(0x64) & 1); i++)
		;
}

static void sio_enter(void)
{
	outb(0x87, SIO_IDX);
	outb(0x01, SIO_IDX);
	outb(0x55, SIO_IDX);
	outb(0x55, SIO_IDX);
}

static void sio_exit(void)
{
	outb(0x02, SIO_IDX);
	outb(0x02, SIO_DAT);
}

/* 0xfe3a0: IT8718F board table, then its GPIO outputs. */
static void superio(void)
{
	static const u8 t[][2] = {
		{ 0x22, 0x84 }, { 0x23, 0x00 }, { 0x07, 0x03 }, { 0xf0, 0x08 },
		{ 0x07, 0x00 }, { 0x30, 0x01 }, { 0xf1, 0x80 }, { 0x07, 0x05 },
		{ 0x30, 0x01 }, { 0x60, 0x00 }, { 0x61, 0x60 }, { 0x62, 0x00 },
		{ 0x63, 0x64 }, { 0x70, 0x01 }, { 0xf0, 0x28 }, { 0x07, 0x06 },
		{ 0x30, 0x01 }, { 0x07, 0x04 }, { 0x30, 0x01 }, { 0x60, 0x02 },
		{ 0x61, 0x90 }, { 0x62, 0x00 }, { 0x63, 0x00 }, { 0x70, 0x00 },
		{ 0x07, 0x04 }, { 0x30, 0x01 }, { 0x70, 0x00 }, { 0xf0, 0x00 },
		{ 0xf1, 0x1f }, { 0xf2, 0x00 }, { 0x07, 0x07 }, { 0x25, 0x40 },
		{ 0x26, 0x0c }, { 0x27, 0x00 }, { 0x28, 0x00 }, { 0x29, 0x08 },
		{ 0x2a, 0x00 }, { 0x2b, 0x00 }, { 0x62, 0x08 }, { 0x63, 0x00 },
		{ 0xb0, 0x40 }, { 0xb1, 0x00 }, { 0xb2, 0x00 }, { 0xb3, 0x00 },
		{ 0xb4, 0x00 }, { 0xb8, 0x40 }, { 0xb9, 0x00 }, { 0xba, 0x00 },
		{ 0xbb, 0x00 }, { 0xbc, 0x00 }, { 0xc0, 0x00 }, { 0xc1, 0x00 },
		{ 0xc2, 0x00 }, { 0xc3, 0x00 }, { 0xc4, 0x00 }, { 0xc8, 0x40 },
		{ 0xc9, 0x0c }, { 0xca, 0x00 }, { 0xcb, 0x00 }, { 0xcc, 0x08 },
		{ 0xf5, 0x13 }, { 0xf6, 0x0e }, { 0x07, 0x04 },
	};
	static const u8 rmw[][3] = {
		{ 0xf0, 0x18, 0x00 }, { 0xf2, 0x2e, 0x1a },
		{ 0xf4, 0xff, 0x60 }, { 0xf5, 0x3f, 0x00 },
	};
	unsigned int i;

	sio_enter();
	for (i = 0; i < ARRAY_SIZE(t); i++) {
		outb(t[i][0], SIO_IDX);
		outb(t[i][1], SIO_DAT);
	}
	for (i = 0; i < ARRAY_SIZE(rmw); i++) {
		outb(rmw[i][0], SIO_IDX);
		outb((inb(SIO_DAT) & rmw[i][1]) | rmw[i][2], SIO_DAT);
	}
	sio_exit();

	outb(inb(SIO_GPIO_BASE + 1) | 0x04, SIO_GPIO_BASE + 1);
	outb(inb(SIO_GPIO_BASE + 4) | 0x08, SIO_GPIO_BASE + 4);

	sio_enter();
	outb(0x07, SIO_IDX);
	outb(0x07, SIO_DAT);
	outb(0xc1, SIO_IDX);
	outb(0x04, SIO_DAT);
	outb(0xc4, SIO_IDX);
	outb(0x08, SIO_DAT);
	sio_exit();
}

/* 0xfe244: one-byte SMBus block write to the clock generator at 0xd2. */
static void clockgen_write(u8 cmd, u8 val)
{
	const u16 tco2 = pmbase() + 0x66;
	int i;

	outb(inb(tco2) & 0x02, tco2);
	for (i = 0; i < 0x800; i++) {
		const u8 st = inb(SMBUS);

		outb(st, SMBUS);
		if (!(st & 0x9f))
			break;
	}
	outb(0xd2, SMBUS + 4);
	outb(1, SMBUS + 5);
	outb(cmd, SMBUS + 3);
	outb(0x54, SMBUS + 2);
	outb(val, SMBUS + 7);
	for (i = 0; i < 0x40; i++) {
		u8 st;

		udelay(100);
		st = inb(SMBUS);
		outb(st, SMBUS);
		if (st & 0x80)
			break;
	}
}

static void clockgen(void)
{
	clockgen_write(0x05, 0xc1);
	clockgen_write(0x08, 0x1f);
	clockgen_write(0x15, 0x7f);
	clockgen_write(0x05, 0xc0);
}

/* 0xfe489, 0xfe4a1: RTC, legacy DMA/PIC/PIT, DMA page registers. */
static void legacy(void)
{
	static const struct {
		u16 port;
		u8 val;
	} t[] = {
		{ 0x3b8, 0x01 }, { 0x61, 0xfc }, { 0x08, 0x04 }, { 0xd0, 0x04 },
		{ 0xf1, 0x00 }, { 0x43, 0x54 }, { 0x41, 0x00 }, { 0x0d, 0x07 },
		{ 0xda, 0x07 }, { 0x43, 0x40 }, { 0x41, 0x12 }, { 0x08, 0x00 },
		{ 0xd0, 0x00 }, { 0x0b, 0x40 }, { 0xd6, 0xc0 }, { 0xd6, 0x41 },
		{ 0x0b, 0x41 }, { 0xd6, 0x42 }, { 0x0b, 0x42 }, { 0xd6, 0x43 },
		{ 0x0b, 0x43 }, { 0xd2, 0x00 }, { 0xd4, 0x00 }, { 0x20, 0x11 },
		{ 0x21, 0x08 }, { 0x21, 0x04 }, { 0x21, 0x01 }, { 0x21, 0xff },
		{ 0xa0, 0x11 }, { 0xa1, 0x70 }, { 0xa1, 0x02 }, { 0xa1, 0x01 },
		{ 0xa1, 0xff }, { 0x43, 0x36 }, { 0x40, 0x00 }, { 0x40, 0x00 },
	};
	unsigned int i;
	u16 port;

	outb(0x8b, 0x70);
	outb(0x02, 0x71);
	outb(0x8a, 0x70);
	outb(0x26, 0x71);
	for (i = 0; i < ARRAY_SIZE(t); i++)
		outb(t[i].val, t[i].port);
	for (port = 0x81; port < 0x90; port++)
		outb(0, port);
}

/* 0xec9f4: the MRC loader's power management, SMBus and RTC set-up. */
static void loader_pm(void)
{
	static const u8 pm[][2] = {
		{ 0x02, 0x00 }, { 0x03, 0x00 }, { 0x00, 0xff }, { 0x01, 0xff },
		{ 0x11, 0x00 }, { 0x28, 0xff }, { 0x29, 0xff }, { 0x2a, 0xff },
		{ 0x2b, 0xff }, { 0x2c, 0x00 }, { 0x2d, 0x00 }, { 0x2e, 0x00 },
		{ 0x2f, 0x00 }, { 0x30, 0x20 }, { 0x31, 0x00 }, { 0x34, 0xff },
		{ 0x35, 0xff }, { 0x41, 0x30 }, { 0x44, 0xff }, { 0x45, 0xff },
	};
	const u16 pm_base = pmbase();
	unsigned int i;
	u8 v;

	outb(0, 0x4d0);
	outb(0, 0x4d1);
	for (i = 0; i < ARRAY_SIZE(pm); i++)
		outb(pm[i][1], pm_base + pm[i][0]);

	outb(0x5e, SMBUS);
	outb(0xff, SMBUS + 4);
	outb(0x40, SMBUS + 2);
	for (i = 0; i < 10000; i++) {
		udelay(10);
		if (!(inb(SMBUS) & 1))
			break;
	}
	outb(0x5e, SMBUS);

	outw(inw(pm_base + 0x00), pm_base + 0x00);
	outw(inw(pm_base + 0x68) | 0x800, pm_base + 0x68);

	outb(0x8b, 0x70);
	v = inb(0x71);
	outb(v & 0x7f, 0x71);
	outb(v | 0x80, 0x71);
	outb(v & 0x7f, 0x71);

	outb(0xfa, 0x70);
	if ((inb(0x71) & 0x30) != 0x30)
		pci_and_config8(LPC, GEN_PMCON_3, 0xfe);
}

/* 0xecd3e, 0xecd5c: ECAM enable and PEG port status/fix-up table. */
static void peg_table(void)
{
	static const struct {
		u16 off;
		u32 and, or;
	} t[] = {
		{ 0x1cc, 0xffffffff, 0x00001000 }, { 0x114, 0x00000001, 0x00000000 },
		{ 0x200, 0xf3ffffff, 0x00000000 }, { 0x006, 0xffff0000, 0x0000ffff },
		{ 0x01e, 0xffff0000, 0x0000ffff }, { 0x0aa, 0xf3ff0000, 0x0400ffff },
		{ 0x1c4, 0xffffffff, 0xffffffff }, { 0x1d0, 0xffffffff, 0xffffffff },
		{ 0x1f0, 0xffffffff, 0xffffffff },
	};
	unsigned int i;

	pci_or_config8(HOST_BRIDGE, D0F0_PCIEXBAR_LO, 1);
	/* Dword accesses at the vendor offsets, unaligned ones included. */
	for (i = 0; i < ARRAY_SIZE(t); i++)
		ecam_rmw32(PEG_ECAM + t[i].off, t[i].and, t[i].or);
}

/* 0xecb8e: graphics control as the stock MRC sees it (setup default). */
static void graphics_control(void)
{
	pci_write_config16(HOST_BRIDGE, D0F0_GGC,
			   (pci_read_config16(HOST_BRIDGE, D0F0_GGC) & 0xfc0f) | 0x0130);
}

/* 0xecc1a: wait for the TPM access register to report valid. */
static void tpm_wait(void)
{
	int i;

	for (i = 0; i < 1000 && !(read8p(0xfed40000) & 0x80); i++)
		udelay(1);
}

/* 0xec53d: SMBus host enable and subsystem ID (the base stays Coreboot's). */
static void smbus_ssid(void)
{
	const pci_devfn_t smb = PCI_DEV(0, 0x1f, 3);

	pci_write_config32(smb, 0x40, 1);
	pci_or_config16(smb, PCI_COMMAND, 1);
	pci_write_config32(smb, PCI_SUBSYSTEM_VENDOR_ID, 0x0df7105b);
}

void g31mx_stock_preinit(void)
{
	lpc_table();
	pmcon3();
	ich_gpio();
	rcba_cir();
	vc_setup();
	root_complex();
	kbc();
	superio();
	clockgen();
	legacy();
	loader_pm();
	peg_table();
	graphics_control();
	tpm_wait();
	smbus_ssid();
	RCBA32(GCS) |= 1 << 5;
}
