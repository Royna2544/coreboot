## SPDX-License-Identifier: GPL-2.0-only

bootblock-y += early_init.c
romstage-y += early_init.c
romstage-y += gpio.c
romstage-y += stock_preinit.c
ramstage-y += cstates.c
ramstage-y += mainboard.c
ramstage-y += peg.c
ramstage-y += power.c
ramstage-y += cpu_smbios.c
ramstage-y += memory_smbios.c
