// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_COPY_H
#define _NVMEVIRT_COPY_H

#include <linux/types.h>
#include "nvmev.h"

#define NVMEV_COPY_FORMAT_0 0
#define NVMEV_COPY_MSSRL 1024U
#define NVMEV_COPY_MCL 1024U
#define NVMEV_COPY_MSRC 127U

struct nvmev_copy_range {
	u64 slba;
	u32 nlb;
};

struct nvmev_copy_ctx {
	u64 sdlba;
	u32 nr_ranges;
	u32 total_lbas;
	u32 desc_bytes;
	u32 status;
};

bool nvmev_copy_supported_ns(struct nvmev_ns *ns);
int nvmev_copy_load_ranges(struct nvmev_ns *ns, struct nvme_copy_command *cmd,
			   struct nvmev_copy_ctx *ctx, struct nvmev_copy_range **ranges);
void nvmev_copy_free_ranges(struct nvmev_copy_range *ranges);

#endif
