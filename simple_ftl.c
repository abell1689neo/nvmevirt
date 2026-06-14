// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "simple_ftl.h"
#include "copy.h"

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(nvmev_vdev->config.cpu_nr_dispatcher);
}

static size_t __cmd_io_size(struct nvme_rw_command *cmd)
{
	NVMEV_DEBUG_VERBOSE("[%c] %llu + %d, prp %llx %llx\n",
			    cmd->opcode == nvme_cmd_write ? 'W' : 'R',
			    le64_to_cpu(cmd->slba), le16_to_cpu(cmd->length),
			    le64_to_cpu(cmd->prp1), le64_to_cpu(cmd->prp2));

	return (le16_to_cpu(cmd->length) + 1) << LBA_BITS;
}

/* Return the time to complete */
static unsigned long long __schedule_io_units(int opcode, unsigned long lba, unsigned int length,
					      unsigned long long nsecs_start)
{
	unsigned int io_unit_size = 1 << nvmev_vdev->config.io_unit_shift;
	unsigned int io_unit =
		(lba >> (nvmev_vdev->config.io_unit_shift - LBA_BITS)) % nvmev_vdev->config.nr_io_units;
	int nr_io_units = min(nvmev_vdev->config.nr_io_units, DIV_ROUND_UP(length, io_unit_size));

	unsigned long long latest; /* Time of completion */
	unsigned int delay = 0;
	unsigned int latency = 0;
	unsigned int trailing = 0;

	if (opcode == nvme_cmd_write) {
		delay = nvmev_vdev->config.write_delay;
		latency = nvmev_vdev->config.write_time;
		trailing = nvmev_vdev->config.write_trailing;
	} else if (opcode == nvme_cmd_read) {
		delay = nvmev_vdev->config.read_delay;
		latency = nvmev_vdev->config.read_time;
		trailing = nvmev_vdev->config.read_trailing;
	}

	latest = max(nsecs_start, nvmev_vdev->io_unit_stat[io_unit]) + delay;

	do {
		latest += latency;
		nvmev_vdev->io_unit_stat[io_unit] = latest;

		if (nr_io_units-- > 0) {
			nvmev_vdev->io_unit_stat[io_unit] += trailing;
		}

		length -= min(length, io_unit_size);
		if (++io_unit >= nvmev_vdev->config.nr_io_units)
			io_unit = 0;
	} while (length > 0);

	return latest;
}

static unsigned long long __schedule_flush(struct nvmev_request *req)
{
	unsigned long long latest = 0;
	int i;

	for (i = 0; i < nvmev_vdev->config.nr_io_units; i++) {
		latest = max(latest, nvmev_vdev->io_unit_stat[i]);
	}

	return latest;
}

static bool simple_copy(struct nvmev_ns *ns, struct nvmev_request *req,
			struct nvmev_result *ret)
{
	struct nvmev_copy_ctx ctx;
	struct nvmev_copy_range *ranges = NULL;
	unsigned long long latest = req->nsecs_start;
	u32 i;
	int err;

	err = nvmev_copy_load_ranges(ns, &req->cmd->copy, &ctx, &ranges);
	if (err) {
		ret->status = NVME_SC_INTERNAL;
		ret->bytes = 0;
		ret->nsecs_target = req->nsecs_start;
		return true;
	}

	if (ctx.status != NVME_SC_SUCCESS) {
		ret->status = ctx.status;
		ret->bytes = 0;
		ret->nsecs_target = req->nsecs_start;
		return true;
	}

	for (i = 0; i < ctx.nr_ranges; i++) {
		unsigned long long source_done;

		source_done = __schedule_io_units(nvme_cmd_read, ranges[i].slba,
						  LBA_TO_BYTE(ranges[i].nlb),
						  req->nsecs_start);
		latest = max(latest, source_done);
	}

	latest = __schedule_io_units(nvme_cmd_write, ctx.sdlba,
				     LBA_TO_BYTE(ctx.total_lbas), latest);

	ret->status = NVME_SC_SUCCESS;
	ret->bytes = LBA_TO_BYTE(ctx.total_lbas);
	ret->nsecs_target = latest;
	ret->copy_sdlba = ctx.sdlba;
	ret->copy_nr_ranges = ctx.nr_ranges;
	ret->copy_desc_bytes = ctx.desc_bytes;
	ret->copy_source_read_bytes = ret->bytes;
	ret->copy_destination_write_bytes = ret->bytes;
	ret->copy_host_payload_avoided_bytes = ret->bytes * 2;
	ret->copy_ranges = ranges;
	return true;
}

bool simple_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			     struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;

	BUG_ON(ns->csi != NVME_CSI_NVM);
	BUG_ON(BASE_SSD != INTEL_OPTANE);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
	case nvme_cmd_read:
		ret->nsecs_target = __schedule_io_units(
			cmd->common.opcode, le64_to_cpu(cmd->rw.slba),
			__cmd_io_size((struct nvme_rw_command *)cmd), __get_wallclock());
		ret->bytes = __cmd_io_size((struct nvme_rw_command *)cmd);
		ret->status = NVME_SC_SUCCESS;
		break;
	case nvme_cmd_copy:
		if (!simple_copy(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		ret->nsecs_target = __schedule_flush(req);
		ret->bytes = 0;
		ret->status = NVME_SC_SUCCESS;
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
			    nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		ret->status = NVME_SC_INVALID_OPCODE;
		ret->bytes = 0;
		ret->nsecs_target = req->nsecs_start;
		break;
	}

	return true;
}

void simple_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			   uint32_t cpu_nr_dispatcher)
{
	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->size = size;
	ns->mapped = mapped_addr;
	ns->proc_io_cmd = simple_proc_nvme_io_cmd;

	return;
}

void simple_remove_namespace(struct nvmev_ns *ns)
{
	// Nothing to do here
}
