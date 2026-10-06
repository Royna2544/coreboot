/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Success-path per-bytelane phase calculation from BLMRC220.006. */
#include "g31_phase.h"

uint8_t g31_post_sweep_5dc(uint8_t value)
{
	return (value & ~0x04) | 0x08;
}

void g31_dll_status_apply(uint8_t selected,
			  const struct g31_dll_status_ops *ops, void *ctx)
{
	ops->write(ctx, (ops->read(ctx) & 0xc0) | selected);
	ops->write(ctx, ops->read(ctx) | 1);
}

void g31_analog_19c_program(uint16_t selected,
			    const struct g31_analog_19c_ops *ops, void *ctx)
{
	ops->write(ctx, (ops->read(ctx) & 0xfff0) | selected);
	ops->write(ctx, (ops->read(ctx) & 0xff00) | selected);
	ops->write(ctx, (ops->read(ctx) & 0xf000) | selected);
	ops->write(ctx, selected);
}

void g31_timing_12d_program(const struct g31_timing_12d_ops *ops, void *ctx)
{
	uint8_t value = (ops->read32(ctx, 0x252) >> 13) & 0x0f;

	ops->write8(ctx, (ops->read8(ctx) & 0x0f) | (value << 4));
	value = (ops->read32(ctx, 0x258) >> 17) & 0x0f;
	ops->write8(ctx, (ops->read8(ctx) & 0xf0) | value);
	ops->write8(ctx, (ops->read8(ctx) & 0x03) | 0x90);
	ops->write8(ctx, (ops->read8(ctx) & 0xfe) | 0x02);
	ops->write8(ctx, ops->read8(ctx) & 0xfe);
}

void g31_post_sweep_channels(unsigned int populated,
			     const struct g31_post_sweep_ops *ops, void *ctx)
{
	unsigned int ch;

	for (ch = 0; ch < 2; ch++)
		ops->mask_channel(ctx, ch);
	ops->enable_channels(ctx);
	for (ch = 0; ch < 2; ch++)
		if (populated & (1 << ch))
			ops->program_channel(ctx, ch);
}

int g31_phase_calculate(const uint8_t records[48], unsigned int coarse,
			unsigned int fine, unsigned int unit, unsigned int fine5,
			unsigned int offset, unsigned int half, unsigned int limit,
			struct g31_phase_regs *regs)
{
	uint8_t a[12], b[12], c[12], d[12], e[12];
	unsigned int base, step, j, i;

	if (!records || !regs || !unit || fine5 < 5 ||
	    coarse > 15 || fine > 7 || !half)
		return -1;
	step = fine5 / 5;
	base = coarse * unit + fine * fine5 / 5;
	*regs = (struct g31_phase_regs){0};

	for (j = 0; j < 12; j++) {
		const uint8_t *r = &records[j * 4];
		unsigned int value, rem, f, q;

		if (r[0] > 3)
			return -1;
		a[j] = r[0];
		value = base + offset + r[2] * fine5 / 5 + r[1] * unit - offset;
		if (value >= half)
			value -= half;
		q = value / unit;
		rem = value % unit;
		f = (rem + step - 1) / step;
		if (f > 7) {
			q++;
			f = 0;
		}
		/* Do not silently truncate the OEM's alternate wrap-boundary path. */
		if (q >= 15)
			return -1;
		b[j] = q;
		c[j] = f;

		if (value >= base && value - base <= limit) {
			d[j] = 1;
			e[j] = 1;
		} else if ((value < base && base - value <= limit) ||
			   (base - value + half) <= limit) {
			d[j] = 1;
			e[j] = 0;
		} else {
			d[j] = 0;
			e[j] = 0;
		}
	}

	for (i = 0; i < 4; i++) {
		regs->coarse[0] |= (uint16_t)b[i + 8] << (4 * i);
		regs->coarse[1] |= (uint16_t)b[i + 4] << (4 * i);
		regs->fine[0] |= (uint16_t)c[i + 8] << (3 * i);
		regs->fine[1] |= (uint16_t)c[i + 4] << (3 * i);
	}
	/* The OEM packs lanes 0, 2, 1, 3 in this register pair. */
	regs->coarse[2] = b[0] | b[2] << 4 | b[1] << 8 | b[3] << 12;
	regs->fine[2] = c[0] | c[2] << 3 | c[1] << 6 | c[3] << 9;
	regs->ctrl_5d0 = a[3] << 7 | d[3] << 6 | e[3] << 5;
	regs->ctrl_5d8 = a[1] << 22 | d[1] << 21 | e[1] << 20;
	regs->ctrl_5dc = a[0] << 14 | d[0] << 10 | e[0] << 6 |
			 a[2] << 16 | d[2] << 11 | e[2] << 7;
	for (j = 4; j < 8; j++)
		regs->ctrl_5e0 |= (uint32_t)a[j] << (24 + 2 * (j - 4)) |
				     (uint32_t)d[j] << (8 + j) |
				     (uint32_t)e[j] << j;
	for (j = 8; j < 12; j++)
		regs->ctrl_5e0 |= (uint32_t)a[j] << (2 * j) |
				     (uint32_t)d[j] << j |
				     (uint32_t)e[j] << (j - 8);
	return 0;
}
