/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __NORTHBRIDGE_INTEL_G31_LIMITS_H__
#define __NORTHBRIDGE_INTEL_G31_LIMITS_H__

#include <stdint.h>

int g31_script30_index(int cas, int mem_clk, unsigned int entries);
int g31_rl_candidate(uint32_t total, uint32_t candidate, uint32_t fsb_period);
int g31_initial_read_latency(uint8_t offset, uint8_t cas, uint16_t tck_ps,
			     uint16_t fsb_period_ps);
uint32_t g31_initial_refresh_control(uint32_t current, uint16_t trefi);

#endif /* __NORTHBRIDGE_INTEL_G31_LIMITS_H__ */
