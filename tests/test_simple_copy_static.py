#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import pathlib
import re
import subprocess
import unittest


REPO = pathlib.Path(__file__).resolve().parents[1]


def read(path):
    return (REPO / path).read_text()


def run(cmd, timeout=180):
    return subprocess.run(
        cmd,
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )


class SimpleCopySourceInvariants(unittest.TestCase):
    def test_copy_module_is_built(self):
        self.assertIn("copy.o", read("Kbuild"))

    def test_opcode_and_status_surface_exist(self):
        nvme_h = read("nvme.h")

        self.assertIn("NVME_CTRL_ONCS_COPY = 1 << 8", nvme_h)
        self.assertIn("op(nvme_cmd_copy, 0x19)", nvme_h)
        self.assertIn("struct nvme_copy_command", nvme_h)
        self.assertIn("struct nvme_copy_range", nvme_h)
        self.assertIn("NVME_SC_OVERLAPPING_RANGE", nvme_h)
        self.assertIn("NVME_SC_CMD_SIZE_LIM_EXCEEDED", nvme_h)

    def test_copy_limits_are_consistent(self):
        copy_h = read("copy.h")
        copy_c = read("copy.c")

        self.assertRegex(copy_h, r"#define\s+NVMEV_COPY_MSSRL\s+1024U")
        self.assertRegex(copy_h, r"#define\s+NVMEV_COPY_MCL\s+1024U")
        self.assertRegex(copy_h, r"#define\s+NVMEV_COPY_MSRC\s+127U")
        self.assertIn("ctx->nr_ranges > NVMEV_COPY_MSRC + 1", copy_c)
        self.assertIn("ctx->desc_bytes > PAGE_SIZE", copy_c)
        self.assertIn("nlb > NVMEV_COPY_MSSRL", copy_c)
        self.assertIn("total + nlb > NVMEV_COPY_MCL", copy_c)

    def test_copy_validation_rejects_unsupported_and_dangerous_fields(self):
        copy_c = read("copy.c")

        self.assertIn("cmd->flags || cmd->metadata || cmd->dspec", copy_c)
        self.assertIn("cmd->reftag || cmd->apptag || cmd->appmask", copy_c)
        self.assertIn("desfmt != NVMEV_COPY_FORMAT_0", copy_c)
        self.assertIn("NVME_SC_INVALID_FIELD", copy_c)
        self.assertIn("NVME_SC_DATA_XFER_ERROR", copy_c)
        self.assertIn("NVME_SC_LBA_RANGE", copy_c)
        self.assertIn("NVME_SC_OVERLAPPING_RANGE", copy_c)

    def test_copy_prp_decoder_handles_lists_and_offsets(self):
        copy_c = read("copy.c")
        prp_list = re.search(
            r"static int __copy_from_prp_list.*?^}\n",
            copy_c,
            re.S | re.M,
        )

        self.assertIn("__copy_from_prp_data_page", copy_c)
        self.assertIn("__copy_from_prp_list", copy_c)
        self.assertIn("NVMEV_PRPS_PER_PAGE", copy_c)
        self.assertIn("PAGE_SIZE - page_offs", copy_c)
        self.assertIn("if (!paddr)\n\t\treturn -EFAULT;", copy_c)
        self.assertIn("entries = kmalloc(PAGE_SIZE, GFP_KERNEL)", copy_c)
        self.assertIn("memcpy(entries, map.addr, PAGE_SIZE)", copy_c)
        self.assertIn("__unmap_prp_page(&map)", copy_c)
        self.assertIn("le64_to_cpu(entries[i])", copy_c)
        self.assertIn("next_list & PAGE_OFFSET_MASK", copy_c)
        self.assertIn("len - copied <= PAGE_SIZE", copy_c)
        self.assertIsNotNone(prp_list)
        list_body = prp_list.group(0)
        self.assertLess(
            list_body.index("memcpy(entries, map.addr, PAGE_SIZE)"),
            list_body.index("__unmap_prp_page(&map)"),
        )
        self.assertLess(
            list_body.index("__unmap_prp_page(&map)"),
            list_body.index("for (i = 0; i < NVMEV_PRPS_PER_PAGE"),
        )

    def test_copy_prp_fault_injection_is_one_shot(self):
        nvmev_h = read("nvmev.h")
        copy_c = read("copy.c")
        main_c = read("main.c")

        self.assertIn("atomic_t copy_prp_fault_once", nvmev_h)
        self.assertIn("__copy_consume_prp_fault_once", copy_c)
        self.assertIn("atomic_dec_if_positive(&nvmev_vdev->copy_prp_fault_once)", copy_c)
        self.assertIn("ctx->status = NVME_SC_DATA_XFER_ERROR", copy_c)
        self.assertIn("copy_prp_fault_once", main_c)
        self.assertIn("copy_prp_fault_clear", main_c)
        self.assertIn('proc_create("debug", 0664', main_c)
        self.assertIn("atomic_set(&nvmev_vdev->copy_prp_fault_once, 0)", main_c)

    def test_supported_namespace_check_is_bounded_and_zero_based(self):
        copy_c = read("copy.c")

        self.assertIn("ns->id >= NR_NAMESPACES", copy_c)
        self.assertIn("NS_SSD_TYPE(ns->id) == SSD_TYPE_NVM", copy_c)
        self.assertIn("NS_SSD_TYPE(ns->id) == SSD_TYPE_CONV", copy_c)
        self.assertNotIn("NS_SSD_TYPE(ns->id - 1)", copy_c)

    def test_admin_advertisement_matches_supported_surface(self):
        admin_c = read("admin.c")
        nvme_h = read("nvme.h")

        self.assertIn("[nvme_cmd_copy]", admin_c)
        self.assertIn("NVME_CMD_EFFECTS_CSUPP", admin_c)
        self.assertIn("NVME_CMD_EFFECTS_LBCC", admin_c)
        self.assertIn("NVME_CTRL_ONCS_COPY", admin_c)
        self.assertIn("ns->mssrl", admin_c)
        self.assertIn("ns->mcl", admin_c)
        self.assertIn("ns->msrc", admin_c)
        self.assertIn("ctrl->ocfs", admin_c)
        self.assertIn("le16_to_cpu(ctrl->oncs) | NVME_CTRL_ONCS_COPY", admin_c)
        self.assertNotIn("raw + 74", admin_c)
        self.assertNotIn("raw + 76", admin_c)
        self.assertNotIn("raw + 80", admin_c)
        self.assertIn("static_assert(offsetof(struct nvme_id_ns, mssrl) == 74)", nvme_h)
        self.assertIn("static_assert(offsetof(struct nvme_id_ctrl, ocfs) == 534)", nvme_h)
        self.assertIn("NVMEV_COPY_MSSRL", admin_c)
        self.assertIn("NVMEV_COPY_MCL", admin_c)
        self.assertIn("NVMEV_COPY_MSRC", admin_c)

    def test_admin_namespace_paths_convert_and_validate_nsid(self):
        admin_c = read("admin.c")

        self.assertGreaterEqual(admin_c.count("le32_to_cpu(cmd->nsid)"), 4)
        self.assertGreaterEqual(admin_c.count("NVME_SC_INVALID_NS"), 3)
        self.assertNotIn("int nsid = cmd->nsid - 1", admin_c)
        self.assertNotIn("if (i > cmd->nsid)", admin_c)

    def test_ftl_success_hands_decoded_snapshot_to_worker(self):
        simple_c = read("simple_ftl.c")
        conv_c = read("conv_ftl.c")

        for source in (simple_c, conv_c):
            self.assertIn("ret->copy_sdlba = ctx.sdlba", source)
            self.assertIn("ret->copy_nr_ranges = ctx.nr_ranges", source)
            self.assertIn("ret->copy_ranges = ranges", source)
            self.assertIn("ret->bytes = LBA_TO_BYTE(ctx.total_lbas)", source)

    def test_copy_ftl_failures_complete_with_status(self):
        simple_c = read("simple_ftl.c")
        conv_c = read("conv_ftl.c")

        for source in (simple_c, conv_c):
            load_error = re.search(
                r"err = nvmev_copy_load_ranges.*?if \(err\) \{(?P<body>.*?)\n\s*\}",
                source,
                re.S,
            )
            self.assertIsNotNone(load_error)
            self.assertIn("ret->status = NVME_SC_INTERNAL", load_error.group("body"))
            self.assertIn("ret->nsecs_target = req->nsecs_start", load_error.group("body"))
            self.assertIn("return true", load_error.group("body"))

        buffer_pressure = re.search(
            r"if \(allocated_buf_size < program_bytes\) \{(?P<body>.*?)\n\s*\}",
            conv_c,
            re.S,
        )
        self.assertIsNotNone(buffer_pressure)
        self.assertIn("ret->status = NVME_SC_INTERNAL", buffer_pressure.group("body"))
        self.assertIn("nvmev_copy_free_ranges(ranges)", buffer_pressure.group("body"))
        self.assertIn("return true", buffer_pressure.group("body"))
        self.assertNotIn("return false", buffer_pressure.group("body"))

    def test_conv_rw_failures_complete_with_status(self):
        conv_c = read("conv_ftl.c")
        conv_read = re.search(
            r"static bool conv_read\(.*?\n}\n\nstatic bool conv_write",
            conv_c,
            re.S,
        )
        conv_write = re.search(
            r"static bool conv_write\(.*?\n}\n\nstatic uint64_t conv_copy_source_reads",
            conv_c,
            re.S,
        )

        self.assertIsNotNone(conv_read)
        self.assertIsNotNone(conv_write)
        read_body = conv_read.group(0)
        write_body = conv_write.group(0)

        self.assertIn("ret->status = NVME_SC_LBA_RANGE", read_body)
        self.assertIn("ret->nsecs_target = req->nsecs_start", read_body)
        self.assertIn("return true", read_body)

        self.assertIn("ret->status = NVME_SC_LBA_RANGE", write_body)
        self.assertIn("ret->status = NVME_SC_INTERNAL", write_body)
        self.assertIn("ret->nsecs_target = req->nsecs_start", write_body)
        self.assertIn("return true", write_body)

    def test_zns_failures_complete_with_status(self):
        zns_rw_c = read("zns_read_write.c")
        zns_send_c = read("zns_mgmt_send.c")
        zns_recv_c = read("zns_mgmt_recv.c")
        zns_write = re.search(
            r"bool zns_write\(.*?\n}\n\nbool zns_read",
            zns_rw_c,
            re.S,
        )
        zns_read = re.search(
            r"bool zns_read\(.*?\n}\n$",
            zns_rw_c,
            re.S,
        )
        zns_mgmt_send = re.search(
            r"void zns_zmgmt_send\(.*?\n}\n$",
            zns_send_c,
            re.S,
        )

        self.assertIn("static bool __zns_lba_range_valid", zns_rw_c)
        self.assertIn("static void __zns_complete_status", zns_rw_c)
        self.assertNotIn("return false;", zns_rw_c)
        self.assertIn("NVME_SC_INTERNAL", zns_rw_c)
        self.assertIn("NVME_SC_LBA_RANGE", zns_rw_c)
        self.assertIn("release_zone_resource(zns_ftl, OPEN_ZONE)", zns_rw_c)
        self.assertIn("uint64_t allocated_bytes = 0", zns_rw_c)
        self.assertIn("status != NVME_SC_SUCCESS && allocated_bytes", zns_rw_c)
        self.assertIn("buffer_release(write_buffer, allocated_bytes)", zns_rw_c)

        self.assertIsNotNone(zns_write)
        write_body = zns_write.group(0)
        self.assertLess(
            write_body.index("__zns_lba_range_valid"),
            write_body.index("zone_descs[zid].zrwav"),
        )
        self.assertIn("__zns_complete_status(req, ret, NVME_SC_LBA_RANGE)", write_body)
        self.assertIn("return true", write_body)

        self.assertIsNotNone(zns_read)
        read_body = zns_read.group(0)
        self.assertLess(
            read_body.index("__zns_lba_range_valid"),
            read_body.index("zone_descs[zid].state"),
        )
        self.assertIn("__zns_complete_status(req, ret, NVME_SC_LBA_RANGE)", read_body)
        self.assertIn("return true", read_body)

        self.assertIn("uint32_t status = NVME_SC_INVALID_FIELD", zns_send_c)
        self.assertIsNotNone(zns_mgmt_send)
        send_body = zns_mgmt_send.group(0)
        self.assertIn("!select_all && zid >= zns_ftl->zp.nr_zones", send_body)
        self.assertIn("status = NVME_SC_LBA_RANGE", send_body)
        self.assertLess(
            send_body.index("!select_all && zid >= zns_ftl->zp.nr_zones"),
            send_body.index("status = __zmgmt_send"),
        )

        self.assertIn("__zns_copy_to_prp", zns_recv_c)
        self.assertIn("__zns_copy_to_prp_list", zns_recv_c)
        self.assertIn("le64_to_cpu(cmd->prp1)", zns_recv_c)
        self.assertIn("le64_to_cpu(cmd->prp2)", zns_recv_c)
        self.assertIn("le64_to_cpu(cmd->slba)", zns_recv_c)
        self.assertIn("le32_to_cpu(cmd->nr_dw)", zns_recv_c)
        self.assertIn("__zone_report_capacity", zns_recv_c)
        self.assertIn("min_t(uint64_t, length, report_capacity)", zns_recv_c)
        self.assertIn("memset(buffer, 0x00, report_capacity)", zns_recv_c)
        self.assertIn("status = NVME_SC_DATA_XFER_ERROR", zns_recv_c)
        self.assertIn("*status = NVME_SC_LBA_RANGE", zns_recv_c)
        self.assertIn("ret->bytes = status == NVME_SC_SUCCESS ? transfer_len : 0", zns_recv_c)
        self.assertNotIn("__prp_transfer_data", zns_recv_c)

    def test_kv_bounds_checks_before_legacy_paths(self):
        kv_c = read("kv_ftl.c")

        self.assertIn("static bool kv_cmd_inline_key_supported", kv_c)
        self.assertIn("key_len > KVCMD_INLINE_KEY_MAX", kv_c)
        self.assertIn("*status = NVME_SC_INVALID_FIELD", kv_c)
        self.assertIn("static int __kv_transfer_value_prp", kv_c)
        self.assertIn("static int __kv_transfer_prp_list", kv_c)
        self.assertIn("le64_to_cpu(kv_io_cmd_value_prp(cmd, 1))", kv_c)
        self.assertIn("le64_to_cpu(kv_io_cmd_value_prp(cmd, 2))", kv_c)
        self.assertIn("NVMEV_KV_PRPS_PER_PAGE", kv_c)
        self.assertNotIn("paddr_list = kmap_atomic_pfn", kv_c)
        self.assertNotIn("vaddr = kmap_atomic_pfn", kv_c)

        kv_io = re.search(
            r"static unsigned int __do_perform_kv_io\(.*?\n}\n\nstatic unsigned int __do_perform_kv_batched_io",
            kv_c,
            re.S,
        )
        self.assertIsNotNone(kv_io)
        kv_io_body = kv_io.group(0)
        self.assertLess(
            kv_io_body.index("kv_cmd_inline_key_supported"),
            kv_io_body.index("get_mapping_entry"),
        )
        self.assertIn("__kv_transfer_value_prp", kv_io_body)
        self.assertIn("NVME_SC_DATA_XFER_ERROR", kv_io_body)

        kv_batch = re.search(
            r"static unsigned int __do_perform_kv_batch\(.*?\n}\n\nstatic unsigned int kv_iter_open",
            kv_c,
            re.S,
        )
        self.assertIsNotNone(kv_batch)
        batch_body = kv_batch.group(0)
        self.assertIn("sub_cmd_cnt > MAX_SUB_CMD", batch_body)
        self.assertIn("length < sizeof(struct batch_cmd_head)", batch_body)
        self.assertIn("if (!value || !buffer)", batch_body)
        self.assertIn("__kv_transfer_value_prp(cmd, buffer, length, false)", batch_body)
        self.assertIn("NVME_SC_DATA_XFER_ERROR", batch_body)
        self.assertIn("goto out", batch_body)
        self.assertLess(
            batch_body.index("key_len <= 0 || key_len > KVCMD_INLINE_KEY_MAX"),
            batch_body.index("sub_len +="),
        )
        self.assertLess(
            batch_body.index("payload_offset + sub_len > length"),
            batch_body.index("memcpy(key,"),
        )

        kv_iter_read = re.search(
            r"static unsigned int kv_iter_read\(.*?\n}\n\nstatic unsigned int __do_perform_kv_iter_io",
            kv_c,
            re.S,
        )
        self.assertIsNotNone(kv_iter_read)
        iter_body = kv_iter_read.group(0)
        self.assertIn("iter <= 0 || iter >= ARRAY_SIZE(kv_ftl->iter_handle)", iter_body)
        self.assertLess(
            iter_body.index("iter <= 0 || iter >= ARRAY_SIZE(kv_ftl->iter_handle)"),
            iter_body.index("handle = kv_ftl->iter_handle[iter]"),
        )
        kv_iter_open = re.search(
            r"static unsigned int kv_iter_open\(.*?\n}\n\nstatic unsigned int kv_iter_close",
            kv_c,
            re.S,
        )
        self.assertIsNotNone(kv_iter_open)
        open_body = kv_iter_open.group(0)
        self.assertIn("iter < ARRAY_SIZE(kv_ftl->iter_handle)", open_body)
        self.assertIn("if (!kv_ftl->iter_handle[iter])", open_body)
        self.assertIn("if (!kv_ftl->iter_handle[iter]->buf)", open_body)
        self.assertIn("*status = NVME_SC_INTERNAL", open_body)
        self.assertIn("__kv_transfer_value_prp(cmd, handle->buf, buf_offset, true)", iter_body)
        self.assertIn("NVME_SC_DATA_XFER_ERROR", iter_body)
        self.assertIn("for (i = 0; i < ARRAY_SIZE(kv_ftl->iter_handle); i++)", kv_c)

    def test_backing_store_access_is_serialized(self):
        nvmev_h = read("nvmev.h")
        main_c = read("main.c")
        io_c = read("io.c")

        self.assertIn("struct mutex storage_lock", nvmev_h)
        self.assertIn("mutex_init(&ns[i].storage_lock)", main_c)
        self.assertIn("mutex_lock(&nvmev_vdev->ns[nsid].storage_lock)", io_c)
        self.assertIn("mutex_unlock(&nvmev_vdev->ns[nsid].storage_lock)", io_c)
        self.assertIn("paddr_list_base", io_c)
        self.assertIn("data_base = paddr & PAGE_MASK", io_c)
        self.assertIn("vaddr_base", io_c)
        self.assertNotIn("memremap(paddr, PAGE_SIZE", io_c)
        self.assertNotIn("memunmap(paddr_list)", io_c)
        self.assertIn("NVMEV_IO_MAX_PRPS", io_c)
        self.assertIn("NVMEV_DMA_MAX_PRPS", io_c)
        self.assertIn("paddr_list = kcalloc", io_c)
        self.assertIn("kfree(paddr_list)", io_c)
        self.assertIn("tmp_paddr_base", io_c)
        self.assertNotIn("static u64 paddr_list", io_c)

        dma_fn = re.search(
            r"static unsigned int __do_perform_io_using_dma\(.*?\n}\n\nstatic uint64_t __do_perform_copy",
            io_c,
            re.S,
        )
        self.assertIsNotNone(dma_fn)
        dma_body = dma_fn.group(0)
        self.assertIn("mutex_lock(&nvmev_vdev->ns[nsid].storage_lock)", dma_body)
        self.assertIn("out_unlock:", dma_body)
        self.assertIn("tmp_paddr_base = kmap_atomic_pfn", dma_body)
        self.assertIn("tmp_paddr_base = memremap", dma_body)
        self.assertIn("tmp_paddr_list = (u64 *)((u8 *)tmp_paddr_base +", dma_body)
        self.assertLess(dma_body.index("paddr_list = kcalloc"), dma_body.index("mutex_lock("))
        self.assertLess(dma_body.index("out_unlock:"), dma_body.index("kfree(paddr_list)"))
        self.assertLess(dma_body.index("mutex_lock("), dma_body.rindex("mutex_unlock("))

        copy_fn = re.search(
            r"static uint64_t __do_perform_copy\(.*?\n}\n\nstatic void __insert_req_sorted",
            io_c,
            re.S,
        )
        self.assertIsNotNone(copy_fn)
        body = copy_fn.group(0)
        self.assertIn("mutex_lock(&ns->storage_lock)", body)
        self.assertIn("mutex_unlock(&ns->storage_lock)", body)
        self.assertLess(body.index("mutex_lock(&ns->storage_lock)"), body.index("memcpy("))
        self.assertLess(body.index("memcpy("), body.index("mutex_unlock(&ns->storage_lock)"))

    def test_worker_status_gate_precedes_payload_paths(self):
        io_c = read("io.c")
        worker = re.search(
            r"if \(w->status != NVME_SC_SUCCESS\).*?else if \(w->opcode == nvme_cmd_copy\).*?else if \(io_using_dma\)",
            io_c,
            re.S,
        )

        self.assertIsNotNone(worker)
        self.assertIn("nvmev_copy_free_ranges(w->copy_ranges)", worker.group(0))
        self.assertIn("__do_perform_copy(w)", io_c)
        self.assertIn("memcpy_fromio(&cmd_snapshot, &sq_entry(sq_entry)", io_c)
        self.assertIn("struct nvme_command command", read("nvmev.h"))
        self.assertIn("w->command = *cmd_snapshot", io_c)
        self.assertIn("w->opcode = cmd_snapshot->common.opcode", io_c)
        self.assertIn("w->nsid = le32_to_cpu(cmd_snapshot->common.nsid)", io_c)
        self.assertIn("const struct nvme_rw_command *cmd = &w->command.rw", io_c)
        self.assertIn("__do_perform_io_using_dma(w)", io_c)
        self.assertIn("__do_perform_io(w)", io_c)
        self.assertIn("ns->identify_io_cmd(ns, w->command)", io_c)
        self.assertIn("ns, &w->command", io_c)
        self.assertIn("w->copy_expected_bytes = ret->bytes", io_c)

    def test_proc_io_cmd_false_completes_with_internal_status(self):
        io_c = read("io.c")
        proc = re.search(
            r"static size_t __nvmev_proc_io\(.*?\n}\n\nint nvmev_proc_io_sq",
            io_c,
            re.S,
        )

        self.assertIsNotNone(proc)
        body = proc.group(0)
        false_branch = re.search(
            r"if \(!ns->proc_io_cmd\(ns, &req, &ret\)\) \{(?P<body>.*?)\n\s*\}",
            body,
            re.S,
        )
        self.assertIsNotNone(false_branch)
        self.assertIn("ret.status = NVME_SC_INTERNAL", false_branch.group("body"))
        self.assertIn("ret.bytes = 0", false_branch.group("body"))
        self.assertIn("ret.nsecs_target = nsecs_start", false_branch.group("body"))
        self.assertNotIn("return false", false_branch.group("body"))
        branch_start = body.index("if (!ns->proc_io_cmd(ns, &req, &ret))")
        stats_after_branch = body.index("__record_io_stats(cmd, &ret, *io_size)",
                                        branch_start)
        self.assertLess(
            branch_start,
            stats_after_branch,
        )

    def test_rw_payload_mapping_failures_complete_with_error_status(self):
        io_c = read("io.c")
        rw_fn = re.search(
            r"static unsigned int __do_perform_io\(.*?\n}\n\n#define NVMEV_DMA_MAX_PRPS",
            io_c,
            re.S,
        )
        dma_fn = re.search(
            r"static unsigned int __do_perform_io_using_dma\(.*?\n}\n\nstatic uint64_t __do_perform_copy",
            io_c,
            re.S,
        )
        worker = re.search(
            r"if \(w->is_copied == false\) \{(?P<body>.*?)\n\s*w->is_copied = true;",
            io_c,
            re.S,
        )

        self.assertIsNotNone(rw_fn)
        self.assertIsNotNone(dma_fn)
        self.assertIsNotNone(worker)

        rw_body = rw_fn.group(0)
        dma_body = dma_fn.group(0)
        worker_body = worker.group("body")

        self.assertRegex(rw_body, r"if \(prp_offs > NVMEV_IO_MAX_PRPS\)\s+goto out_unlock;")
        self.assertRegex(rw_body, r"if \(!paddr\)\s+goto out_unlock;")
        self.assertRegex(rw_body, r"if \(!paddr_list\)\s+goto out_unlock;")
        self.assertRegex(rw_body, r"if \(!paddr_list_base\)\s+goto out_unlock;")
        self.assertRegex(rw_body, r"if \(!vaddr_base\)\s+goto out_unlock;")
        self.assertIn("return ret", rw_body)

        self.assertRegex(dma_body, r"if \(!list_paddr\)\s+goto out_unlock;")
        self.assertRegex(dma_body, r"if \(!tmp_paddr_list\)\s+goto out_unlock;")
        self.assertRegex(dma_body, r"if \(!paddr_list\[prp_offs\]\)\s+goto out_unlock;")
        self.assertRegex(dma_body, r"if \(!paddr\)\s+goto out_unlock;")
        self.assertIn("return ret", dma_body)

        self.assertRegex(
            worker_body,
            r"(?s)if \(!__do_perform_io_using_dma\(.*?\)\)\s+w->status\s*=\s*NVME_SC_DATA_XFER_ERROR;",
        )
        self.assertRegex(
            worker_body,
            r"(?s)if \(!__do_perform_io\(.*?\)\)\s+w->status =\s*NVME_SC_DATA_XFER_ERROR;",
        )

    def test_copy_payload_short_execution_fails_completion(self):
        io_c = read("io.c")
        nvmev_h = read("nvmev.h")
        copy_fn = re.search(
            r"static uint64_t __do_perform_copy\(.*?\n}\n\nstatic void __insert_req_sorted",
            io_c,
            re.S,
        )
        worker = re.search(
            r"else if \(w->opcode == nvme_cmd_copy\) \{(?P<body>.*?)\n\s*\} else if \(io_using_dma\)",
            io_c,
            re.S,
        )

        self.assertIn("uint64_t copy_expected_bytes", nvmev_h)
        self.assertIsNotNone(copy_fn)
        self.assertIsNotNone(worker)
        self.assertIn("return copied", copy_fn.group(0))
        self.assertIn("w->copy_expected_bytes = ret->bytes", io_c)
        self.assertIn("w->copy_expected_bytes = 0", io_c)
        self.assertIn("__do_perform_copy(w) !=", worker.group("body"))
        self.assertIn("w->copy_expected_bytes", worker.group("body"))
        self.assertIn("w->status =", worker.group("body"))
        self.assertIn("NVME_SC_DATA_XFER_ERROR", worker.group("body"))

    def test_failed_copy_branch_only_releases_decoded_ranges(self):
        io_c = read("io.c")
        worker = re.search(
            r"if \(w->status != NVME_SC_SUCCESS\) \{(?P<failed>.*?)\n\s*\} else if \(w->opcode == nvme_cmd_copy\)",
            io_c,
            re.S,
        )

        self.assertIsNotNone(worker)
        failed = worker.group("failed")
        self.assertIn("nvmev_copy_free_ranges(w->copy_ranges)", failed)
        self.assertIn("w->copy_ranges = NULL", failed)
        self.assertIn("w->copy_nr_ranges = 0", failed)
        self.assertNotIn("__do_perform_copy", failed)
        self.assertNotIn("__do_perform_io", failed)
        self.assertNotIn("__do_perform_io_using_dma", failed)

    def test_copy_payload_uses_device_backing_store_not_prps(self):
        io_c = read("io.c")
        copy_fn = re.search(
            r"static uint64_t __do_perform_copy\(.*?\n}\n\nstatic void __insert_req_sorted",
            io_c,
            re.S,
        )

        self.assertIsNotNone(copy_fn)
        body = copy_fn.group(0)
        self.assertIn("memcpy(ns->mapped + LBA_TO_BYTE(dst_lba)", body)
        self.assertNotIn("prp", body.lower())
        self.assertNotIn("sq_entry", body)
        self.assertIn("nvmev_copy_free_ranges(w->copy_ranges)", body)

    def test_rw_and_completion_paths_use_endian_helpers(self):
        io_c = read("io.c")
        simple_c = read("simple_ftl.c")

        for source in (io_c, simple_c):
            self.assertIn("le64_to_cpu(cmd->slba)", source)
            self.assertIn("le16_to_cpu(cmd->length)", source)
        self.assertIn("le64_to_cpu(cmd->prp1)", io_c)
        self.assertIn("le64_to_cpu(cmd->prp2)", io_c)
        self.assertIn("memcpy_fromio(&cqe_snapshot, &cq_entry(i)", io_c)
        self.assertIn("le16_to_cpu(cqe_snapshot.sq_id)", io_c)
        self.assertIn("memcpy_toio(&cq_entry(cq_head), &cqe, sizeof(cqe))", io_c)
        self.assertIn("cpu_to_le16(sqid)", io_c)
        self.assertIn("cpu_to_le16(sq_entry)", io_c)
        self.assertIn("cpu_to_le16(cq->phase | (status << 1))", io_c)
        self.assertIn("cpu_to_le32(result0)", io_c)
        self.assertIn("cpu_to_le32(result1)", io_c)

    def test_copy_counters_are_exported(self):
        nvmev_h = read("nvmev.h")
        main_c = read("main.c")
        io_c = read("io.c")
        simple_c = read("simple_ftl.c")
        conv_c = read("conv_ftl.c")

        self.assertIn("struct nvmev_copy_stat", nvmev_h)
        self.assertIn("NVMEV_COPY_STATUS_BUCKETS", nvmev_h)
        self.assertIn("status[NVMEV_COPY_STATUS_BUCKETS]", nvmev_h)
        self.assertIn("status_overflow", nvmev_h)
        self.assertIn("proc_copy_stat", nvmev_h)
        self.assertIn('proc_create("copy_stat"', main_c)
        self.assertIn("copy_host_payload_avoided_bytes", main_c)
        self.assertIn("copy_status 0x%03x count", main_c)
        self.assertIn("copy_status_overflow", main_c)
        self.assertIn("__record_io_stats", io_c)
        self.assertIn("ret->status & ~NVME_SC_DNR", io_c)
        self.assertIn("copy_stat.status[copy_status]", io_c)
        self.assertIn("copy_stat.status_overflow", io_c)
        self.assertIn("copy_backing_memcpy_bytes", main_c)
        for source in (simple_c, conv_c):
            self.assertIn("ret->copy_desc_bytes = ctx.desc_bytes", source)
            self.assertIn("ret->copy_host_payload_avoided_bytes = ret->bytes * 2", source)

    def test_worker_diagnostics_are_exported(self):
        nvmev_h = read("nvmev.h")
        main_c = read("main.c")

        self.assertIn("proc_worker_stat", nvmev_h)
        self.assertIn('proc_create("worker_stat"', main_c)
        self.assertIn('strcmp(filename, "worker_stat")', main_c)
        self.assertIn("copy_ranges", main_c)
        self.assertIn("free_entries", main_c)
        self.assertIn('remove_proc_entry("worker_stat"', main_c)

    def test_latency_counters_are_exported(self):
        nvmev_h = read("nvmev.h")
        main_c = read("main.c")
        io_c = read("io.c")

        self.assertIn("struct nvmev_opcode_latency_stat", nvmev_h)
        self.assertIn("opcode_latency[256]", nvmev_h)
        self.assertIn("proc_latency_stat", nvmev_h)
        self.assertIn("__record_latency_stats", io_c)
        self.assertIn("__record_latency_enqueue", io_c)
        self.assertIn("__atomic64_update_max", io_c)
        self.assertIn("__atomic64_dec_if_positive", io_c)
        self.assertIn("max_inflight", nvmev_h)
        self.assertIn("atomic64_inc_return(&s->inflight)", io_c)
        self.assertIn("__atomic64_dec_if_positive(&s->inflight)", io_c)
        self.assertIn("wall_completion_ns", nvmev_h)
        self.assertIn("wall_queue_ns", nvmev_h)
        self.assertIn("wall_payload_ns", nvmev_h)
        self.assertIn("model_ns", nvmev_h)
        self.assertIn("wall_after_model_ns", nvmev_h)
        self.assertIn("max_wall_completion_ns", nvmev_h)
        self.assertIn("max_wall_queue_ns", nvmev_h)
        self.assertIn("max_wall_payload_ns", nvmev_h)
        self.assertIn("max_wall_after_model_ns", nvmev_h)
        self.assertIn("dispatcher CPU clock domain", nvmev_h)
        self.assertIn("w->nsecs_copy_start > w->nsecs_start", io_c)
        self.assertIn("w->nsecs_copy_done > w->nsecs_copy_start", io_c)
        self.assertIn("w->nsecs_copy_start = 0", io_c)
        self.assertIn("w->nsecs_copy_done = 0", io_c)
        self.assertIn("w->nsecs_copy_start = curr_nsecs", io_c)
        self.assertIn("w->nsecs_copy_done = local_clock() + delta", io_c)
        self.assertIn("avg_wall_completion_ns", main_c)
        self.assertIn("avg_wall_queue_ns", main_c)
        self.assertIn("avg_wall_payload_ns", main_c)
        self.assertIn("avg_wall_after_model_ns", main_c)
        self.assertNotIn("avg_total_ns", main_c)
        self.assertNotIn("completion_lag_ns", io_c)
        self.assertIn("max_inflight", main_c)
        self.assertIn('proc_create("latency_stat"', main_c)
        self.assertIn('strcmp(filename, "latency_stat")', main_c)
        self.assertIn('remove_proc_entry("latency_stat"', main_c)

    def test_exit_path_has_stage_logs(self):
        main_c = read("main.c")
        nvmev_h = read("nvmev.h")
        io_c = read("io.c")

        self.assertIn("atomic_t quiescing", nvmev_h)
        self.assertIn("bool nvmev_io_drain", nvmev_h)
        self.assertIn("atomic_set(&nvmev_vdev->quiescing, 1)", main_c)
        self.assertIn("nvmev_io_drain(nvmev_vdev, 5000)", main_c)
        self.assertIn("NVMEV_INFO(\"Exit: quiescing I/O before PCI removal", main_c)
        self.assertLess(
            main_c.index("nvmev_io_drain(nvmev_vdev, 5000)"),
            main_c.index("pci_stop_root_bus"),
        )
        self.assertIn("Exit: stopping virtual PCI root bus", main_c)
        self.assertIn("Exit: removing virtual PCI root bus", main_c)
        self.assertIn("Exit: stopping dispatcher and I/O workers", main_c)
        self.assertIn("Exit: finalizing namespaces and storage", main_c)
        self.assertIn("Exit: capturing worker state before PCI removal", main_c)
        self.assertIn("Exit worker %u: pending=%d", main_c)
        self.assertIn("NVMEV_LOG_EXIT_WORKERS(nvmev_vdev)", main_c)
        self.assertIn("static bool __io_workers_idle", io_c)
        self.assertIn("static bool __io_submission_queues_idle", io_c)
        self.assertIn("bool nvmev_io_drain", io_c)
        self.assertIn("NVME_SC_ABORT_REQ", io_c)
        self.assertIn("atomic_read(&nvmev_vdev->quiescing)", io_c)
        self.assertIn("w->nsecs_target <= curr_nsecs ||", io_c)
        self.assertIn("w->nsecs_target <= worker->latest_nsecs ||", io_c)
        self.assertIn("__io_workers_idle(dev) && __io_submission_queues_idle(dev)", io_c)
        self.assertIn("sq->stat.nr_in_flight", io_c)
        self.assertIn("sqid == 0 || sqid > nvmev_vdev->nr_sq", io_c)
        self.assertIn("CQ %d completion with invalid SQ %d", io_c)
        self.assertIn("if (sq->stat.nr_in_flight)", io_c)
        self.assertIn("CQ %d completion with zero in-flight SQ %d", io_c)

    def test_internal_release_is_not_lost_when_work_queue_is_full(self):
        io_c = read("io.c")
        scheduler = re.search(
            r"void schedule_internal_operation\(.*?\n}\n\nstatic void __reclaim_completed_reqs",
            io_c,
            re.S,
        )

        self.assertIsNotNone(scheduler)
        body = scheduler.group(0)
        self.assertIn("if (!worker)", body)
        self.assertIn("buffer_release(write_buffer, buffs_to_release)", body)

    def test_worker_free_list_empty_sentinel_is_safe(self):
        io_c = read("io.c")
        allocator = re.search(
            r"static struct nvmev_io_worker \*__allocate_work_queue_entry\(.*?\n}\n\nstatic bool __enqueue_io_req",
            io_c,
            re.S,
        )
        reclaimer = re.search(
            r"static void __reclaim_completed_reqs\(.*?\n}\n\nstatic bool __io_workers_idle",
            io_c,
            re.S,
        )

        self.assertIsNotNone(allocator)
        self.assertIsNotNone(reclaimer)

        alloc_body = allocator.group(0)
        reclaim_body = reclaimer.group(0)
        self.assertIn("if (e == -1 || e >= NR_MAX_PARALLEL_IO)", alloc_body)
        self.assertLess(
            alloc_body.index("if (e == -1 || e >= NR_MAX_PARALLEL_IO)"),
            alloc_body.index("w = worker->work_queue + e"),
        )
        self.assertIn("worker->free_seq_end = -1", alloc_body)
        self.assertNotIn("if (w->next >= NR_MAX_PARALLEL_IO)", alloc_body)

        self.assertIn("if (worker->free_seq == -1)", reclaim_body)
        self.assertIn("worker->free_seq = first_entry", reclaim_body)
        self.assertIn("worker->work_queue[worker->free_seq_end]", reclaim_body)
        self.assertLess(
            reclaim_body.index("if (worker->free_seq == -1)"),
            reclaim_body.index("worker->work_queue[worker->free_seq_end]"),
        )

    def test_runtime_stress_harness_builds_and_is_guarded(self):
        runner = read("tests/run_simple_copy_runtime_stress.sh")
        harness = read("tests/simple_copy_runtime_stress.c")

        self.assertIn("ALLOW_NVMEV_RUNTIME_STRESS", runner)
        self.assertIn("memmap=", runner)
        self.assertIn('if [ "$#" -ne 2 ]', runner)
        self.assertIn("does not look like an NVMeVirt namespace", runner)
        self.assertIn("sudo -n blockdev --getsz", runner)
        self.assertIn("sudo -n timeout 60s", runner)
        self.assertIn("CSL_Virt_MN_01", runner)
        self.assertIn("timeout 60s", runner)
        self.assertIn("ALLOW_NVMEV_UNLOAD_STRESS", runner)
        self.assertIn("argc != 3", harness)
        self.assertIn("nsid must be non-zero", harness)
        self.assertNotIn('"/dev/nvme3n1"', harness)
        self.assertIn("NVME_SC_INVALID_FIELD", harness)
        self.assertIn("NVME_SC_OVERLAPPING_RANGE", harness)
        self.assertIn("test_prp_boundary", harness)
        self.assertIn("oracle=dest_eq_source", harness)
        self.assertIn("descriptor=prp_boundary", harness)
        self.assertIn("prp_boundary_copy: destination mismatch", harness)
        self.assertIn("test_multi_range_concat", harness)
        self.assertIn("oracle=dest_eq_concatenated_sources", harness)
        self.assertIn("multi_range_concat: destination mismatch", harness)
        self.assertIn("issue_copy_desc(fd, nsid, ranges, sizeof(ranges), dst_lba, 2)", harness)
        self.assertIn("test_malformed_descriptor", harness)
        self.assertIn("malformed_reserved0: wrong status", harness)
        self.assertIn("dest_unchanged=yes,status=expected", harness)
        self.assertIn("test_overlap_rejection", harness)
        self.assertIn("overlap_rejection: wrong status", harness)
        self.assertIn("overlap_rejection: destination changed", harness)
        self.assertIn("test_concurrent_copy_write", harness)
        self.assertIn("oracle=whole_pattern", harness)

        result = run([
            "gcc",
            "-O2",
            "-Wall",
            "-Wextra",
            "-pthread",
            "tests/simple_copy_runtime_stress.c",
            "-o",
            "/tmp/simple_copy_runtime_stress",
        ])
        self.assertEqual(result.returncode, 0, result.stdout)


class SimpleCopyBuildTests(unittest.TestCase):
    def test_diff_has_no_whitespace_errors(self):
        result = run(["git", "diff", "HEAD", "--check"], timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_build_matrix(self):
        commands = [
            ["make", "clean"],
            ["make"],
            ["make", "clean"],
            ["make", "CONFIG_NVMEVIRT_NVM=", "CONFIG_NVMEVIRT_SSD=y"],
            ["make", "clean"],
            ["make", "CONFIG_NVMEVIRT_NVM=", "CONFIG_NVMEVIRT_ZNS=y"],
            ["make", "clean"],
            ["make", "CONFIG_NVMEVIRT_NVM=", "CONFIG_NVMEVIRT_KV=y"],
            ["make", "clean"],
            ["make"],
        ]

        for cmd in commands:
            with self.subTest(cmd=" ".join(cmd)):
                result = run(cmd)
                self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
