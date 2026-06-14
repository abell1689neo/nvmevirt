// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "ssd.h"
#include "zns_ftl.h"

struct zns_prp_mapping {
	void *base;
	void *addr;
	bool memremap;
};

#define NVMEV_ZNS_PRPS_PER_PAGE (PAGE_SIZE / sizeof(__le64))

static bool __zns_map_prp_page(u64 paddr, struct zns_prp_mapping *map)
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

	map->addr = (u8 *)map->base + offset;
	return true;
}

static void __zns_unmap_prp_page(struct zns_prp_mapping *map)
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

static int __zns_copy_to_prp_data_page(u64 paddr, const void *src, size_t len,
				       size_t *copied, bool allow_offset)
{
	struct zns_prp_mapping map;
	size_t page_offs = paddr & PAGE_OFFSET_MASK;
	size_t copy_len;

	if (*copied >= len)
		return 0;
	if (!paddr)
		return -EFAULT;
	if (!allow_offset && page_offs)
		return -EFAULT;

	if (!__zns_map_prp_page(paddr, &map))
		return -EFAULT;

	copy_len = min_t(size_t, len - *copied, PAGE_SIZE - page_offs);
	memcpy(map.addr, (const u8 *)src + *copied, copy_len);
	__zns_unmap_prp_page(&map);
	*copied += copy_len;
	return 0;
}

static int __zns_copy_to_prp_list(u64 list_paddr, const void *src, size_t len,
				  size_t *copied)
{
	u32 guard = 0;
	u32 max_lists = DIV_ROUND_UP(DIV_ROUND_UP(len, PAGE_SIZE),
				     NVMEV_ZNS_PRPS_PER_PAGE - 1) + 1;

	if (!list_paddr || (list_paddr & PAGE_OFFSET_MASK))
		return -EFAULT;

	while (*copied < len) {
		struct zns_prp_mapping map;
		__le64 *entries;
		u64 next_list = 0;
		u32 i;
		int ret = 0;

		if (++guard > max_lists)
			return -EFAULT;

		entries = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!entries)
			return -ENOMEM;

		if (!__zns_map_prp_page(list_paddr, &map)) {
			kfree(entries);
			return -EFAULT;
		}

		memcpy(entries, map.addr, PAGE_SIZE);
		__zns_unmap_prp_page(&map);

		for (i = 0; i < NVMEV_ZNS_PRPS_PER_PAGE && *copied < len; i++) {
			u64 paddr = le64_to_cpu(entries[i]);

			if (!paddr) {
				ret = -EFAULT;
				break;
			}

			if (i == NVMEV_ZNS_PRPS_PER_PAGE - 1 &&
			    *copied + PAGE_SIZE < len) {
				next_list = paddr;
				break;
			}

			ret = __zns_copy_to_prp_data_page(paddr, src, len,
							 copied, false);
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

static int __zns_copy_to_prp(u64 prp1, u64 prp2, const void *src, size_t len)
{
	size_t copied = 0;
	int ret;

	if (!len)
		return 0;

	ret = __zns_copy_to_prp_data_page(prp1, src, len, &copied, true);
	if (ret || copied == len)
		return ret;

	if (len - copied <= PAGE_SIZE)
		return __zns_copy_to_prp_data_page(prp2, src, len, &copied,
						   false);

	return __zns_copy_to_prp_list(prp2, src, len, &copied);
}

static size_t __zone_report_capacity(struct zns_ftl *zns_ftl)
{
	size_t header = offsetof(struct zone_report, zd);

	return header + sizeof(struct zone_descriptor) * zns_ftl->zp.nr_zones;
}

static void __fill_zone_report(struct zns_ftl *zns_ftl, struct nvme_zone_mgmt_recv *cmd,
			       struct zone_report *report, uint64_t slba,
			       size_t bytes_transfer)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	uint64_t start_zid = lba_to_zone(zns_ftl, slba);
	size_t header = offsetof(struct zone_report, zd);
	uint64_t nr_zone_to_report;

	if (cmd->zra_specific_features == 0) // all
		nr_zone_to_report = zns_ftl->zp.nr_zones - start_zid;
	else if (bytes_transfer <= header) // partial, header only
		nr_zone_to_report = 0;
	else // partial. # of zone desc transferred
		nr_zone_to_report =
			(bytes_transfer - header) / sizeof(struct zone_descriptor);

	if (nr_zone_to_report > zns_ftl->zp.nr_zones - start_zid) {
		// Userspace asked for zones exceeding the device limit, adjust nr_zones
		nr_zone_to_report = zns_ftl->zp.nr_zones - start_zid;
	}

	report->nr_zones = nr_zone_to_report;

	memcpy(report->zd, &(zone_descs[start_zid]),
	       sizeof(struct zone_descriptor) * nr_zone_to_report);
}

static bool __check_zmgmt_rcv_option_supported(struct zns_ftl *zns_ftl,
					       struct nvme_zone_mgmt_recv *cmd,
					       uint64_t slba, uint32_t *status)
{
	*status = NVME_SC_INVALID_FIELD;

	if (lba_to_zone(zns_ftl, slba) >= zns_ftl->zp.nr_zones) {
		NVMEV_ERROR("Invalid lba range\n");
		*status = NVME_SC_LBA_RANGE;
		return false;
	}

	if (cmd->zra != 0) {
		NVMEV_ERROR("Currently, Not support Extended Report Zones\n");
		return false;
	}

	if (cmd->zra_specific_field != 0) {
		NVMEV_ERROR("Currently, Only support listing all zone\n");
		return false;
	}

	return true;
}

void zns_zmgmt_recv(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;
	struct zone_report *buffer = zns_ftl->report_buffer;
	struct nvme_zone_mgmt_recv *cmd = (struct nvme_zone_mgmt_recv *)req->cmd;

	uint64_t prp1 = le64_to_cpu(cmd->prp1);
	uint64_t prp2 = le64_to_cpu(cmd->prp2);
	uint64_t slba = le64_to_cpu(cmd->slba);
	uint64_t length = ((uint64_t)le32_to_cpu(cmd->nr_dw) + 1) * sizeof(uint32_t);
	size_t report_capacity = __zone_report_capacity(zns_ftl);
	size_t transfer_len = min_t(uint64_t, length, report_capacity);
	uint32_t status = NVME_SC_SUCCESS;

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_dw 0x%llx  action %u partial %u action_specific 0x%x\n",
			__func__, slba, length, cmd->zra, cmd->zra_specific_features,
			cmd->zra_specific_field);

	if (__check_zmgmt_rcv_option_supported(zns_ftl, cmd, slba, &status)) {
		memset(buffer, 0x00, report_capacity);
		__fill_zone_report(zns_ftl, cmd, buffer, slba, transfer_len);

		if (__zns_copy_to_prp(prp1, prp2, buffer, transfer_len))
			status = NVME_SC_DATA_XFER_ERROR;
	}

	ret->nsecs_target = req->nsecs_start; // no delay
	ret->status = status;
	ret->bytes = status == NVME_SC_SUCCESS ? transfer_len : 0;
	return;
}
