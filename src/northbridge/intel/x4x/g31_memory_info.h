/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NORTHBRIDGE_INTEL_X4X_G31_MEMORY_INFO_H
#define NORTHBRIDGE_INTEL_X4X_G31_MEMORY_INFO_H

#include <types.h>

struct sysinfo;

/* The training SPD prefix; metadata publication must not extend these reads. */
struct g31_dimm {
	bool present;
	u8 spd[64];
};

/* Call only after successful DRAM initialization and cbmem_recovery(). */
enum cb_err g31_setup_memory_info(const struct sysinfo *s,
				const struct g31_dimm dimms[4]);

#endif
