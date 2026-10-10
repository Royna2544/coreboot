/* SPDX-License-Identifier: GPL-2.0-only */

#include <boot/coreboot_tables.h>
#include <drivers/option/cfr_frontend.h>
#include <southbridge/intel/common/pmutil.h>

static const struct sm_object power_on_after_fail = SM_DECLARE_ENUM({
	.opt_name	= "power_on_after_fail",
	.ui_name	= "Restore AC Power Loss",
	.ui_helptext	= "Specify what to do when power is re-applied after a power loss.",
	.default_value	= CONFIG_MAINBOARD_POWER_FAILURE_STATE,
	.values		= (const struct sm_enum_value[]) {
				{ "Power off (S5)", MAINBOARD_POWER_OFF		},
				{ "Power on  (S0)", MAINBOARD_POWER_ON		},
				{ "Previous state", MAINBOARD_POWER_KEEP	},
				SM_ENUM_VALUE_END				},
});

static const struct sm_object wake_on_lan = SM_DECLARE_BOOL({
	.opt_name	= "wake_on_lan",
	.ui_name	= "Wake on LAN",
	.ui_helptext	= "Let the onboard network controller wake the system from S3-S5."
			  " The operating system must also enable wake-up on the adapter.",
	.default_value	= true,
});

static const struct sm_object cpu_fan_mode = SM_DECLARE_ENUM({
	.opt_name	= "cpu_fan_mode",
	.ui_name	= "CPU Fan Control",
	.ui_helptext	= "Full: CPU_FAN always at full speed. Smart: CPU_FAN follows the CPU"
			  " temperature (50% at 40 C, full speed at 70 C). SYS_FAN always"
			  " runs at full speed.",
	.default_value	= 0,
	.values		= (const struct sm_enum_value[]) {
				{ "Full speed",	0	},
				{ "Smart",	1	},
				SM_ENUM_VALUE_END	},
});

static const struct sm_object nmi = SM_DECLARE_BOOL({
	.opt_name	= "nmi",
	.ui_name	= "Non-Maskable Interrupts",
	.ui_helptext	= "Enable NMI sources (IOCHK#).",
	.default_value	= false,
});

static struct sm_obj_form power = {
	.ui_name = "Power",
	.obj_list = (const struct sm_object *[]) {
		&power_on_after_fail,
		&wake_on_lan,
		NULL
	},
};

static struct sm_obj_form hwmon = {
	.ui_name = "Hardware Monitor",
	.obj_list = (const struct sm_object *[]) {
		&cpu_fan_mode,
		NULL
	},
};

static struct sm_obj_form system = {
	.ui_name = "System",
	.obj_list = (const struct sm_object *[]) {
		&nmi,
		NULL
	},
};

static struct sm_obj_form *sm_root[] = {
	&system,
	&power,
	&hwmon,
	NULL
};

void mb_cfr_setup_menu(struct lb_cfr *cfr_root)
{
	cfr_write_setup_menu(cfr_root, sm_root);
}
