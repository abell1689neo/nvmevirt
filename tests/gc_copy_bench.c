// SPDX-License-Identifier: GPL-2.0-only
//
// gc_copy_bench - quantify the host-side cost of F2FS-GC-style valid-block
// relocation, comparing the conventional read+write path against the NVMe
// Simple Copy Command (SCC) path on an NVMeVirt namespace.
//
// The benchmark mimics the *data-movement* core of F2FS garbage collection:
// take NUM_BLOCKS scattered 4KiB blocks (the "valid blocks" of a victim
// segment) and relocate them into a contiguous destination region (the new
// clean segment).  It does this two ways and measures each:
//
//   CONV : per block, O_DIRECT pread(source) -> pwrite(destination).
//          This is what gc_data_segment()/move_data_block() does today:
//          data crosses the host (DRAM + PCIe) twice.
//   SCC  : batch the blocks into NVMe Copy commands (<=128 ranges / <=1024
//          LBAs each, per MSRC/MCL) so the device relocates them internally;
//          no block payload crosses the host.
//
// Metrics (per phase):
//   wall_ms            - wall-clock time (CLOCK_MONOTONIC)
//   cpu_ms             - host *initiator* CPU (getrusage RUSAGE_SELF, u+s).
//                        NOTE: NVMeVirt emulates the device on its io_worker
//                        cores, so the backing memcpy is NOT charged here;
//                        this is exactly the host-side cost we care about.
//   host_xfer_bytes    - bytes that actually crossed host<->device, read from
//                        /proc/nvmev/copy_stat: CONV = read+write opcode bytes,
//                        SCC = copy descriptor bytes only.
//   payload_avoided    - SCC only: copy_host_payload_avoided_bytes.
//
// Usage: gc_copy_bench /dev/nvmeXnY nsid [num_blocks] [iters] [copy_stat_path]

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/nvme_ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define NVME_CMD_COPY 0x19
#define LBA_SIZE 512ULL
#define PAGE_BYTES 4096ULL
#define LBAS_PER_BLOCK (PAGE_BYTES / LBA_SIZE) /* 8 */

/* SCC per-command limits implemented by NVMeVirt (see copy.h). */
#define COPY_MAX_RANGES 128U /* MSRC + 1 */
#define COPY_MAX_LBAS 1024U  /* MCL */

/* How GC-like the source layout is: scatter blocks STRIDE blocks apart. */
#define SCATTER_STRIDE_BLOCKS 2ULL

/* Keep source and destination regions far apart (no overlap, copy rejects it). */
#define SRC_BASE_LBA 8192ULL
#define DST_BASE_LBA 1048576ULL /* ~512 MiB in */

struct copy_range_fmt0 {
	uint8_t rsvd0[8];
	uint64_t slba;
	uint16_t nlb;
	uint8_t rsvd18[6];
	uint32_t eilbrt;
	uint16_t elbat;
	uint16_t elbatm;
} __attribute__((packed));

struct phase_result {
	double wall_ms;
	double cpu_ms;
	long long host_xfer_bytes;
	long long payload_avoided_bytes;
	long long commands;
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
	cmd.timeout_ms = 30000;

	return ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
}

/* ---- /proc/nvmev/copy_stat helpers ------------------------------------ */

static void stats_reset(const char *path)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0)
		die("open copy_stat (reset)");
	if (write(fd, "0", 1) < 0)
		die("write copy_stat (reset)");
	close(fd);
}

static char *stats_slurp(const char *path)
{
	static char buf[8192];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		die("open copy_stat (read)");
	n = read(fd, buf, sizeof(buf) - 1);
	if (n < 0)
		die("read copy_stat");
	buf[n] = '\0';
	close(fd);
	return buf;
}

/* Value following a "<key> <num>" line, or 0 if absent. */
static long long stats_get(const char *buf, const char *key)
{
	const char *p = buf;
	size_t klen = strlen(key);

	while ((p = strstr(p, key))) {
		if ((p == buf || p[-1] == '\n') && p[klen] == ' ')
			return strtoll(p + klen + 1, NULL, 10);
		p += klen;
	}
	return 0;
}

/* "bytes" field of an "opcode 0xNN ... bytes <num>" line. */
static long long stats_opcode_bytes(const char *buf, const char *opcode_hex)
{
	char needle[32];
	const char *p;

	snprintf(needle, sizeof(needle), "opcode %s ", opcode_hex);
	p = strstr(buf, needle);
	if (!p)
		return 0;
	p = strstr(p, "bytes ");
	if (!p)
		return 0;
	return strtoll(p + 6, NULL, 10);
}

/* ---- timing ------------------------------------------------------------ */

static double ts_ms(struct timespec a, struct timespec b)
{
	return (b.tv_sec - a.tv_sec) * 1000.0 +
	       (b.tv_nsec - a.tv_nsec) / 1.0e6;
}

static double tv_ms(struct timeval a, struct timeval b)
{
	return (b.tv_sec - a.tv_sec) * 1000.0 +
	       (b.tv_usec - a.tv_usec) / 1000.0;
}

/* ---- the two relocation paths ----------------------------------------- */

static void run_conv(int fd, uint64_t *src_lba, uint64_t *dst_lba,
		     unsigned int n, unsigned int iters, uint8_t *blkbuf,
		     const char *statp, struct phase_result *r)
{
	struct timespec w0, w1;
	struct rusage u0, u1;
	unsigned int it, i;

	stats_reset(statp);
	getrusage(RUSAGE_SELF, &u0);
	clock_gettime(CLOCK_MONOTONIC, &w0);

	for (it = 0; it < iters; it++) {
		for (i = 0; i < n; i++) {
			//1. syscall 1 (read)
			full_pread(fd, blkbuf, PAGE_BYTES,
				   (off_t)(src_lba[i] * LBA_SIZE));
			//2. syscall 2 (write)
			full_pwrite(fd, blkbuf, PAGE_BYTES,
				    (off_t)(dst_lba[i] * LBA_SIZE));
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &w1);
	getrusage(RUSAGE_SELF, &u1);

	{
		const char *s = stats_slurp(statp);

		r->host_xfer_bytes = stats_opcode_bytes(s, "0x01") +
				     stats_opcode_bytes(s, "0x02");
		r->payload_avoided_bytes = 0;
		r->commands = (long long)iters * n * 2; /* 1 read + 1 write each */
	}
	r->wall_ms = ts_ms(w0, w1);
	//process가 쓴 CPU time
	r->cpu_ms = tv_ms(u0.ru_utime, u1.ru_utime) +
		    tv_ms(u0.ru_stime, u1.ru_stime);
}

static void run_scc(int fd, uint32_t nsid, uint64_t *src_lba, uint64_t *dst_lba,
		    unsigned int n, unsigned int iters, const char *statp,
		    struct phase_result *r)
{
	struct copy_range_fmt0 ranges[COPY_MAX_RANGES];
	struct timespec w0, w1;
	struct rusage u0, u1;
	unsigned int it, i;
	long long cmds = 0;

	stats_reset(statp);
	getrusage(RUSAGE_SELF, &u0); //CPU 시작점: io 대기 동안은 안쓰임
	clock_gettime(CLOCK_MONOTONIC, &w0);//시계 시작점

	for (it = 0; it < iters; it++) {
		/* Chunk the scattered blocks into copy commands that respect
		 * MSRC (<=128 ranges) and MCL (<=1024 LBAs) at once. */
		 //작업 Loop
		for (i = 0; i < n;) {//블록 수 
			unsigned int cnt = 0;
			uint64_t dst = dst_lba[i];

			for (; i < n && cnt < COPY_MAX_RANGES && //명령 당 128개 제한
			       (cnt + 1) * LBAS_PER_BLOCK <= COPY_MAX_LBAS;
			     i++, cnt++) {
				memset(&ranges[cnt], 0, sizeof(ranges[cnt]));
				ranges[cnt].slba = src_lba[i];
				ranges[cnt].nlb = LBAS_PER_BLOCK - 1;
			}

			if (issue_copy_desc(fd, nsid, ranges,
					    cnt * sizeof(ranges[0]), dst, cnt))
				die("copy");
			cmds++;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &w1);//시계 끝ㄴ
	getrusage(RUSAGE_SELF, &u1);//CPU 끝

	{
		const char *s = stats_slurp(statp);

		/* SCC host<->device payload is just the descriptors. */
		r->host_xfer_bytes = stats_get(s, "copy_descriptor_bytes");
		r->payload_avoided_bytes =
			stats_get(s, "copy_host_payload_avoided_bytes");
	}
	r->commands = cmds;
	r->wall_ms = ts_ms(w0, w1);
	r->cpu_ms = tv_ms(u0.ru_utime, u1.ru_utime) +
		    tv_ms(u0.ru_stime, u1.ru_stime);
}

/* ---- main -------------------------------------------------------------- */

static void verify_scc(int fd, uint64_t *src_lba, uint64_t *dst_lba,
		       unsigned int n, uint8_t *a, uint8_t *b)
{
	/* Spot-check first/middle/last relocated block matches its source. */
	unsigned int idx[3] = { 0, n / 2, n - 1 };
	unsigned int j;

	for (j = 0; j < 3; j++) {
		unsigned int i = idx[j];

		full_pread(fd, a, PAGE_BYTES, (off_t)(src_lba[i] * LBA_SIZE));
		full_pread(fd, b, PAGE_BYTES, (off_t)(dst_lba[i] * LBA_SIZE));
		if (memcmp(a, b, PAGE_BYTES)) {
			fprintf(stderr, "verify: block %u src!=dst after SCC\n", i);
			exit(3);
		}
	}
}

int main(int argc, char **argv)
{
	const char *dev, *statp = "/proc/nvmev/copy_stat";
	uint32_t nsid;
	unsigned int n = 512, iters = 50, i;
	uint64_t *src_lba, *dst_lba;
	uint8_t *blkbuf, *aux;
	struct phase_result conv = { 0 }, scc = { 0 };
	int fd;

	if (argc < 3) {
		fprintf(stderr,
			"usage: %s /dev/nvmeXnY nsid [num_blocks] [iters] [copy_stat_path]\n",
			argv[0]);
		return 2;
	}
	dev = argv[1];
	nsid = (uint32_t)strtoul(argv[2], NULL, 0);
	if (argc > 3)
		n = (unsigned int)strtoul(argv[3], NULL, 0);
	if (argc > 4)
		iters = (unsigned int)strtoul(argv[4], NULL, 0);
	if (argc > 5)
		statp = argv[5];
	if (!nsid || !n || !iters) {
		fprintf(stderr, "nsid/num_blocks/iters must be non-zero\n");
		return 2;
	}

	fd = open(dev, O_RDWR | O_DIRECT);
	if (fd < 0)
		die("open");

	src_lba = calloc(n, sizeof(*src_lba));
	dst_lba = calloc(n, sizeof(*dst_lba));
	if (!src_lba || !dst_lba)
		die("calloc");
	if (posix_memalign((void **)&blkbuf, PAGE_BYTES, PAGE_BYTES) ||
	    posix_memalign((void **)&aux, PAGE_BYTES, PAGE_BYTES))
		die("posix_memalign");

	/* Scattered sources, contiguous destination (mirrors GC relocation). */
	for (i = 0; i < n; i++) {
		src_lba[i] = SRC_BASE_LBA +
			     (uint64_t)i * SCATTER_STRIDE_BLOCKS * LBAS_PER_BLOCK;
		dst_lba[i] = DST_BASE_LBA + (uint64_t)i * LBAS_PER_BLOCK;
	}

	/* Pre-fill source blocks with distinct data (outside the timed region). */
	for (i = 0; i < n; i++) {
		fill_pattern(blkbuf, PAGE_BYTES, (uint8_t)(0x40 + (i & 0x3f)));
		full_pwrite(fd, blkbuf, PAGE_BYTES,
			    (off_t)(src_lba[i] * LBA_SIZE));
	}

	printf("# gc_copy_bench dev=%s nsid=%u blocks=%u iters=%u (4KiB blocks)\n",
	       dev, nsid, n, iters);
	printf("# moved-per-iter=%.2f MiB  total-moved=%.2f MiB\n",
	       n * 4096.0 / (1024 * 1024),
	       (double)iters * n * 4096.0 / (1024 * 1024));

	run_conv(fd, src_lba, dst_lba, n, iters, blkbuf, statp, &conv);
	run_scc(fd, nsid, src_lba, dst_lba, n, iters, statp, &scc);
	verify_scc(fd, src_lba, dst_lba, n, blkbuf, aux);

	{
		double moved_mib = (double)iters * n * 4096.0 / (1024 * 1024);

		printf("\n%-6s %10s %10s %14s %16s %10s %12s\n", "mode",
		       "wall_ms", "cpu_ms", "host_xfer_MiB", "payload_avoid_MiB",
		       "commands", "MiB/s");
		printf("%-6s %10.1f %10.1f %14.2f %16s %10lld %12.1f\n", "CONV",
		       conv.wall_ms, conv.cpu_ms,
		       conv.host_xfer_bytes / 1048576.0, "-", conv.commands,
		       moved_mib / (conv.wall_ms / 1000.0));
		printf("%-6s %10.1f %10.1f %14.4f %16.2f %10lld %12.1f\n", "SCC",
		       scc.wall_ms, scc.cpu_ms, scc.host_xfer_bytes / 1048576.0,
		       scc.payload_avoided_bytes / 1048576.0, scc.commands,
		       moved_mib / (scc.wall_ms / 1000.0));

		printf("\n# host initiator CPU: SCC uses %.1f%% of CONV\n",
		       conv.cpu_ms > 0 ? 100.0 * scc.cpu_ms / conv.cpu_ms : 0.0);
		printf("# host<->device transfer: SCC uses %.4f%% of CONV (payload kept on device)\n",
		       conv.host_xfer_bytes > 0 ?
			       100.0 * scc.host_xfer_bytes / conv.host_xfer_bytes :
			       0.0);
		printf("# verify: SCC destination == source (spot-checked) OK\n");
	}

	close(fd);
	free(src_lba);
	free(dst_lba);
	free(blkbuf);
	free(aux);
	return 0;
}
