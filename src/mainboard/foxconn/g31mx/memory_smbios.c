/* SPDX-License-Identifier: GPL-2.0-only */

#include <smbios.h>
#include <stdio.h>

void smbios_fill_dimm_asset_tag(const struct dimm_info *dimm, struct smbios_type17 *t)
{
	// TODO: Replace the zero placeholder with a real user-assigned DIMM asset tag.
	t->asset_tag = 0;
}

void smbios_fill_dimm_locator(const struct dimm_info *dimm, struct smbios_type17 *t)
{
	char locator[48];

	// TODO: Replace logical names and the absent bank with verified PCB labels.
	snprintf(locator, sizeof(locator), "Channel-%d-DIMM-%d (logical)",
		dimm->channel_num, dimm->dimm_num);
	t->device_locator = smbios_add_string(t->eos, locator);
	t->bank_locator = 0;
}
