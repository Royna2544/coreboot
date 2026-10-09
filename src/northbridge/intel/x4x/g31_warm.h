/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __NORTHBRIDGE_INTEL_G31_WARM_H__
#define __NORTHBRIDGE_INTEL_G31_WARM_H__

#include <stdbool.h>
#include <stdint.h>

/* These match the x4x boot-path values; OEM MRC uses bits 4, 1 and 2. */
enum g31_boot_path {
	G31_BOOT_COLD = 0,
	G31_BOOT_WARM = 1,
	G31_BOOT_RESUME = 2,
};

enum g31_warm_error {
	G31_WARM_OK = 0,
	G31_WARM_UNSAFE_RESET = -1,
	G31_WARM_UNTRAINED = -2,
	G31_WARM_CLOCK_MISMATCH = -3,
};

enum g31_mrc_optional_step {
	G31_STEP_CLOCK_CFG,
	G31_STEP_PATTERN_SETUP,
	G31_STEP_DLL_SWEEP,
	G31_STEP_SIGNAL_GROUPS,
	G31_STEP_WAIT_CAL,
	G31_STEP_RCVEN_TRAIN,
	G31_STEP_RCVEN_RESTORE,
	G31_STEP_JEDEC,
	G31_STEP_AFTER_REFRESH,
	G31_STEP_RCVEN_RESUME,
};

struct g31_warm_ops {
	uint8_t (*read8)(void *ctx, uint32_t offset);
	uint16_t (*read16)(void *ctx, uint32_t offset);
	uint32_t (*read32)(void *ctx, uint32_t offset);
	void (*write32)(void *ctx, uint32_t offset, uint32_t value);
};

struct g31_warm_result {
	uint8_t coarse[2];
	uint8_t offset[2][8];
};

enum g31_boot_path g31_select_boot_path(bool resume, uint32_t pmsts,
					 uint8_t coarse_out);
int g31_warm_preflight(enum g31_boot_path path, uint32_t pmsts,
		       uint8_t coarse_out, uint8_t channels,
		       unsigned int selected_memclk, unsigned int retained_memclk);
bool g31_step_selected(enum g31_mrc_optional_step step, enum g31_boot_path path);
int g31_warm_restore(const struct g31_warm_ops *ops, void *ctx,
		     uint8_t expected_coarse, struct g31_warm_result *result);
void g31_resume_restore(const struct g31_warm_ops *ops, void *ctx,
			const struct g31_warm_result *saved);

#endif /* __NORTHBRIDGE_INTEL_G31_WARM_H__ */
