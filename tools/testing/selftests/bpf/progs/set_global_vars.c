// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2025 Meta Platforms, Inc. and affiliates. */
#include "bpf_experimental.h"
#include <bpf/bpf_helpers.h>
#include "bpf_misc.h"
#include <stdbool.h>

char _license[] SEC("license") = "GPL";

enum Enum { EA1 = 0, EA2 = 11 };
enum Enumu64 {EB1 = 0llu, EB2 = 12llu };
enum Enums64 { EC1 = 0ll, EC2 = 13ll };

const volatile __s64 var_s64 = -1;
const volatile __u64 var_u64 = 0;
const volatile __s32 var_s32 = -1;
const volatile __u32 var_u32 = 0;
const volatile __s16 var_s16 = -1;
const volatile __u16 var_u16 = 0;
const volatile __s8 var_s8 = -1;
const volatile __u8 var_u8 = 0;
const volatile enum Enum var_ea = EA1;
const volatile enum Enumu64 var_eb = EB1;
const volatile enum Enums64 var_ec = EC1;
const volatile bool var_b = false;

struct Struct {
	int:16;
	__u16 filler;
	struct {
		const __u16 filler2;
	};
	struct Struct2 {
		__u16 filler;
		volatile struct {
			const int:1;
			union {
				const volatile __u8 var_u8;
				const volatile __s16 filler3;
				const int:1;
			} u;
		};
	} struct2;
};

const volatile __u32 stru = 0; /* same prefix as below */
const volatile struct Struct struct1 = {.struct2 = {.u = {.var_u8 = 1}}};

union Union {
	__u16 var_u16;
	struct Struct3 {
		struct {
			__u8 var_u8_l;
		};
		struct {
			struct {
				__u8 var_u8_h;
			};
		};
	} struct3;
};

const volatile union Union union1 = {.var_u16 = -1};

char arr[4] = {0};

SEC("socket")
int test_set_globals(void *ctx)
{
	volatile __s8 a;

	a = var_s64;
	a = var_u64;
	a = var_s32;
	a = var_u32;
	a = var_s16;
	a = var_u16;
	a = var_s8;
	a = var_u8;
	a = var_ea;
	a = var_eb;
	a = var_ec;
	a = var_b;
	a = struct1.struct2.u.var_u8;
	a = union1.var_u16;

	return a;
}
