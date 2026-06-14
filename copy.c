// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/highmem.h>
#include <linux/slab.h>

#include "copy.h"

struct nvmev_prp_mapping {
	void *base;
	void *addr;
	bool memremap;
};

#define NVMEV_PRPS_PER_PAGE (PAGE_SIZE / sizeof(__le64))

static bool __mem_is_zero(const void *buf, size_t len)
{
	const u8 *p = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		if (p[i] != 0)
			return false;
	}

	return true;
}

static bool __map_prp_page(u64 paddr, struct nvmev_prp_mapping *map)
{
	u64 base = paddr & PAGE_MASK;
	size_t offset = paddr & PAGE_OFFSET_MASK;

	if (!paddr)
		return false;

	map->base = NULL;
	map->addr = NULL;
	map->memremap = false;

	if (pfn_valid(base >> PAGE_SHIFT)) {
		map->base = kmap_atomic_pfn(PRP_PFN(base));
	} else {
		map->base = memremap(base, PAGE_SIZE, MEMREMAP_WT);
		map->memremap = true;
	}

	if (!map->base)
		return false;

	map->addr = map->base + offset;
	return true;
}

static void __unmap_prp_page(struct nvmev_prp_mapping *map)
{
	if (!map->base)
		return;

	if (map->memremap)
		memunmap(map->base);
	else
		kunmap_atomic(map->base);

	map->base = NULL;
	map->addr = NULL;
	map->memremap = false;
}

static int __copy_from_prp_data_page(u64 paddr, void *dst, size_t len,
				     size_t *copied, bool allow_offset)
{
	struct nvmev_prp_mapping map;
	size_t page_offs = paddr & PAGE_OFFSET_MASK;
	size_t copy_len;

	if (*copied >= len)
		return 0;
	if (!paddr)
		return -EFAULT;

	if (!allow_offset && page_offs)
		return -EFAULT;

	if (!__map_prp_page(paddr, &map))
		return -EFAULT;

	copy_len = min_t(size_t, len - *copied, PAGE_SIZE - page_offs);
	memcpy(dst + *copied, map.addr, copy_len);
	__unmap_prp_page(&map);
	*copied += copy_len;
	return 0;
}

static int __copy_from_prp_list(u64 list_paddr, void *dst, size_t len,
				size_t *copied)
{
	u32 guard = 0;

	if (!list_paddr || (list_paddr & PAGE_OFFSET_MASK))
		return -EFAULT;

	while (*copied < len) {
		struct nvmev_prp_mapping map;
		__le64 *entries;
		u64 next_list = 0;
		u32 i;
		int ret = 0;

		if (++guard > 16)
			return -EFAULT;

		entries = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!entries)
			return -ENOMEM;

		if (!__map_prp_page(list_paddr, &map)) {
			kfree(entries);
			return -EFAULT;
		}

		memcpy(entries, map.addr, PAGE_SIZE);
		__unmap_prp_page(&map);

		for (i = 0; i < NVMEV_PRPS_PER_PAGE && *copied < len; i++) {
			u64 paddr = le64_to_cpu(entries[i]);

			if (!paddr) {
				ret = -EFAULT;
				break;
			}

			if (i == NVMEV_PRPS_PER_PAGE - 1 &&
			    *copied + PAGE_SIZE < len) {
				next_list = paddr;
				break;
			}

			ret = __copy_from_prp_data_page(paddr, dst, len, copied,
							false);
			if (ret)
				break;
		}

		kfree(entries);
		if (ret)
			return ret;
		if (next_list) {
			if (next_list & PAGE_OFFSET_MASK)
				return -EFAULT;
			list_paddr = next_list;
			continue;
		}
		if (*copied < len)
			return -EFAULT;
	}

	return 0;
}

static int __copy_from_prp(u64 prp1, u64 prp2, void *dst, size_t len)
{
	size_t copied = 0;
	int ret;

	if (!len)
		return 0;

	ret = __copy_from_prp_data_page(prp1, dst, len, &copied, true);
	if (ret || copied == len)
		return ret;

	if (len - copied <= PAGE_SIZE)
		return __copy_from_prp_data_page(prp2, dst, len, &copied,
						 false);

	return __copy_from_prp_list(prp2, dst, len, &copied);
}

static bool __copy_consume_prp_fault_once(void)
{
	if (!nvmev_vdev)
		return false;

	return atomic_dec_if_positive(&nvmev_vdev->copy_prp_fault_once) >= 0;
}

bool nvmev_copy_supported_ns(struct nvmev_ns *ns)
{
	if (!ns || ns->csi != NVME_CSI_NVM)
		return false;
	if (ns->id >= NR_NAMESPACES)
		return false;

	return NS_SSD_TYPE(ns->id) == SSD_TYPE_NVM || NS_SSD_TYPE(ns->id) == SSD_TYPE_CONV;
}

static void __copy_prepare(struct nvmev_ns *ns, struct nvme_copy_command *cmd,
			   struct nvmev_copy_ctx *ctx)
{
	u32 cdw12 = le32_to_cpu(cmd->cdw12);
	u32 desfmt = (cdw12 >> 8) & 0xf;
	u32 unsupported_cdw12 = cdw12 & ~((u32)0xff | ((u32)0xf << 8) |
					  ((u32)1 << 30) | ((u32)1 << 31));

	memset(ctx, 0, sizeof(*ctx));
	ctx->status = NVME_SC_SUCCESS;
	ctx->sdlba = le64_to_cpu(cmd->sdlba);
	ctx->nr_ranges = (cdw12 & 0xff) + 1;
	ctx->desc_bytes = ctx->nr_ranges * sizeof(struct nvme_copy_range);

	if (!nvmev_copy_supported_ns(ns)) {
		ctx->status = NVME_SC_INVALID_OPCODE;
		return;
	}

	if (cmd->flags || cmd->metadata || cmd->dspec || cmd->rsvd13 ||
	    cmd->reftag || cmd->apptag || cmd->appmask || unsupported_cdw12) {
		ctx->status = NVME_SC_INVALID_FIELD;
		return;
	}

	if (desfmt != NVMEV_COPY_FORMAT_0) {
		ctx->status = NVME_SC_INVALID_FIELD;
		return;
	}

	if (ctx->nr_ranges > NVMEV_COPY_MSRC + 1) {
		ctx->status = NVME_SC_CMD_SIZE_LIM_EXCEEDED;
		return;
	}

	if (ctx->desc_bytes > PAGE_SIZE) {
		ctx->status = NVME_SC_CMD_SIZE_LIM_EXCEEDED;
		return;
	}
}

static void __copy_parse_ranges(struct nvmev_ns *ns, struct nvme_copy_range *descs,
				struct nvmev_copy_ctx *ctx, struct nvmev_copy_range *ranges)
{
	u64 max_lbas = ns->size >> LBA_BITS;
	u64 total = 0;
	u32 i;

	for (i = 0; i < ctx->nr_ranges; i++) {
		u64 slba = le64_to_cpu(descs[i].slba);
		u32 nlb = le16_to_cpu(descs[i].nlb) + 1;

		if (!__mem_is_zero(descs[i].rsvd0, sizeof(descs[i].rsvd0)) ||
		    !__mem_is_zero(descs[i].rsvd18, sizeof(descs[i].rsvd18)) ||
		    descs[i].eilbrt || descs[i].elbat || descs[i].elbatm) {
			ctx->status = NVME_SC_INVALID_FIELD;
			return;
		}

		if (nlb > NVMEV_COPY_MSSRL || total + nlb > NVMEV_COPY_MCL) {
			ctx->status = NVME_SC_CMD_SIZE_LIM_EXCEEDED;
			return;
		}

		if (slba >= max_lbas || nlb > max_lbas - slba) {
			ctx->status = NVME_SC_LBA_RANGE;
			return;
		}

		ranges[i].slba = slba;
		ranges[i].nlb = nlb;
		total += nlb;
	}

	if (ctx->sdlba >= max_lbas || total > max_lbas - ctx->sdlba) {
		ctx->status = NVME_SC_LBA_RANGE;
		return;
	}

	for (i = 0; i < ctx->nr_ranges; i++) {
		u64 src_start = ranges[i].slba;
		u64 src_end = src_start + ranges[i].nlb;
		u64 dst_start = ctx->sdlba;
		u64 dst_end = dst_start + total;

		if (src_start < dst_end && dst_start < src_end) {
			ctx->status = NVME_SC_OVERLAPPING_RANGE;
			return;
		}
	}

	ctx->total_lbas = total;
	ctx->status = NVME_SC_SUCCESS;
}

int nvmev_copy_load_ranges(struct nvmev_ns *ns, struct nvme_copy_command *cmd,
			   struct nvmev_copy_ctx *ctx, struct nvmev_copy_range **ranges)
{
	struct nvme_copy_range *descs = NULL;
	int ret;

	*ranges = NULL;
	__copy_prepare(ns, cmd, ctx);
	if (ctx->status != NVME_SC_SUCCESS)
		return 0;

	if (__copy_consume_prp_fault_once()) {
		ctx->status = NVME_SC_DATA_XFER_ERROR;
		return 0;
	}

	descs = kmalloc(ctx->desc_bytes, GFP_KERNEL);
	if (!descs)
		return -ENOMEM;

	*ranges = kcalloc(ctx->nr_ranges, sizeof(**ranges), GFP_KERNEL);
	if (!*ranges) {
		kfree(descs);
		return -ENOMEM;
	}

	ret = __copy_from_prp(le64_to_cpu(cmd->prp1), le64_to_cpu(cmd->prp2), descs,
			      ctx->desc_bytes);
	if (ret) {
		kfree(descs);
		kfree(*ranges);
		*ranges = NULL;
		ctx->status = NVME_SC_DATA_XFER_ERROR;
		return 0;
	}

	__copy_parse_ranges(ns, descs, ctx, *ranges);
	kfree(descs);

	if (ctx->status != NVME_SC_SUCCESS) {
		kfree(*ranges);
		*ranges = NULL;
	}

	return 0;
}

void nvmev_copy_free_ranges(struct nvmev_copy_range *ranges)
{
	kfree(ranges);
}
