/* SPDX-License-Identifier: GPL-2.0-or-later */
/* BLMRC220.006 path selection and warm step 0x82 at 0xfffb8b4c. */
#include "g31_warm.h"

#define PMSTS_WRO	0x100
#define PMSTS_SELF_REFRESH	0x03

/* OEM paths are 4 for cold, 1 for warm and 2 for resume. */
enum g31_boot_path g31_select_boot_path(bool resume, uint32_t pmsts,
					 uint8_t coarse_out)
{
	if (resume)
		return G31_BOOT_RESUME;
	if ((pmsts & PMSTS_WRO) && coarse_out)
		return G31_BOOT_WARM;
	return G31_BOOT_COLD;
}

int g31_warm_preflight(enum g31_boot_path path, uint32_t pmsts,
		       uint8_t coarse_out, uint8_t channels,
		       unsigned int selected_memclk, unsigned int retained_memclk)
{
	/* S3 drops PWROK: nothing is retained, the trained values come from flash. */
	if (path == G31_BOOT_RESUME)
		return G31_WARM_OK;
	if (path != G31_BOOT_COLD && path != G31_BOOT_WARM)
		return G31_WARM_UNTRAINED;
	if (!channels || (channels & ~PMSTS_SELF_REFRESH))
		return G31_WARM_UNTRAINED;

	/* G31 requires self-refresh on each populated channel after warm reset. */
	if ((pmsts & PMSTS_WRO) &&
	    (pmsts & channels) != channels)
		return G31_WARM_UNSAFE_RESET;
	if (path == G31_BOOT_COLD && (pmsts & PMSTS_WRO) && coarse_out)
		return G31_WARM_UNTRAINED;

	if (path == G31_BOOT_WARM) {
		if (!(pmsts & PMSTS_WRO) || !coarse_out)
			return G31_WARM_UNTRAINED;
		if (selected_memclk < 1 || selected_memclk > 3 ||
		    selected_memclk != retained_memclk)
			return G31_WARM_CLOCK_MISMATCH;
	}
	return G31_WARM_OK;
}

bool g31_step_selected(enum g31_mrc_optional_step step, enum g31_boot_path path)
{
	/* Masks from the OEM init table at ds:0x4c70, in execution order. */
	static const uint8_t masks[] = {
		0x06, 0x06, 0x06, 0x06, 0x06, 0x04, 0x51, 0x05, 0x05, 0x22,
	};
	uint8_t path_bit;

	if (step < G31_STEP_CLOCK_CFG || step > G31_STEP_RCVEN_RESUME)
		return false;
	switch (path) {
	case G31_BOOT_COLD:
		path_bit = 4;
		break;
	case G31_BOOT_WARM:
		path_bit = 1;
		break;
	case G31_BOOT_RESUME:
		path_bit = 2;
		break;
	default:
		return false;
	}
	return (masks[step] & path_bit) != 0;
}

int g31_warm_restore(const struct g31_warm_ops *ops, void *ctx,
		     uint8_t expected_coarse, struct g31_warm_result *result)
{
	uint8_t coarse;
	unsigned int ch, lane;

	if (!ops || !ops->read8 || !ops->read16 || !ops->read32 ||
	    !ops->write32 || !result || !expected_coarse)
		return -1;

	coarse = ops->read8(ctx, 0xa0c);
	if (coarse != expected_coarse)
		return -1;

	for (ch = 0; ch < 2; ch++) {
		const uint32_t o = ch * 0x400;
		const uint8_t nibble = (coarse >> (ch * 4)) & 0x0f;
		uint16_t lanes[2];
		uint32_t latency;

		result->coarse[ch] = (nibble << 2) |
				     ((ops->read8(ctx, o + 0x53d) >> 2) & 3);
		latency = ops->read32(ctx, o + 0x248);
		latency = (latency & ~0x000f0000U) | ((uint32_t)nibble << 16);
		ops->write32(ctx, o + 0x248, latency);

		lanes[1] = ops->read16(ctx, o + 0x530);
		lanes[0] = ops->read16(ctx, o + 0x52c);
		for (lane = 0; lane < 8; lane++)
			result->offset[ch][lane] =
				(lanes[lane / 4] >> ((lane % 4) * 4)) & 0x0f;
	}
	return 0;
}

static void g31_rmw32(const struct g31_warm_ops *ops, void *ctx, uint32_t offset,
		      uint32_t clear, uint32_t set)
{
	ops->write32(ctx, offset, (ops->read32(ctx, offset) & ~clear) | set);
}

/*
 * Resume step 0x82 (0xfffb8bec): S3 loses the trained receive-enable state,
 * so write back the saved form that warm 0x82 reads from the registers.
 * The OEM byte and word stores are done as aligned dword read-modify-writes.
 */
void g31_resume_restore(const struct g31_warm_ops *ops, void *ctx,
			const struct g31_warm_result *saved)
{
	unsigned int ch, lane;

	for (ch = 0; ch < 2; ch++) {
		const uint32_t o = ch * 0x400;
		const uint8_t coarse = saved->coarse[ch];
		uint32_t lanes = 0;

		for (lane = 0; lane < 8; lane++)
			lanes |= (uint32_t)(saved->offset[ch][lane] & 0x0f) << (lane * 4);

		/* 0x53d bits 3:2 */
		g31_rmw32(ops, ctx, o + 0x53c, 3 << 10, (coarse & 3) << 10);
		g31_rmw32(ops, ctx, o + 0x248, 0x000f0000, ((coarse >> 2) & 0x0f) << 16);
		g31_rmw32(ops, ctx, o + 0x52c, 0xffff, lanes & 0xffff);
		g31_rmw32(ops, ctx, o + 0x530, 0xffff, lanes >> 16);
		g31_rmw32(ops, ctx, o + 0x534, 0xffff, 0x0924);
		g31_rmw32(ops, ctx, o + 0x538, 0xffff, 0x0924);
	}
}
