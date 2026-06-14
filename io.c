// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>

#include "nvmev.h"
#include "dma.h"
#include "copy.h"

#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
#include "ssd.h"
#else
struct buffer;
#endif

#undef PERF_DEBUG

#define sq_entry(entry_id) sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

extern bool io_using_dma;

static inline unsigned int __get_io_worker(int sqid)
{
#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
	return (sqid - 1) % nvmev_vdev->config.nr_io_workers;
#else
	return nvmev_vdev->io_worker_turn;
#endif
}

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(nvmev_vdev->config.cpu_nr_dispatcher);
}

static void __atomic64_update_max(atomic64_t *max, u64 value)
{
	s64 old = atomic64_read(max);

	while (value > old) {
		s64 prev = atomic64_cmpxchg(max, old, value);

		if (prev == old)
			break;
		old = prev;
	}
}

static void __atomic64_dec_if_positive(atomic64_t *value)
{
	s64 old = atomic64_read(value);

	while (old > 0) {
		s64 prev = atomic64_cmpxchg(value, old, old - 1);

		if (prev == old)
			break;
		old = prev;
	}
}

static void __record_latency_enqueue(struct nvmev_io_work *w)
{
	struct nvmev_opcode_latency_stat *s;
	s64 inflight;

	if (w->is_internal)
		return;

	s = &nvmev_vdev->opcode_latency[w->opcode];
	inflight = atomic64_inc_return(&s->inflight);
	__atomic64_update_max(&s->max_inflight, inflight);
}

static void __record_io_stats(const struct nvme_command *cmd, struct nvmev_result *ret,
			      size_t io_size)
{
	u8 opcode = cmd->common.opcode;
	u32 copy_status = ret->status & ~NVME_SC_DNR;

	atomic64_inc(&nvmev_vdev->opcode_cmds[opcode]);
	atomic64_add(io_size, &nvmev_vdev->opcode_bytes[opcode]);

	if (opcode != nvme_cmd_copy)
		return;

	atomic64_inc(&nvmev_vdev->copy_stat.submitted);
	if (copy_status < NVMEV_COPY_STATUS_BUCKETS)
		atomic64_inc(&nvmev_vdev->copy_stat.status[copy_status]);
	else
		atomic64_inc(&nvmev_vdev->copy_stat.status_overflow);
	if (ret->status != NVME_SC_SUCCESS) {
		atomic64_inc(&nvmev_vdev->copy_stat.failed);
		return;
	}

	atomic64_inc(&nvmev_vdev->copy_stat.succeeded);
	atomic64_add(ret->bytes, &nvmev_vdev->copy_stat.logical_bytes);
	atomic64_add(ret->copy_desc_bytes, &nvmev_vdev->copy_stat.descriptor_bytes);
	atomic64_add(ret->copy_source_read_bytes,
		     &nvmev_vdev->copy_stat.source_read_bytes);
	atomic64_add(ret->copy_destination_write_bytes,
		     &nvmev_vdev->copy_stat.destination_write_bytes);
	atomic64_add(ret->copy_host_payload_avoided_bytes,
	     &nvmev_vdev->copy_stat.host_payload_avoided_bytes);
}

static void __record_latency_stats(struct nvmev_io_work *w, u64 completed_nsecs)
{
	struct nvmev_opcode_latency_stat *s;
	u64 wall_completion_ns = 0;
	u64 wall_queue_ns = 0;
	u64 wall_payload_ns = 0;
	u64 model_ns = 0;
	u64 wall_after_model_ns = 0;

	if (w->is_internal)
		return;

	if (completed_nsecs > w->nsecs_start)
		wall_completion_ns = completed_nsecs - w->nsecs_start;
	if (w->nsecs_copy_start > w->nsecs_start)
		wall_queue_ns = w->nsecs_copy_start - w->nsecs_start;
	if (w->nsecs_copy_done > w->nsecs_copy_start)
		wall_payload_ns = w->nsecs_copy_done - w->nsecs_copy_start;
	if (w->nsecs_target > w->nsecs_start)
		model_ns = w->nsecs_target - w->nsecs_start;
	if (completed_nsecs > w->nsecs_target)
		wall_after_model_ns = completed_nsecs - w->nsecs_target;

	s = &nvmev_vdev->opcode_latency[w->opcode];
	atomic64_inc(&s->completions);
	atomic64_add(wall_completion_ns, &s->wall_completion_ns);
	atomic64_add(wall_queue_ns, &s->wall_queue_ns);
	atomic64_add(wall_payload_ns, &s->wall_payload_ns);
	atomic64_add(model_ns, &s->model_ns);
	atomic64_add(wall_after_model_ns, &s->wall_after_model_ns);
	__atomic64_update_max(&s->max_wall_completion_ns, wall_completion_ns);
	__atomic64_update_max(&s->max_wall_queue_ns, wall_queue_ns);
	__atomic64_update_max(&s->max_wall_payload_ns, wall_payload_ns);
	__atomic64_update_max(&s->max_model_ns, model_ns);
	__atomic64_update_max(&s->max_wall_after_model_ns,
			      wall_after_model_ns);
	__atomic64_dec_if_positive(&s->inflight);
}

static inline size_t __cmd_io_offset(const struct nvme_rw_command *cmd)
{
	return le64_to_cpu(cmd->slba) << LBA_BITS;
}

static inline size_t __cmd_io_size(const struct nvme_rw_command *cmd)
{
	return (le16_to_cpu(cmd->length) + 1) << LBA_BITS;
}

#define NVMEV_IO_MAX_PRPS 512

static unsigned int __do_perform_io(struct nvmev_io_work *w)
{
	const struct nvme_rw_command *cmd = &w->command.rw;
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	u64 paddr;
	u64 *paddr_list = NULL;
	void *paddr_list_base = NULL;
	uint32_t nsid_raw = le32_to_cpu(cmd->nsid);
	size_t nsid;
	bool paddr_list_memremap = false;
	unsigned int ret = 0;

	if (nsid_raw == 0 || nsid_raw > nvmev_vdev->nr_ns)
		return 0;
	nsid = nsid_raw - 1; // 0-based

	mutex_lock(&nvmev_vdev->ns[nsid].storage_lock);
	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;

	while (remaining) {
		u64 data_base;
		size_t io_size;
		void *vaddr;
		void *vaddr_base;
		size_t mem_offs = 0;
		bool vaddr_memremap = false;

		prp_offs++;
		if (prp_offs > NVMEV_IO_MAX_PRPS)
			goto out_unlock;

		if (prp_offs == 1) {
			paddr = le64_to_cpu(cmd->prp1);
		} else if (prp_offs == 2) {
			paddr = le64_to_cpu(cmd->prp2);
			if (remaining > PAGE_SIZE) {
				u64 list_base = paddr & PAGE_MASK;
				size_t list_offs = paddr & PAGE_OFFSET_MASK;

				if (!paddr)
					goto out_unlock;
				if (pfn_valid(list_base >> PAGE_SHIFT)) {
					paddr_list_base = kmap_atomic_pfn(PRP_PFN(list_base));
				} else {
					paddr_list_base = memremap(list_base, PAGE_SIZE,
								   MEMREMAP_WT);
					paddr_list_memremap = true;
				}
				if (!paddr_list_base)
					goto out_unlock;
				paddr_list = (u64 *)((u8 *)paddr_list_base +
						     list_offs);
				paddr = paddr_list[prp2_offs++];
			}
		} else {
			if (!paddr_list)
				goto out_unlock;
			paddr = paddr_list[prp2_offs++];
		}
		if (!paddr)
			goto out_unlock;

		data_base = paddr & PAGE_MASK;
		mem_offs = paddr & PAGE_OFFSET_MASK;
		if (pfn_valid(data_base >> PAGE_SHIFT)) {
			vaddr_base = kmap_atomic_pfn(PRP_PFN(data_base));
		} else {
			vaddr_base = memremap(data_base, PAGE_SIZE, MEMREMAP_WT);
			vaddr_memremap = true;
		}
		if (!vaddr_base)
			goto out_unlock;
		vaddr = vaddr_base + mem_offs;

		io_size = min_t(size_t, remaining, PAGE_SIZE - mem_offs);

		if (cmd->opcode == nvme_cmd_write ||
		    cmd->opcode == nvme_cmd_zone_append) {
			memcpy(nvmev_vdev->ns[nsid].mapped + offset, vaddr, io_size);
		} else if (cmd->opcode == nvme_cmd_read) {
			memcpy(vaddr, nvmev_vdev->ns[nsid].mapped + offset, io_size);
		}

		if (!vaddr_memremap) {
			kunmap_atomic(vaddr_base);
		} else {
			memunmap(vaddr_base);
			vaddr_memremap = false;
		}

		remaining -= io_size;
		offset += io_size;
	}

	ret = length;

out_unlock:
	if (paddr_list_base) {
		if (!paddr_list_memremap)
			kunmap_atomic(paddr_list_base);
		else
			memunmap(paddr_list_base);
	}
	mutex_unlock(&nvmev_vdev->ns[nsid].storage_lock);
	return ret;
}

#define NVMEV_DMA_MAX_PRPS NVMEV_IO_MAX_PRPS

static unsigned int __do_perform_io_using_dma(struct nvmev_io_work *w)
{
	const struct nvme_rw_command *cmd = &w->command.rw;
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	int num_prps = 0;
	u64 paddr;
	u64 *paddr_list;
	u64 *tmp_paddr_list = NULL;
	void *tmp_paddr_base = NULL;
	size_t io_size;
	size_t mem_offs = 0;
	bool tmp_paddr_memremap = false;
	uint32_t nsid_raw = le32_to_cpu(cmd->nsid);
	size_t nsid;
	unsigned int ret = 0;

	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;
	if (nsid_raw == 0 || nsid_raw > nvmev_vdev->nr_ns)
		return 0;
	nsid = nsid_raw - 1;

	/* Index 0 is intentionally unused so index == PRP number. */
	paddr_list = kcalloc(NVMEV_DMA_MAX_PRPS + 1, sizeof(*paddr_list),
			     GFP_KERNEL);
	if (!paddr_list)
		return 0;

	mutex_lock(&nvmev_vdev->ns[nsid].storage_lock);
	/* Loop to get the PRP list */
	while (remaining) {
		io_size = 0;
		mem_offs = 0;

		prp_offs++;
		if (prp_offs > NVMEV_DMA_MAX_PRPS)
			goto out_unlock;

		if (prp_offs == 1) {
			paddr_list[prp_offs] = le64_to_cpu(cmd->prp1);
		} else if (prp_offs == 2) {
			paddr_list[prp_offs] = le64_to_cpu(cmd->prp2);
			if (remaining > PAGE_SIZE) {
				u64 list_paddr = paddr_list[prp_offs];
				u64 list_base = list_paddr & PAGE_MASK;
				size_t list_offs = list_paddr & PAGE_OFFSET_MASK;

				if (!list_paddr)
					goto out_unlock;
				if (pfn_valid(list_base >> PAGE_SHIFT)) {
					tmp_paddr_base = kmap_atomic_pfn(PRP_PFN(list_base));
				} else {
					tmp_paddr_base = memremap(list_base, PAGE_SIZE,
								  MEMREMAP_WT);
					tmp_paddr_memremap = true;
				}
				if (!tmp_paddr_base)
					goto out_unlock;
				tmp_paddr_list = (u64 *)((u8 *)tmp_paddr_base +
							 list_offs);
				paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
			}
		} else {
			if (!tmp_paddr_list)
				goto out_unlock;
			paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
		}
		if (!paddr_list[prp_offs])
			goto out_unlock;

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr_list[prp_offs] & PAGE_OFFSET_MASK) {
			mem_offs = paddr_list[prp_offs] & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		remaining -= io_size;
	}
	num_prps = prp_offs;

	if (tmp_paddr_base != NULL && !tmp_paddr_memremap) {
		kunmap_atomic(tmp_paddr_base);
		tmp_paddr_base = NULL;
	} else if (tmp_paddr_base != NULL && tmp_paddr_memremap) {
		memunmap(tmp_paddr_base);
		tmp_paddr_base = NULL;
		tmp_paddr_memremap = false;
	}

	remaining = length;
	prp_offs = 1;

	/* Loop for data transfer */
	while (remaining) {
		size_t page_size;
		mem_offs = 0;
		io_size = 0;
		page_size = 0;

		paddr = paddr_list[prp_offs];
		if (!paddr)
			goto out_unlock;
		page_size = min_t(size_t, remaining, PAGE_SIZE);

		/* For non-page aligned paddr, it will never be between continuous PRP list (Always first paddr)  */
		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (page_size + mem_offs > PAGE_SIZE) {
				page_size = PAGE_SIZE - mem_offs;
			}
		}

		for (prp_offs++; prp_offs <= num_prps; prp_offs++) {
			if (paddr_list[prp_offs] == paddr_list[prp_offs - 1] + PAGE_SIZE)
				page_size += PAGE_SIZE;
			else
				break;
		}

		io_size = min_t(size_t, remaining, page_size);

		if (cmd->opcode == nvme_cmd_write ||
		    cmd->opcode == nvme_cmd_zone_append) {
			ioat_dma_submit(paddr, nvmev_vdev->config.storage_start + offset, io_size);
		} else if (cmd->opcode == nvme_cmd_read) {
			ioat_dma_submit(nvmev_vdev->config.storage_start + offset, paddr, io_size);
		}

		remaining -= io_size;
		offset += io_size;
	}
	ret = length;

out_unlock:
	if (tmp_paddr_base != NULL && !tmp_paddr_memremap)
		kunmap_atomic(tmp_paddr_base);
	else if (tmp_paddr_base != NULL && tmp_paddr_memremap)
		memunmap(tmp_paddr_base);
	mutex_unlock(&nvmev_vdev->ns[nsid].storage_lock);
	kfree(paddr_list);
	return ret;
}

static uint64_t __do_perform_copy(struct nvmev_io_work *w)
{
	struct nvmev_ns *ns;
	uint64_t dst_lba = w->copy_sdlba;
	uint64_t copied = 0;
	u32 i;

	if (!w->copy_ranges)
		return 0;
	if (w->nsid == 0 || w->nsid > nvmev_vdev->nr_ns) {
		nvmev_copy_free_ranges(w->copy_ranges);
		w->copy_ranges = NULL;
		w->copy_nr_ranges = 0;
		return 0;
	}

	ns = &nvmev_vdev->ns[w->nsid - 1];
	mutex_lock(&ns->storage_lock);
	for (i = 0; i < w->copy_nr_ranges; i++) {
		size_t bytes = LBA_TO_BYTE(w->copy_ranges[i].nlb);

		memcpy(ns->mapped + LBA_TO_BYTE(dst_lba),
		       ns->mapped + LBA_TO_BYTE(w->copy_ranges[i].slba), bytes);
		dst_lba += w->copy_ranges[i].nlb;
		copied += bytes;
	}
	mutex_unlock(&ns->storage_lock);

	nvmev_copy_free_ranges(w->copy_ranges);
	w->copy_ranges = NULL;
	w->copy_nr_ranges = 0;
	atomic64_add(copied, &nvmev_vdev->copy_stat.backing_memcpy_bytes);
	return copied;
}

static void __insert_req_sorted(unsigned int entry, struct nvmev_io_worker *worker,
				unsigned long nsecs_target)
{
	/**
	 * Requests are placed in @work_queue sorted by their target time.
	 * @work_queue is statically allocated and the ordered list is
	 * implemented by chaining the indexes of entries with @prev and @next.
	 * This implementation is nasty but we do this way over dynamically
	 * allocated linked list to minimize the influence of dynamic memory allocation.
	 * Also, this O(n) implementation can be improved to O(logn) scheme with
	 * e.g., red-black tree but....
	 */
	if (worker->io_seq == -1) {
		worker->io_seq = entry;
		worker->io_seq_end = entry;
	} else {
		unsigned int curr = worker->io_seq_end;

		while (curr != -1) {
			if (worker->work_queue[curr].nsecs_target <= worker->latest_nsecs)
				break;

			if (worker->work_queue[curr].nsecs_target <= nsecs_target)
				break;

			curr = worker->work_queue[curr].prev;
		}

		if (curr == -1) { /* Head inserted */
			worker->work_queue[worker->io_seq].prev = entry;
			worker->work_queue[entry].next = worker->io_seq;
			worker->io_seq = entry;
		} else if (worker->work_queue[curr].next == -1) { /* Tail */
			worker->work_queue[entry].prev = curr;
			worker->io_seq_end = entry;
			worker->work_queue[curr].next = entry;
		} else { /* In between */
			worker->work_queue[entry].prev = curr;
			worker->work_queue[entry].next = worker->work_queue[curr].next;

			worker->work_queue[worker->work_queue[entry].next].prev = entry;
			worker->work_queue[curr].next = entry;
		}
	}
}

static struct nvmev_io_worker *__allocate_work_queue_entry(int sqid, unsigned int *entry)
{
	unsigned int io_worker_turn = __get_io_worker(sqid);
	struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[io_worker_turn];
	unsigned int e = worker->free_seq;
	struct nvmev_io_work *w;

	if (e == -1 || e >= NR_MAX_PARALLEL_IO) {
		WARN_ON_ONCE(1);
		return NULL;
	}
	w = worker->work_queue + e;

	if (++io_worker_turn == nvmev_vdev->config.nr_io_workers)
		io_worker_turn = 0;
	nvmev_vdev->io_worker_turn = io_worker_turn;

	worker->free_seq = w->next;
	if (worker->free_seq == -1)
		worker->free_seq_end = -1;
	else
		BUG_ON(worker->free_seq >= NR_MAX_PARALLEL_IO);
	*entry = e;

	return worker;
}

static bool __enqueue_io_req(int sqid, int cqid, int sq_entry,
			     const struct nvme_command *cmd_snapshot,
			     unsigned long long nsecs_start,
			     struct nvmev_result *ret)
{
	struct nvmev_io_worker *worker;
	struct nvmev_io_work *w;
	unsigned int entry;

	worker = __allocate_work_queue_entry(sqid, &entry);
	if (!worker)
		return false;

	w = worker->work_queue + entry;

	NVMEV_DEBUG_VERBOSE("%s/%u[%d], sq %d cq %d, entry %d, %llu + %llu\n", worker->thread_name, entry,
			    cmd_snapshot->rw.opcode, sqid, cqid, sq_entry, nsecs_start,
			    ret->nsecs_target - nsecs_start);

	/////////////////////////////////
	w->sqid = sqid;
	w->cqid = cqid;
	w->sq_entry = sq_entry;
	w->command = *cmd_snapshot;
	w->command_id = cmd_snapshot->common.command_id;
	w->opcode = cmd_snapshot->common.opcode;
	w->nsid = le32_to_cpu(cmd_snapshot->common.nsid);
	w->nsecs_start = nsecs_start;
	w->nsecs_enqueue = local_clock();
	w->nsecs_copy_start = 0;
	w->nsecs_copy_done = 0;
	w->nsecs_cq_filled = 0;
	w->nsecs_target = ret->nsecs_target;
	w->status = ret->status;
	w->result0 = (unsigned int)(ret->result & 0xFFFFFFFF);
	w->result1 = (unsigned int)(ret->result >> 32);
	w->copy_sdlba = ret->copy_sdlba;
	w->copy_nr_ranges = ret->copy_nr_ranges;
	w->copy_expected_bytes = ret->bytes;
	w->copy_ranges = ret->copy_ranges;
	w->is_completed = false;
	w->is_copied = false;
	w->prev = -1;
	w->next = -1;

	w->is_internal = false;
	mb(); /* IO worker shall see the updated w at once */

	__record_latency_enqueue(w);
	__insert_req_sorted(entry, worker, ret->nsecs_target);
	return true;
}

void schedule_internal_operation(int sqid, unsigned long long nsecs_target,
				 struct buffer *write_buffer, size_t buffs_to_release)
{
	struct nvmev_io_worker *worker;
	struct nvmev_io_work *w;
	unsigned int entry;

	worker = __allocate_work_queue_entry(sqid, &entry);
	if (!worker) {
#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
		if (write_buffer && buffs_to_release)
			buffer_release(write_buffer, buffs_to_release);
#endif
		return;
	}

	w = worker->work_queue + entry;

	NVMEV_DEBUG_VERBOSE("%s/%u, internal sq %d, %llu + %llu\n", worker->thread_name, entry, sqid,
		    local_clock(), nsecs_target - local_clock());

	/////////////////////////////////
	w->sqid = sqid;
	w->opcode = 0;
	w->nsid = 0;
	memset(&w->command, 0, sizeof(w->command));
	w->nsecs_start = w->nsecs_enqueue = local_clock();
	w->nsecs_copy_start = 0;
	w->nsecs_copy_done = 0;
	w->nsecs_cq_filled = 0;
	w->nsecs_target = nsecs_target;
	w->is_completed = false;
	w->is_copied = true;
	w->prev = -1;
	w->next = -1;

	w->is_internal = true;
	w->write_buffer = write_buffer;
	w->buffs_to_release = buffs_to_release;
	w->copy_sdlba = 0;
	w->copy_nr_ranges = 0;
	w->copy_expected_bytes = 0;
	w->copy_ranges = NULL;
	mb(); /* IO worker shall see the updated w at once */

	__insert_req_sorted(entry, worker, nsecs_target);
}

static void __reclaim_completed_reqs(void)
{
	unsigned int turn;

	for (turn = 0; turn < nvmev_vdev->config.nr_io_workers; turn++) {
		struct nvmev_io_worker *worker;
		struct nvmev_io_work *w;

		unsigned int first_entry = -1;
		unsigned int last_entry = -1;
		unsigned int curr;
		int nr_reclaimed = 0;

		worker = &nvmev_vdev->io_workers[turn];

		first_entry = worker->io_seq;
		curr = first_entry;

		while (curr != -1) {
			w = &worker->work_queue[curr];
			if (w->is_completed == true && w->is_copied == true &&
			    (w->nsecs_target <= worker->latest_nsecs ||
			     atomic_read(&nvmev_vdev->quiescing))) {
				last_entry = curr;
				curr = w->next;
				nr_reclaimed++;
			} else {
				break;
			}
		}

		if (last_entry != -1) {
			w = &worker->work_queue[last_entry];
			worker->io_seq = w->next;
			if (w->next != -1) {
				worker->work_queue[w->next].prev = -1;
			}
			w->next = -1;

			w = &worker->work_queue[first_entry];
			if (worker->free_seq == -1) {
				w->prev = -1;
				worker->free_seq = first_entry;
			} else {
				w->prev = worker->free_seq_end;
				w = &worker->work_queue[worker->free_seq_end];
				w->next = first_entry;
			}

			worker->free_seq_end = last_entry;
			NVMEV_DEBUG_VERBOSE("%s: %u -- %u, %d\n", __func__,
					first_entry, last_entry, nr_reclaimed);
		}
	}
}

static bool __io_workers_idle(struct nvmev_dev *dev)
{
	unsigned int turn;

	if (!dev || !dev->io_workers)
		return true;

	for (turn = 0; turn < dev->config.nr_io_workers; turn++) {
		struct nvmev_io_worker *worker = &dev->io_workers[turn];

		if (worker->io_seq != -1)
			return false;
	}

	return true;
}

static bool __io_submission_queues_idle(struct nvmev_dev *dev)
{
	unsigned int qid;

	if (!dev)
		return true;

	for (qid = 1; qid <= dev->nr_sq; qid++) {
		struct nvmev_submission_queue *sq = dev->sqes[qid];

		if (sq && sq->stat.nr_in_flight)
			return false;
	}

	return true;
}

bool nvmev_io_drain(struct nvmev_dev *dev, unsigned long timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	do {
		__reclaim_completed_reqs();
		if (__io_workers_idle(dev) && __io_submission_queues_idle(dev))
			return true;
		msleep(20);
	} while (time_before(jiffies, deadline));

	__reclaim_completed_reqs();
	return __io_workers_idle(dev) && __io_submission_queues_idle(dev);
}

static size_t __nvmev_proc_io(int sqid, int sq_entry, size_t *io_size)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	unsigned long long nsecs_start = __get_wallclock();
	struct nvme_command cmd_snapshot;
	struct nvme_command *cmd = &cmd_snapshot;
	struct nvmev_ns *ns;
#if (BASE_SSD == KV_PROTOTYPE)
	uint32_t nsid = 0; // Some KVSSD programs give 0 as nsid for KV IO
#else
	uint32_t nsid_raw;
	uint32_t nsid;
#endif
	struct nvmev_request req = {
		.cmd = cmd,
		.sq_id = sqid,
		.nsecs_start = nsecs_start,
	};
	struct nvmev_result ret = {
		.nsecs_target = nsecs_start,
		.status = NVME_SC_SUCCESS,
		.bytes = 0,
		.copy_sdlba = 0,
		.copy_nr_ranges = 0,
		.copy_desc_bytes = 0,
		.copy_source_read_bytes = 0,
		.copy_destination_write_bytes = 0,
		.copy_host_payload_avoided_bytes = 0,
		.copy_ranges = NULL,
	};

#ifdef PERF_DEBUG
	unsigned long long prev_clock = local_clock();
	unsigned long long prev_clock2 = 0;
	unsigned long long prev_clock3 = 0;
	unsigned long long prev_clock4 = 0;
	static unsigned long long clock1 = 0;
	static unsigned long long clock2 = 0;
	static unsigned long long clock3 = 0;
	static unsigned long long counter = 0;
#endif

	memcpy_fromio(&cmd_snapshot, &sq_entry(sq_entry), sizeof(cmd_snapshot));

#if (BASE_SSD != KV_PROTOTYPE)
	nsid_raw = le32_to_cpu(cmd->common.nsid);
	if (unlikely(nsid_raw == 0 || nsid_raw > nvmev_vdev->nr_ns)) {
		ret.status = NVME_SC_INVALID_NS;
		*io_size = 0;
		__record_io_stats(cmd, &ret, *io_size);
		if (!__enqueue_io_req(sqid, sq->cqid, sq_entry, cmd, nsecs_start,
				      &ret))
			return false;
		__reclaim_completed_reqs();
		return true;
	}
	nsid = nsid_raw - 1;
#endif
	ns = &nvmev_vdev->ns[nsid];

	if (unlikely(atomic_read(&nvmev_vdev->quiescing))) {
		ret.status = NVME_SC_ABORT_REQ;
		ret.bytes = 0;
		ret.nsecs_target = nsecs_start;
		*io_size = 0;
		__record_io_stats(cmd, &ret, *io_size);
		if (!__enqueue_io_req(sqid, sq->cqid, sq_entry, cmd, nsecs_start,
				      &ret))
			return false;
		__reclaim_completed_reqs();
		return true;
	}

	if (!ns->proc_io_cmd(ns, &req, &ret)) {
		ret.status = NVME_SC_INTERNAL;
		ret.bytes = 0;
		ret.nsecs_target = nsecs_start;
	}
	if (ret.status != NVME_SC_SUCCESS) {
		*io_size = 0;
	} else if (ret.bytes) {
		*io_size = ret.bytes;
	} else {
		switch (cmd->common.opcode) {
		case nvme_cmd_read:
		case nvme_cmd_write:
		case nvme_cmd_zone_append:
			*io_size = __cmd_io_size(&cmd->rw);
			break;
		default:
			*io_size = 0;
			break;
		}
	}

	__record_io_stats(cmd, &ret, *io_size);

#ifdef PERF_DEBUG
	prev_clock2 = local_clock();
#endif

	if (!__enqueue_io_req(sqid, sq->cqid, sq_entry, cmd, nsecs_start, &ret)) {
		if (ret.copy_ranges)
			nvmev_copy_free_ranges(ret.copy_ranges);
		return false;
	}

#ifdef PERF_DEBUG
	prev_clock3 = local_clock();
#endif

	__reclaim_completed_reqs();

#ifdef PERF_DEBUG
	prev_clock4 = local_clock();

	clock1 += (prev_clock2 - prev_clock);
	clock2 += (prev_clock3 - prev_clock2);
	clock3 += (prev_clock4 - prev_clock3);
	counter++;

	if (counter > 1000) {
		NVMEV_DEBUG("LAT: %llu, ENQ: %llu, CLN: %llu\n", clock1 / counter, clock2 / counter,
			    clock3 / counter);
		clock1 = 0;
		clock2 = 0;
		clock3 = 0;
		counter = 0;
	}
#endif
	return true;
}

int nvmev_proc_io_sq(int sqid, int new_db, int old_db)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;
	int latest_db;

	if (unlikely(!sq))
		return old_db;
	if (unlikely(num_proc < 0))
		num_proc += sq->queue_size;

	for (seq = 0; seq < num_proc; seq++) {
		size_t io_size;
		if (!__nvmev_proc_io(sqid, sq_entry, &io_size))
			break;

		if (++sq_entry == sq->queue_size) {
			sq_entry = 0;
		}
		sq->stat.nr_dispatched++;
		sq->stat.nr_in_flight++;
		sq->stat.total_io += io_size;
	}
	sq->stat.nr_dispatch++;
	sq->stat.max_nr_in_flight = max_t(int, sq->stat.max_nr_in_flight, sq->stat.nr_in_flight);

	latest_db = (old_db + seq) % sq->queue_size;
	return latest_db;
}

void nvmev_proc_io_cq(int cqid, int new_db, int old_db)
{
	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	int i;
	for (i = old_db; i != new_db; i++) {
		int sqid;
		struct nvmev_submission_queue *sq;

		if (i >= cq->queue_size) {
			i = -1;
			continue;
		}
		{
			struct nvme_completion cqe_snapshot;

			memcpy_fromio(&cqe_snapshot, &cq_entry(i),
				      sizeof(cqe_snapshot));
			sqid = le16_to_cpu(cqe_snapshot.sq_id);
		}
		if (sqid == 0 || sqid > nvmev_vdev->nr_sq) {
			NVMEV_ERROR("CQ %d completion with invalid SQ %d\n",
				    cqid, sqid);
			continue;
		}

		/* Should check the validity here since SPDK deletes SQ immediately
		 * before processing associated CQes */
		sq = nvmev_vdev->sqes[sqid];
		if (!sq)
			continue;

		if (sq->stat.nr_in_flight)
			sq->stat.nr_in_flight--;
		else
			NVMEV_ERROR("CQ %d completion with zero in-flight SQ %d\n",
				    cqid, sqid);
	}

	cq->cq_tail = new_db - 1;
	if (new_db == -1)
		cq->cq_tail = cq->queue_size - 1;
}

static void __fill_cq_result(struct nvmev_io_work *w)
{
	int sqid = w->sqid;
	int cqid = w->cqid;
	int sq_entry = w->sq_entry;
	unsigned int command_id = w->command_id;
	unsigned int status = w->status;
	unsigned int result0 = w->result0;
	unsigned int result1 = w->result1;

	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	struct nvme_completion cqe = { 0 };
	int cq_head;

	spin_lock(&cq->entry_lock);
	cq_head = cq->cq_head;

	cqe.command_id = command_id;
	cqe.sq_id = cpu_to_le16(sqid);
	cqe.sq_head = cpu_to_le16(sq_entry);
	cqe.status = cpu_to_le16(cq->phase | (status << 1));
	cqe.result0 = cpu_to_le32(result0);
	cqe.result1 = cpu_to_le32(result1);
	memcpy_toio(&cq_entry(cq_head), &cqe, sizeof(cqe));

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
	cq->interrupt_ready = true;
	spin_unlock(&cq->entry_lock);
}

static int nvmev_io_worker(void *data)
{
	struct nvmev_io_worker *worker = (struct nvmev_io_worker *)data;
	struct nvmev_ns *ns;
	static unsigned long last_io_time = 0;

#ifdef PERF_DEBUG
	static unsigned long long intr_clock[NR_MAX_IO_QUEUE + 1];
	static unsigned long long intr_counter[NR_MAX_IO_QUEUE + 1];

	unsigned long long prev_clock;
#endif

	NVMEV_INFO("%s started on cpu %d (node %d)\n", worker->thread_name, smp_processor_id(),
		   cpu_to_node(smp_processor_id()));

	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs_wall = __get_wallclock();
		unsigned long long curr_nsecs_local = local_clock();
		long long delta = curr_nsecs_wall - curr_nsecs_local;

		volatile unsigned int curr = worker->io_seq;
		int qidx;

		while (curr != -1) {
			struct nvmev_io_work *w = &worker->work_queue[curr];
			unsigned long long curr_nsecs = local_clock() + delta;
			worker->latest_nsecs = curr_nsecs;

			if (w->is_completed == true) {
				curr = w->next;
				continue;
			}

			if (w->is_copied == false) {
				w->nsecs_copy_start = curr_nsecs;
				if (w->is_internal) {
					;
				} else if (w->status != NVME_SC_SUCCESS) {
					if (w->copy_ranges) {
						nvmev_copy_free_ranges(w->copy_ranges);
						w->copy_ranges = NULL;
						w->copy_nr_ranges = 0;
					}
				} else if (w->opcode == nvme_cmd_copy) {
					if (__do_perform_copy(w) !=
					    w->copy_expected_bytes)
						w->status =
							NVME_SC_DATA_XFER_ERROR;
				} else if (io_using_dma) {
					if (!__do_perform_io_using_dma(w))
						w->status =
							NVME_SC_DATA_XFER_ERROR;
				} else {
#if (BASE_SSD == KV_PROTOTYPE)
					ns = &nvmev_vdev->ns[0];
					if (ns->identify_io_cmd(ns, w->command)) {
						w->result0 = ns->perform_io_cmd(
							ns, &w->command,
							&(w->status));
					} else {
						if (!__do_perform_io(w))
							w->status =
								NVME_SC_DATA_XFER_ERROR;
					}
#else
					if (!__do_perform_io(w))
						w->status =
							NVME_SC_DATA_XFER_ERROR;
#endif
				}

				w->nsecs_copy_done = local_clock() + delta;
				w->is_copied = true;
				last_io_time = jiffies;

				NVMEV_DEBUG_VERBOSE("%s: copied %u, %d %d %d\n", worker->thread_name, curr,
					    w->sqid, w->cqid, w->sq_entry);
			}

			if (w->nsecs_target <= curr_nsecs ||
			    atomic_read(&nvmev_vdev->quiescing)) {
				if (w->is_internal) {
#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
					buffer_release((struct buffer *)w->write_buffer,
						       w->buffs_to_release);
#endif
				} else {
					__record_latency_stats(w, curr_nsecs);
					__fill_cq_result(w);
				}

				NVMEV_DEBUG_VERBOSE("%s: completed %u, %d %d %d\n", worker->thread_name, curr,
					    w->sqid, w->cqid, w->sq_entry);

#ifdef PERF_DEBUG
				w->nsecs_cq_filled = local_clock() + delta;
				trace_printk("%llu %llu %llu %llu %llu %llu\n", w->nsecs_start,
					     w->nsecs_enqueue - w->nsecs_start,
					     w->nsecs_copy_start - w->nsecs_start,
					     w->nsecs_copy_done - w->nsecs_start,
					     w->nsecs_cq_filled - w->nsecs_start,
					     w->nsecs_target - w->nsecs_start);
#endif
				mb(); /* Reclaimer shall see after here */
				w->is_completed = true;
			}

			curr = w->next;
		}

		for (qidx = 1; qidx <= nvmev_vdev->nr_cq; qidx++) {
			struct nvmev_completion_queue *cq = nvmev_vdev->cqes[qidx];

#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
			if ((worker->id) != __get_io_worker(qidx))
				continue;
#endif
			if (cq == NULL || !cq->irq_enabled)
				continue;

			if (mutex_trylock(&cq->irq_lock)) {
				if (cq->interrupt_ready == true) {
#ifdef PERF_DEBUG
					prev_clock = local_clock();
#endif
					cq->interrupt_ready = false;
					nvmev_signal_irq(cq->irq_vector);

#ifdef PERF_DEBUG
					intr_clock[qidx] += (local_clock() - prev_clock);
					intr_counter[qidx]++;

					if (intr_counter[qidx] > 1000) {
						NVMEV_DEBUG("Intr %d: %llu\n", qidx,
							    intr_clock[qidx] / intr_counter[qidx]);
						intr_clock[qidx] = 0;
						intr_counter[qidx] = 0;
					}
#endif
				}
				mutex_unlock(&cq->irq_lock);
			}
		}
		if (CONFIG_NVMEVIRT_IDLE_TIMEOUT != 0 &&
		    time_after(jiffies, last_io_time + (CONFIG_NVMEVIRT_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else
			cond_resched();
	}

	return 0;
}

void NVMEV_IO_WORKER_INIT(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i, worker_id;

	nvmev_vdev->io_workers =
		kcalloc(nvmev_vdev->config.nr_io_workers, sizeof(struct nvmev_io_worker), GFP_KERNEL);
	nvmev_vdev->io_worker_turn = 0;

	for (worker_id = 0; worker_id < nvmev_vdev->config.nr_io_workers; worker_id++) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[worker_id];

		worker->work_queue =
			kzalloc(sizeof(struct nvmev_io_work) * NR_MAX_PARALLEL_IO, GFP_KERNEL);
		for (i = 0; i < NR_MAX_PARALLEL_IO; i++) {
			worker->work_queue[i].next = i + 1;
			worker->work_queue[i].prev = i - 1;
		}
		worker->work_queue[NR_MAX_PARALLEL_IO - 1].next = -1;
		worker->id = worker_id;
		worker->free_seq = 0;
		worker->free_seq_end = NR_MAX_PARALLEL_IO - 1;
		worker->io_seq = -1;
		worker->io_seq_end = -1;

		snprintf(worker->thread_name, sizeof(worker->thread_name), "nvmev_io_worker_%d", worker_id);

		worker->task_struct = kthread_create(nvmev_io_worker, worker, "%s", worker->thread_name);

		kthread_bind(worker->task_struct, nvmev_vdev->config.cpu_nr_io_workers[worker_id]);
		wake_up_process(worker->task_struct);
	}
}

void NVMEV_IO_WORKER_FINAL(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i;

	for (i = 0; i < nvmev_vdev->config.nr_io_workers; i++) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[i];

		if (!IS_ERR_OR_NULL(worker->task_struct)) {
			kthread_stop(worker->task_struct);
		}

		kfree(worker->work_queue);
	}

	kfree(nvmev_vdev->io_workers);
}
