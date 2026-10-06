/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "g31_limits.h"

int g31_script30_index(int cas, int mem_clk, unsigned int entries)
{
	int idx;

	if (mem_clk < 1 || mem_clk > 3)
		return -1;
	idx = (cas - 4) + (mem_clk - 1) * 3;
	if (idx < 0 || (unsigned int)idx >= entries)
		return -1;
	return idx;
}

int g31_rl_candidate(uint32_t total, uint32_t candidate, uint32_t fsb_period)
{
	uint32_t quotient;

	if (!fsb_period || total < candidate)
		return -1;
	quotient = (total - candidate) / fsb_period;
	if (quotient > 32)
		return -1;
	return quotient ? quotient - 1 : 0;
}

int g31_initial_read_latency(uint8_t offset, uint8_t cas, uint16_t tck_ps,
			     uint16_t fsb_period_ps)
{
	uint32_t value;

	if (cas < 3 || cas > 6 || !tck_ps || !fsb_period_ps)
		return -1;
	value = ((uint32_t)offset + cas + 7) * tck_ps / fsb_period_ps;
	return value <= 0x1f ? value : -1;
}

uint32_t g31_initial_refresh_control(uint32_t current, uint16_t trefi)
{
	/* OEM masks only the low word; preserve the upper refresh-controller fields. */
	return (current & 0xffffc000) | trefi | 0x000fc000;
}
