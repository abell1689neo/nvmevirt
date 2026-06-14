// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/nvme_ioctl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NVME_CMD_COPY 0x19
#define NVME_SC_INVALID_FIELD 0x2
#define NVME_SC_OVERLAPPING_RANGE 0x114
#define LBA_SIZE 512ULL
#define PAGE_BYTES 4096ULL

struct copy_range_fmt0 {
	uint8_t rsvd0[8];
	uint64_t slba;
	uint16_t nlb;
	uint8_t rsvd18[6];
	uint32_t eilbrt;
	uint16_t elbat;
	uint16_t elbatm;
} __attribute__((packed));

struct thread_arg {
	int fd;
	uint32_t nsid;
	uint64_t src_lba;
	uint64_t dst_lba;
	unsigned int iters;
};

static void die(const char *msg)
{
	perror(msg);
	exit(2);
}

static void fill_pattern(uint8_t *buf, size_t bytes, uint8_t seed)
{
	size_t i;

	for (i = 0; i < bytes; i++)
		buf[i] = (uint8_t)(seed + i * 13U + (i >> 4));
}

static void full_pread(int fd, void *buf, size_t bytes, off_t off)
{
	uint8_t *p = buf;
	size_t done = 0;

	while (done < bytes) {
		ssize_t ret = pread(fd, p + done, bytes - done, off + done);

		if (ret <= 0)
			die("pread");
		done += (size_t)ret;
	}
}

static void full_pwrite(int fd, const void *buf, size_t bytes, off_t off)
{
	const uint8_t *p = buf;
	size_t done = 0;

	while (done < bytes) {
		ssize_t ret = pwrite(fd, p + done, bytes - done, off + done);

		if (ret <= 0)
			die("pwrite");
		done += (size_t)ret;
	}
}

static int issue_copy_desc(int fd, uint32_t nsid, void *desc, uint32_t desc_len,
			   uint64_t dst_lba, uint32_t nr_ranges)
{
	struct nvme_passthru_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = NVME_CMD_COPY;
	cmd.nsid = nsid;
	cmd.addr = (uintptr_t)desc;
	cmd.data_len = desc_len;
	cmd.cdw10 = (uint32_t)dst_lba;
	cmd.cdw11 = (uint32_t)(dst_lba >> 32);
	cmd.cdw12 = nr_ranges - 1;
	cmd.timeout_ms = 5000;

	return ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
}

static void init_one_range(struct copy_range_fmt0 *range, uint64_t src_lba,
			   uint16_t nlb)
{
	memset(range, 0, sizeof(*range));
	range->slba = src_lba;
	range->nlb = nlb;
}

static void expect_copy_success(int fd, uint32_t nsid, uint64_t src_lba,
				uint64_t dst_lba, uint8_t seed,
				const char *name)
{
	uint8_t src[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	uint8_t dst[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	struct copy_range_fmt0 range;
	int ret;

	fill_pattern(src, sizeof(src), seed);
	memset(dst, 0xa5, sizeof(dst));
	full_pwrite(fd, src, sizeof(src), (off_t)(src_lba * LBA_SIZE));
	full_pwrite(fd, dst, sizeof(dst), (off_t)(dst_lba * LBA_SIZE));

	init_one_range(&range, src_lba, (PAGE_BYTES / LBA_SIZE) - 1);
	ret = issue_copy_desc(fd, nsid, &range, sizeof(range), dst_lba, 1);
	if (ret)
		die(name);

	full_pread(fd, dst, sizeof(dst), (off_t)(dst_lba * LBA_SIZE));
	if (memcmp(src, dst, sizeof(src))) {
		fprintf(stderr, "%s: destination mismatch\n", name);
		exit(3);
	}

	printf("%s,PASS,oracle=dest_eq_source\n", name);
}

static void test_prp_boundary(int fd, uint32_t nsid)
{
	uint8_t *raw;
	uint8_t src[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	uint8_t dst[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	struct copy_range_fmt0 *range;
	uintptr_t base;

	if (posix_memalign((void **)&raw, PAGE_BYTES, PAGE_BYTES * 3))
		die("posix_memalign boundary");
	memset(raw, 0, PAGE_BYTES * 3);

	base = (uintptr_t)raw;
	range = (struct copy_range_fmt0 *)(base + PAGE_BYTES - 16);
	init_one_range(range, 0, (PAGE_BYTES / LBA_SIZE) - 1);

	expect_copy_success(fd, nsid, 0, 64, 0x31, "control_copy");
	fill_pattern(src, sizeof(src), 0x41);
	memset(dst, 0x5c, sizeof(dst));
	full_pwrite(fd, src, sizeof(src), 0);
	full_pwrite(fd, dst, sizeof(dst), 128 * LBA_SIZE);
	if (issue_copy_desc(fd, nsid, range, sizeof(*range), 128, 1))
		die("prp_boundary_copy");
	full_pread(fd, dst, sizeof(dst), 128 * LBA_SIZE);
	if (memcmp(src, dst, sizeof(src))) {
		fprintf(stderr, "prp_boundary_copy: destination mismatch\n");
		exit(7);
	}
	printf("prp_boundary_copy,PASS,oracle=dest_eq_source,descriptor=prp_boundary\n");

	free(raw);
}

static void test_multi_range_concat(int fd, uint32_t nsid)
{
	uint8_t src0[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	uint8_t src1[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	uint8_t expected[PAGE_BYTES * 2] __attribute__((aligned(PAGE_BYTES)));
	uint8_t dst[PAGE_BYTES * 2] __attribute__((aligned(PAGE_BYTES)));
	struct copy_range_fmt0 ranges[2];
	uint64_t src0_lba = 400;
	uint64_t src1_lba = 408;
	uint64_t dst_lba = 448;

	fill_pattern(src0, sizeof(src0), 0x52);
	fill_pattern(src1, sizeof(src1), 0x6d);
	memset(dst, 0xcc, sizeof(dst));
	memcpy(expected, src0, sizeof(src0));
	memcpy(expected + sizeof(src0), src1, sizeof(src1));

	full_pwrite(fd, src0, sizeof(src0), (off_t)(src0_lba * LBA_SIZE));
	full_pwrite(fd, src1, sizeof(src1), (off_t)(src1_lba * LBA_SIZE));
	full_pwrite(fd, dst, sizeof(dst), (off_t)(dst_lba * LBA_SIZE));

	init_one_range(&ranges[0], src0_lba, (PAGE_BYTES / LBA_SIZE) - 1);
	init_one_range(&ranges[1], src1_lba, (PAGE_BYTES / LBA_SIZE) - 1);
	if (issue_copy_desc(fd, nsid, ranges, sizeof(ranges), dst_lba, 2))
		die("multi_range_concat");

	full_pread(fd, dst, sizeof(dst), (off_t)(dst_lba * LBA_SIZE));
	if (memcmp(expected, dst, sizeof(expected))) {
		fprintf(stderr, "multi_range_concat: destination mismatch\n");
		exit(10);
	}
	printf("multi_range_concat,PASS,oracle=dest_eq_concatenated_sources,ranges=2\n");
}

static void test_malformed_descriptor(int fd, uint32_t nsid)
{
	uint8_t before[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	uint8_t after[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	struct copy_range_fmt0 range;
	uint64_t dst_lba = 192;
	int ret;

	fill_pattern(before, sizeof(before), 0x77);
	full_pwrite(fd, before, sizeof(before), (off_t)(dst_lba * LBA_SIZE));

	init_one_range(&range, 0, (PAGE_BYTES / LBA_SIZE) - 1);
	range.rsvd0[0] = 0x5a;
	errno = 0;
	ret = issue_copy_desc(fd, nsid, &range, sizeof(range), dst_lba, 1);
	if (!ret) {
		fprintf(stderr, "malformed_reserved0: unexpectedly succeeded\n");
		exit(4);
	}
	if (ret != NVME_SC_INVALID_FIELD) {
		fprintf(stderr,
			"malformed_reserved0: wrong status ret=%d expected=%u errno=%d\n",
			ret, NVME_SC_INVALID_FIELD, errno);
		exit(11);
	}

	full_pread(fd, after, sizeof(after), (off_t)(dst_lba * LBA_SIZE));
	if (memcmp(before, after, sizeof(before))) {
		fprintf(stderr, "malformed_reserved0: destination changed\n");
		exit(5);
	}
	printf("malformed_reserved0,PASS,ret=%d,errno=%d,dest_unchanged=yes,status=expected\n",
	       ret, errno);
}

static void test_overlap_rejection(int fd, uint32_t nsid)
{
	uint8_t before[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	uint8_t after[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	uint8_t src[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	struct copy_range_fmt0 range;
	uint64_t src_lba = 384;
	uint64_t dst_lba = 388;
	int ret;

	fill_pattern(src, sizeof(src), 0x91);
	fill_pattern(before, sizeof(before), 0xb4);
	full_pwrite(fd, src, sizeof(src), (off_t)(src_lba * LBA_SIZE));
	full_pwrite(fd, before, sizeof(before), (off_t)(dst_lba * LBA_SIZE));

	init_one_range(&range, src_lba, (PAGE_BYTES / LBA_SIZE) - 1);
	errno = 0;
	ret = issue_copy_desc(fd, nsid, &range, sizeof(range), dst_lba, 1);
	if (!ret) {
		fprintf(stderr, "overlap_rejection: unexpectedly succeeded\n");
		exit(8);
	}
	if (ret != NVME_SC_OVERLAPPING_RANGE) {
		fprintf(stderr,
			"overlap_rejection: wrong status ret=%d expected=%u errno=%d\n",
			ret, NVME_SC_OVERLAPPING_RANGE, errno);
		exit(12);
	}

	full_pread(fd, after, sizeof(after), (off_t)(dst_lba * LBA_SIZE));
	if (memcmp(before, after, sizeof(before))) {
		fprintf(stderr, "overlap_rejection: destination changed\n");
		exit(9);
	}
	printf("overlap_rejection,PASS,ret=%d,errno=%d,dest_unchanged=yes,status=expected\n",
	       ret, errno);
}

static void *writer_thread(void *opaque)
{
	struct thread_arg *arg = opaque;
	uint8_t buf[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	unsigned int i;

	for (i = 0; i < arg->iters; i++) {
		fill_pattern(buf, sizeof(buf), (uint8_t)(0x20 + (i % 16)));
		full_pwrite(arg->fd, buf, sizeof(buf),
			    (off_t)(arg->src_lba * LBA_SIZE));
	}

	return NULL;
}

static void *copy_thread(void *opaque)
{
	struct thread_arg *arg = opaque;
	struct copy_range_fmt0 range;
	unsigned int i;

	init_one_range(&range, arg->src_lba, (PAGE_BYTES / LBA_SIZE) - 1);
	for (i = 0; i < arg->iters; i++) {
		if (issue_copy_desc(arg->fd, arg->nsid, &range, sizeof(range),
				    arg->dst_lba, 1))
			die("concurrent_copy");
	}

	return NULL;
}

static void test_concurrent_copy_write(int fd, uint32_t nsid)
{
	struct thread_arg arg = {
		.fd = fd,
		.nsid = nsid,
		.src_lba = 256,
		.dst_lba = 320,
		.iters = 128,
	};
	pthread_t writer;
	pthread_t copier;
	uint8_t dst[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	uint8_t expected[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
	unsigned int seed;
	int matched = 0;

	if (pthread_create(&writer, NULL, writer_thread, &arg))
		die("pthread_create writer");
	if (pthread_create(&copier, NULL, copy_thread, &arg))
		die("pthread_create copier");
	if (pthread_join(writer, NULL))
		die("pthread_join writer");
	if (pthread_join(copier, NULL))
		die("pthread_join copier");

	full_pread(fd, dst, sizeof(dst), (off_t)(arg.dst_lba * LBA_SIZE));
	for (seed = 0x20; seed < 0x30; seed++) {
		fill_pattern(expected, sizeof(expected), (uint8_t)seed);
		if (!memcmp(dst, expected, sizeof(dst))) {
			matched = 1;
			break;
		}
	}

	if (!matched) {
		fprintf(stderr, "concurrent_copy_write: torn destination pattern\n");
		exit(6);
	}
	printf("concurrent_copy_write,PASS,oracle=whole_pattern,seed=0x%x\n",
	       seed);
}

int main(int argc, char **argv)
{
	const char *dev;
	uint32_t nsid;
	int fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s /dev/nvmeXnY nsid\n", argv[0]);
		return 2;
	}

	dev = argv[1];
	nsid = (uint32_t)strtoul(argv[2], NULL, 0);
	if (!nsid) {
		fprintf(stderr, "nsid must be non-zero\n");
		return 2;
	}

	fd = open(dev, O_RDWR | O_DIRECT);
	if (fd < 0)
		die("open");

	test_prp_boundary(fd, nsid);
	test_multi_range_concat(fd, nsid);
	test_malformed_descriptor(fd, nsid);
	test_overlap_rejection(fd, nsid);
	test_concurrent_copy_write(fd, nsid);

	close(fd);
	return 0;
}
