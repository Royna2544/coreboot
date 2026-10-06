/* SPDX-License-Identifier: GPL-2.0-only */

#define __SIMPLE_DEVICE__

#include <bootstate.h>
#include <console/console.h>
#include <southbridge/intel/common/pmbase.h>
#include <southbridge/intel/common/pmutil.h>

static void g31mx_smi_check(void *unused)
{
	const u32 smi_en = read_pmbase32(SMI_EN);
	const u32 required = APMC_EN | GBL_SMI_EN | SLP_SMI_EN;

	/* EOS may clear during arbitration; only persistent route enables are required. */
	if ((smi_en & required) != required) {
		post_code(0xc7);
		die("G31MX: SMI activation did not latch\n");
	}
}

/* After MP/SMM initialization, before LPC finalization applies SMI_LOCK. */
BOOT_STATE_INIT_ENTRY(BS_DEV_INIT, BS_ON_EXIT, g31mx_smi_check, NULL);
