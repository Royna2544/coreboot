/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded G31 receive-enable search, from BLMRC220.006 at 0xfffb8d64. */
#include "g31_rcven.h"

int g31_rcven_search(uint8_t start, const struct g31_rcven_ops *ops, void *ctx,
		     struct g31_rcven_result *result)
{
	uint8_t delay = start;
	uint8_t fine;

	if (!ops || !ops->set_delay || !ops->set_fine || !ops->sample || !result ||
	    start > 63)
		return -1;

	/* Seek the end of a run of high samples. */
	for (;;) {
		ops->set_delay(ctx, delay);
		if (!ops->sample(ctx, true, 3))
			break;
		if (delay == 63)
			return -1;
		delay++;
	}

	/* Confirm the following coarse setting samples low. */
	if (delay == 63)
		return -1;
	delay++;
	ops->set_delay(ctx, delay);
	if (!ops->sample(ctx, false, 3)) {
		delay--;
		ops->set_delay(ctx, delay);
	}

	/* Advance the phase interpolator until the first high result. */
	ops->set_fine(ctx, 0);
	for (fine = 0; fine <= 0x0e; fine++) {
		if (ops->sample(ctx, true, 3))
			break;
		if (fine == 0x0e)
			return -1;
		ops->set_fine(ctx, fine + 1);
	}

	/* Find the low edge while stepping coarse backwards by four. */
	if (delay < 3)
		return -1;
	delay -= 3;
	for (;;) {
		ops->set_delay(ctx, delay);
		if (!ops->sample(ctx, true, 1)) {
			result->coarse = delay;
			result->fine = fine;
			return 0;
		}
		if (delay < 4)
			return -1;
		delay -= 4;
	}
}

int g31_rcven_pack(const uint8_t offsets[8], uint16_t regs[2])
{
	unsigned int lane;

	if (!offsets || !regs)
		return -1;
	for (lane = 0; lane < 8; lane++)
		if (offsets[lane] > 15)
			return -1;
	for (lane = 0; lane < 2; lane++)
		regs[lane] = offsets[4 * lane] |
			     (uint16_t)offsets[4 * lane + 1] << 4 |
			     (uint16_t)offsets[4 * lane + 2] << 8 |
			     (uint16_t)offsets[4 * lane + 3] << 12;
	return 0;
}

int g31_rcven_install(const uint8_t offsets[8], uint32_t base,
		      void (*write)(void *ctx, uint32_t offset, uint16_t value),
		      void *ctx)
{
	uint16_t regs[2];

	if (!write || g31_rcven_pack(offsets, regs))
		return -1;
	write(ctx, base, regs[0]);
	write(ctx, base + 4, regs[1]);
	return 0;
}
