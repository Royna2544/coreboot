# SPDX-License-Identifier: GPL-2.0-only

ifeq ($(CONFIG_NORTHBRIDGE_INTEL_X4X),y)

bootblock-y += bootblock.c

romstage-y += early_init.c
romstage-y += raminit.c
romstage-y += raminit_ddr23.c
romstage-y += memmap.c
romstage-y += rcven.c
romstage-y += raminit_tables.c
romstage-y += dq_dqs.c
romstage-y += romstage.c

ifeq ($(CONFIG_NORTHBRIDGE_INTEL_G31),y)
romstage-y += g31_raminit.c
romstage-y += g31_memory_info.c
romstage-y += g31_tables.c
romstage-y += g31_cold.c
romstage-y += g31_limits.c
romstage-y += g31_phase.c
romstage-y += g31_prepass.c
romstage-y += g31_rcven.c
romstage-y += g31_warm.c
romstage-y += g31_mrc_services.c
romstage-y += g31_train.c
endif

ramstage-y += acpi.c
ramstage-y += memmap.c
ramstage-y += gma.c
ramstage-y += northbridge.c

postcar-y += memmap.c

endif
