/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded state machine for BLMRC220.006 prepass at PE 0xfffb84b6. */
#include "g31_prepass.h"

static bool launch(const struct g31_prepass_ops *ops, void *ctx,
		   unsigned int *count, unsigned int max_launches)
{
	if (*count >= max_launches)
		return false;
	++*count;
	ops->launch(ctx);
	return true;
}

static uint32_t sample(const struct g31_prepass_ops *ops, void *ctx)
{
	ops->arm(ctx);
	return ops->result(ctx);
}

uint32_t g31_dll_first_sample(const struct g31_dll_sample_ops *ops, void *ctx)
{
	ops->step(ctx);
	ops->arm(ctx);
	return ops->result(ctx);
}

int g31_edge_prepass(unsigned int budget, bool special, unsigned int max_launches,
		     const struct g31_prepass_ops *ops, void *ctx)
{
	unsigned int count = 0, n, i;
	uint32_t errors;

	if (!ops || !ops->launch || !ops->arm || !ops->result)
		return -1;
	errors = sample(ops, ctx);
	if (!errors)
		goto zero;
	if (errors == UINT32_MAX)
		goto all;
	goto partial;

partial:
	n = 0;
partial_test:
	if (!launch(ops, ctx, &count, max_launches))
		return -1;
	ops->arm(ctx);
	if (n > 15)
		return 0x10;
	errors = ops->result(ctx);
	if (!errors)
		goto zero;
	if (errors == UINT32_MAX)
		goto all;
	n++;
	goto partial_test;

all:
	n = 0;
all_test:
	if (!launch(ops, ctx, &count, max_launches))
		return -1;
	ops->arm(ctx);
	if (n > 15)
		return 0x08;
	errors = ops->result(ctx);
	if (errors) {
		n++;
		goto all_test;
	}

	n = budget;
	for (i = 0; i < n; i++)
		if (!launch(ops, ctx, &count, max_launches))
			return -1;
	errors = sample(ops, ctx);
	if (errors) {
		if (errors == UINT32_MAX)
			return 0x00;
		if (!n)
			return 0x80;
		if (!special)
			budget--;
		goto partial;
	}
	if (!n)
		return 0x40;
	if (!special)
		budget--;
	goto zero;

zero:
	n = 0;
zero_test:
	if (!launch(ops, ctx, &count, max_launches))
		return -1;
	ops->arm(ctx);
	if (n > 15)
		return 0x02;
	errors = ops->result(ctx);
	if (!errors) {
		n++;
		goto zero_test;
	}
	if (errors == UINT32_MAX)
		goto all;

	n = 0;
partial_to_zero_test:
	if (!launch(ops, ctx, &count, max_launches))
		return -1;
	ops->arm(ctx);
	if (n > 15)
		return 0x04;
	errors = ops->result(ctx);
	if (!errors)
		return 0x01;
	if (errors == UINT32_MAX)
		goto all;
	n++;
	goto partial_to_zero_test;
}
