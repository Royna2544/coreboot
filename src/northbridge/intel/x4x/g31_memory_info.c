/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <cbmem.h>
#include <commonlib/helpers.h>
#include <device/dram/ddr2.h>
#include <memory_info.h>
#include <smbios.h>
#include <string.h>

#include "g31.h"
#include "g31_memory_info.h"

static u16 g31_spd_max_speed(u8 tck)
{
	/* DDR2 SPD byte 9: integer ns and tenths, with four special fractions. */
	static const u16 fraction_ps[4] = { 250, 330, 660, 750 };
	const unsigned int fraction = tck & 0x0f;
	unsigned int period = (tck >> 4) * 1000;

	if (!tck || fraction >= 0x0e)
		return 0;
	period += fraction < 0x0a ? fraction * 100 : fraction_ps[fraction - 0x0a];
	return (2000000 + period / 2) / period;
}

enum cb_err g31_setup_memory_info(const struct sysinfo *s,
				const struct g31_dimm dimms[4])
{
	u32 dimm_size[TOTAL_DIMMS] = { 0 };
	u32 channel_size[TOTAL_CHANNELS] = { 0 };
	unsigned int slots[TOTAL_CHANNELS] = { 0 };
	struct memory_info *info;

	if (s->spd_type != DDR2 || s->selected_timings.mem_clk < MEM_CLOCK_533MHz ||
	    s->selected_timings.mem_clk > MEM_CLOCK_800MHz)
		return CB_ERR;

	/* Validate before allocation: a rejected state must not publish a partial array. */
	for (unsigned int i = 0; i < TOTAL_DIMMS; i++) {
		const struct dimminfo *geometry = &s->dimms[i];
		const u8 *spd = dimms[i].spd;
		const unsigned int ch = i / DIMMS_PER_CHANNEL;
		unsigned int index, code;

		if (dimms[i].present != DIMM_IS_POPULATED(s->dimms, i))
			return CB_ERR;
		if (!s->spd_map[i]) {
			if (dimms[i].present)
				return CB_ERR;
			continue;
		}
		/* G31 supports one routed DIMM per channel, not x4x's four sockets. */
		if (++slots[ch] > 1 || s->spd_map[i] >= 0x80)
			return CB_ERR;
		for (unsigned int j = 0; j < i; j++)
			if (s->spd_map[j] == s->spd_map[i])
				return CB_ERR;
		if (!dimms[i].present)
			continue;

		if (spd[SPD_MEMORY_TYPE] != DDR2SPD ||
		    spd[SPD_MODULE_DATA_WIDTH_LSB] != 64 || spd[SPD_MODULE_DATA_WIDTH_MSB] ||
		    spd[11] || spd[SPD_ERROR_CHECKING_SDRAM_WIDTH] ||
		    geometry->rows < 12 || geometry->rows > 15 ||
		    geometry->cols < 9 || geometry->cols > 10 ||
		    (geometry->width != CHIP_WIDTH_x8 && geometry->width != CHIP_WIDTH_x16) ||
		    (geometry->n_banks != N_BANKS_4 && geometry->n_banks != N_BANKS_8) ||
		    geometry->ranks < 1 || geometry->ranks > 2 ||
		    (geometry->width == CHIP_WIDTH_x16 && geometry->ranks != 1))
			return CB_ERR;

		/* Use the same DRA/rank-size tables as the final programmed DRB boundaries. */
		index = (geometry->rows - 12) + 4 * ((geometry->cols - 9) +
			2 * ((geometry->width == CHIP_WIDTH_x16) +
				2 * (geometry->n_banks == N_BANKS_8)));
		code = g31_dra_code[index];
		if (code >= ARRAY_SIZE(g31_rank_size))
			return CB_ERR;
		dimm_size[i] = (g31_rank_size[code] << 6) * geometry->ranks;
		channel_size[ch] += dimm_size[i];
	}
	if (!channel_size[0] && !channel_size[1])
		return CB_ERR;
	if (channel_size[0] + channel_size[1] > 4096 ||
	    channel_size[0] != s->channel_capacity[0] ||
	    channel_size[1] != s->channel_capacity[1])
		return CB_ERR;

	info = cbmem_add(CBMEM_ID_MEMINFO, sizeof(*info));
	if (!info)
		return CB_ERR;
	memset(info, 0, sizeof(*info));
	info->ecc_type = MEMORY_ARRAY_ECC_NONE;
	/* Intel G31/P31 datasheet 317495-001: maximum supported DRAM is 4 GiB. */
	info->max_capacity_mib = 4096;

	for (unsigned int i = 0; i < TOTAL_DIMMS; i++) {
		if (!s->spd_map[i])
			continue;
		struct dimm_info *d = &info->dimm[info->dimm_cnt++];

		info->number_of_devices++;
		d->channel_num = i / DIMMS_PER_CHANNEL;
		d->dimm_num = i % DIMMS_PER_CHANNEL;
		d->bank_locator = d->channel_num;
		if (!dimms[i].present)
			continue;

		const struct dimminfo *geometry = &s->dimms[i];

		d->dimm_size = dimm_size[i];
		d->ddr_type = MEMORY_TYPE_DDR2;
		d->configured_speed_mts = g31_mem_rate[s->selected_timings.mem_clk];
		d->max_speed_mts = g31_spd_max_speed(dimms[i].spd[9]);
		d->rank_per_dimm = geometry->ranks;
		d->mod_type = dimms[i].spd[20] & SPD_DDR2_DIMM_TYPE_MASK;
		d->bus_width = MEMORY_BUS_WIDTH_64;
		// TODO: Read module identity; these zero/Unknown values are placeholders.
		d->mod_id = 0;
		memset(d->serial, 0, sizeof(d->serial));
		memcpy(d->module_part_number, "Unknown", sizeof("Unknown"));
	}
	return CB_SUCCESS;
}
