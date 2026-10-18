#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <linux/fs.h>
#include <linux/io_uring.h>
#include "pr1_uapi.h"

#define MIB (1024UL * 1024UL)

static void die(const char *what)
{
	fprintf(stderr, "FATAL %s: %s\n", what, strerror(errno));
	exit(2);
}

struct ring {
	int fd;
	struct io_uring_params p;
	unsigned *sq_tail, *sq_mask, *sq_array;
	struct io_uring_sqe *sqes;
	unsigned *cq_head, *cq_tail, *cq_mask;
	struct io_uring_cqe *cqes;
};

static void ring_setup(struct ring *r, unsigned entries)
{
	size_t sq_len, cq_len, sqe_len;
	void *sq_map, *cq_map;

	memset(r, 0, sizeof(*r));
	r->fd = syscall(__NR_io_uring_setup, entries, &r->p);
	if (r->fd < 0)
		die("io_uring_setup");
	if (r->p.flags & IORING_SETUP_CQE32) {
		fprintf(stderr, "FATAL ring uses 128-byte CQEs, unsupported here\n");
		exit(2);
	}
	sq_len = r->p.sq_off.array + r->p.sq_entries * sizeof(unsigned);
	cq_len = r->p.cq_off.cqes + r->p.cq_entries * sizeof(struct io_uring_cqe);
	sqe_len = r->p.sq_entries * sizeof(struct io_uring_sqe);
	sq_map = mmap(NULL, sq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
	if (sq_map == MAP_FAILED)
		die("mmap sq ring");
	cq_map = mmap(NULL, cq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
	if (cq_map == MAP_FAILED)
		die("mmap cq ring");
	r->sqes = mmap(NULL, sqe_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
	if (r->sqes == MAP_FAILED)
		die("mmap sqes");
	r->sq_tail = sq_map + r->p.sq_off.tail;
	r->sq_mask = sq_map + r->p.sq_off.ring_mask;
	r->sq_array = sq_map + r->p.sq_off.array;
	r->cq_head = cq_map + r->p.cq_off.head;
	r->cq_tail = cq_map + r->p.cq_off.tail;
	r->cq_mask = cq_map + r->p.cq_off.ring_mask;
	r->cqes = cq_map + r->p.cq_off.cqes;
}

static void ring_register_buffers2(struct ring *r, struct iovec *iov, unsigned nr)
{
	struct io_uring_rsrc_register rr = { .nr = nr, .data = (uint64_t)(uintptr_t)iov };
	long rc = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_BUFFERS2,
			  &rr, (unsigned)sizeof(rr));
	if (rc < 0)
		die("IORING_REGISTER_BUFFERS2");
}

static void ring_update_slot_dmabuf(struct ring *r, unsigned slot, int dmabuf_fd, int target_fd)
{
	struct pr1_io_uring_regbuf_desc d;
	struct pr1_io_uring_rsrc_update2 up;
	long rc;

	memset(&d, 0, sizeof(d));
	d.type = PR1_IO_REGBUF_TYPE_DMABUF;
	d.dmabuf_fd = dmabuf_fd;
	d.target_fd = target_fd;
	memset(&up, 0, sizeof(up));
	up.offset = slot;
	up.flags = PR1_IORING_RSRC_UPDATE_EXTENDED;
	up.data = (uint64_t)(uintptr_t)&d;
	up.nr = 1;
	rc = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_BUFFERS_UPDATE,
		     &up, (unsigned)sizeof(up));
	if (rc < 0)
		die("IORING_REGISTER_BUFFERS_UPDATE (dmabuf)");
}

static int ring_read_fixed(struct ring *r, int fd, unsigned buf_index,
			   uint64_t addr, uint32_t len, uint64_t file_off)
{
	struct io_uring_sqe *sqe;
	unsigned tail;
	long rc;

	for (;;) {
		tail = *r->sq_tail;
		sqe = &r->sqes[tail & *r->sq_mask];
		memset(sqe, 0, sizeof(*sqe));
		sqe->opcode = IORING_OP_READ_FIXED;
		sqe->fd = fd;
		sqe->off = file_off;
		sqe->addr = addr;
		sqe->len = len;
		sqe->buf_index = buf_index;
		r->sq_array[tail & *r->sq_mask] = tail & *r->sq_mask;
		__atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
		rc = syscall(__NR_io_uring_enter, r->fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
		if (rc < 0)
			die("io_uring_enter");
		for (;;) {
			unsigned head = __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE);
			struct io_uring_cqe *cqe;
			int res;

			if (head == *r->cq_tail)
				break;
			cqe = &r->cqes[head & *r->cq_mask];
			res = cqe->res;
			__atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
			if (res == -EAGAIN)
				break;
			return res;
		}
	}
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static long cpu_ms(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000 +
	       (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000;
}

static long max_rss_kb(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_maxrss;
}

static void *alloc_host(size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

	if (p == MAP_FAILED)
		die("mmap host buffer");
	memset(p, 0xa5, len);
	return p;
}

static int p2p_alloc(int drm_fd, uint64_t size)
{
	struct pr1_drm_xe_p2p_alloc a = { .size = size };

	return ioctl(drm_fd, PR1_DRM_IOCTL_XE_P2P_ALLOC, &a);
}

static int xe_vram_bo_prime_fd(int drm_fd, uint64_t size, void **cpu_map, uint32_t *handle)
{
	struct pr1_drm_xe_gem_create bo;
	struct pr1_drm_xe_gem_mmap_offset mm;
	struct drm_prime_handle pr;
	void *map;

	memset(&bo, 0, sizeof(bo));
	bo.size = size;
	bo.placement = 1u << PR1_DRM_XE_MEM_REGION_CLASS_VRAM;
	bo.flags = PR1_DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
	bo.cpu_caching = PR1_DRM_XE_GEM_CPU_CACHING_WC;
	if (ioctl(drm_fd, PR1_DRM_IOCTL_XE_GEM_CREATE, &bo) < 0)
		die("DRM_IOCTL_XE_GEM_CREATE (visible VRAM BO)");

	memset(&mm, 0, sizeof(mm));
	mm.handle = bo.handle;
	if (ioctl(drm_fd, PR1_DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mm) < 0)
		die("DRM_IOCTL_XE_GEM_MMAP_OFFSET");
	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, (off_t)mm.offset);
	if (map == MAP_FAILED)
		die("mmap VRAM BO");
	memset(map, 0x5a, size);

	memset(&pr, 0, sizeof(pr));
	pr.handle = bo.handle;
	pr.flags = O_RDWR | O_CLOEXEC;
	if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &pr) < 0)
		die("DRM_IOCTL_PRIME_HANDLE_TO_FD");
	*cpu_map = map;
	*handle = bo.handle;
	return pr.fd;
}

static void report(const char *leg, uint64_t bytes, double wall, long cpu0)
{
	double gb = bytes / (double)(1024UL * 1024UL * 1024UL);

	printf("LEG %-6s bytes=%llu wall=%.3fs GBps=%.2f cpu_ms=%ld maxrss_kb=%ld\n",
	       leg, (unsigned long long)bytes, wall, gb / wall,
	       cpu_ms() - cpu0, max_rss_kb());
}

static void usage(void)
{
	fprintf(stderr,
		"usage: p2p_direct_bench --drm NODE --bdev DEV --leg host|vram|pread|verify\n"
		"         [--size-mib N] [--block-mib N] [--chunk-mib N] [--file-off BYTES]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *drm = NULL, *bdev = NULL, *leg = NULL;
	uint64_t size_mib = 4096, block_mib = 128, chunk_mib = 256, file_off = 0;
	unsigned long lba = 512;
	uint64_t blk, total, done = 0;
	double t0;
	long c0;
	int drm_fd = -1, bdev_fd, chunk_fd = -1, res;
	void *host = NULL, *chunk_map = NULL;
	struct ring r;
	unsigned buf_index = 0;
	uint64_t addr_base;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--drm") && i + 1 < argc)
			drm = argv[++i];
		else if (!strcmp(argv[i], "--bdev") && i + 1 < argc)
			bdev = argv[++i];
		else if (!strcmp(argv[i], "--leg") && i + 1 < argc)
			leg = argv[++i];
		else if (!strcmp(argv[i], "--size-mib") && i + 1 < argc)
			size_mib = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--block-mib") && i + 1 < argc)
			block_mib = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--chunk-mib") && i + 1 < argc)
			chunk_mib = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--file-off") && i + 1 < argc)
			file_off = strtoull(argv[++i], NULL, 0);
		else
			usage();
	}
	if (!drm || !bdev || !leg)
		usage();

	bdev_fd = open(bdev, O_RDONLY | O_DIRECT);
	if (bdev_fd < 0)
		die("open target O_DIRECT");
	if (ioctl(bdev_fd, BLKSSZGET, &lba) < 0) {
		lba = 512;
		fprintf(stderr, "note: %s is not a block device (BLKSSZGET: %s), assuming %lu-byte logical blocks\n",
			bdev, strerror(errno), lba);
	}

	blk = block_mib * MIB;
	total = size_mib * MIB;
	if (file_off % (uint64_t)lba || blk % (uint64_t)lba || blk % 4096) {
		fprintf(stderr, "FATAL offset/length must be multiples of device sector size %lu and page size\n", lba);
		return 2;
	}

	if (!strcmp(leg, "pread")) {
		if (chunk_mib * MIB < blk) {
			fprintf(stderr, "FATAL --chunk-mib must be >= --block-mib\n");
			return 2;
		}
		drm_fd = open(drm, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0)
			die("open render node");
		chunk_fd = p2p_alloc(drm_fd, chunk_mib * MIB);
		if (chunk_fd < 0) {
			fprintf(stderr, "FATAL DRM_IOCTL_XE_P2P_ALLOC: %s\n", strerror(errno));
			return 3;
		}
		chunk_map = mmap(NULL, chunk_mib * MIB, PROT_READ | PROT_WRITE, MAP_SHARED, chunk_fd, 0);
		if (chunk_map == MAP_FAILED)
			die("mmap chunk");
		c0 = cpu_ms();
		t0 = now_s();
		while (done < total) {
			uint64_t n = (total - done) > blk ? blk : (total - done);
			ssize_t rc = pread(bdev_fd, chunk_map, n, file_off + done);

			if (rc < 0)
				die("pread into chunk mapping");
			if (!rc)
				break;
			done += rc;
		}
		report("pread", done, now_s() - t0, c0);
		return 0;
	}

	if (!strcmp(leg, "verify")) {
		uint64_t n = 32 * MIB;

		drm_fd = open(drm, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0)
			die("open render node");
		chunk_fd = p2p_alloc(drm_fd, chunk_mib * MIB);
		if (chunk_fd < 0) {
			fprintf(stderr, "FATAL DRM_IOCTL_XE_P2P_ALLOC: %s\n", strerror(errno));
			return 3;
		}
		host = alloc_host(n);
		chunk_map = mmap(NULL, chunk_mib * MIB, PROT_READ | PROT_WRITE, MAP_SHARED, chunk_fd, 0);
		if (chunk_map == MAP_FAILED)
			die("mmap chunk");
		if (pread(bdev_fd, host, n, file_off) != (ssize_t)n) {
			fprintf(stderr, "host pread failed: %s\n", strerror(errno));
			return 1;
		}
		if (pread(bdev_fd, chunk_map, n, file_off) != (ssize_t)n) {
			fprintf(stderr, "pread into the chunk mapping failed: %s\n", strerror(errno));
			return 1;
		}
		res = memcmp(host, chunk_map, n);
		res = memcmp(host, chunk_map, n);
		printf("VERIFY %s host_first=%02x%02x%02x%02x vram_first=%02x%02x%02x%02x\n",
		       res ? "MISMATCH" : "MATCH",
		       ((unsigned char *)host)[0], ((unsigned char *)host)[1],
		       ((unsigned char *)host)[2], ((unsigned char *)host)[3],
		       ((unsigned char *)chunk_map)[0], ((unsigned char *)chunk_map)[1],
		       ((unsigned char *)chunk_map)[2], ((unsigned char *)chunk_map)[3]);
		return res ? 1 : 0;
	}

	ring_setup(&r, 8);
	if (!strcmp(leg, "host")) {
		struct iovec iov;

		if (blk > total)
			blk = total;
		host = alloc_host(blk);
		iov.iov_base = host;
		iov.iov_len = blk;
		ring_register_buffers2(&r, &iov, 1);
		addr_base = (uint64_t)(uintptr_t)host;
	} else if (!strcmp(leg, "vram")) {
		struct iovec iov;
		void *dummy;

		if (chunk_mib * MIB < blk) {
			fprintf(stderr, "FATAL --chunk-mib must be >= --block-mib\n");
			return 2;
		}
		drm_fd = open(drm, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0)
			die("open render node");
		chunk_fd = p2p_alloc(drm_fd, chunk_mib * MIB);
		if (chunk_fd < 0) {
			fprintf(stderr, "FATAL DRM_IOCTL_XE_P2P_ALLOC: %s (carve-out off, or pre-PR kernel)\n",
				strerror(errno));
			return 3;
		}
		chunk_map = mmap(NULL, chunk_mib * MIB, PROT_READ | PROT_WRITE, MAP_SHARED, chunk_fd, 0);
		if (chunk_map == MAP_FAILED)
			die("mmap chunk");
		printf("CHUNK size_mib=%llu fd=%d mapped=%p\n",
		       (unsigned long long)chunk_mib, chunk_fd, chunk_map);
		dummy = alloc_host(4096);
		iov.iov_base = dummy;
		iov.iov_len = 4096;
		ring_register_buffers2(&r, &iov, 1);
		ring_update_slot_dmabuf(&r, 0, chunk_fd, bdev_fd);
		addr_base = 0;
	} else if (!strcmp(leg, "vrambo")) {
		struct iovec iov;
		void *dummy, *bo_map = NULL;
		int bo_fd;
		uint32_t bo_handle = 0;
		uint64_t bo_size = (blk + 65535) & ~(uint64_t)65535;
		void *hostref;

		drm_fd = open(drm, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0)
			die("open render node");
		bo_fd = xe_vram_bo_prime_fd(drm_fd, bo_size, &bo_map, &bo_handle);
		printf("VRAM BO handle=%u size_mib=%llu dmabuf_fd=%d cpu_map=%p\n",
		       bo_handle, (unsigned long long)(bo_size / MIB), bo_fd, bo_map);
		dummy = alloc_host(4096);
		iov.iov_base = dummy;
		iov.iov_len = 4096;
		ring_register_buffers2(&r, &iov, 1);
		ring_update_slot_dmabuf(&r, 0, bo_fd, bdev_fd);
		addr_base = 0;

		c0 = cpu_ms();
		t0 = now_s();
		while (done < total) {
			uint64_t n = (total - done) > bo_size ? bo_size : (total - done);

			res = ring_read_fixed(&r, bdev_fd, buf_index, 0, (uint32_t)n, file_off + done);
			if (res < 0) {
				fprintf(stderr, "FATAL READ_FIXED into VRAM BO: %s\n", strerror(-res));
				return 1;
			}
			if (!res)
				break;
			done += (uint64_t)res;
		}
		report("vrambo", done, now_s() - t0, c0);

		hostref = alloc_host(32 * MIB);
		res = ring_read_fixed(&r, bdev_fd, buf_index, 0, (uint32_t)(32 * MIB), file_off);
		if (res < 0) {
			fprintf(stderr, "FATAL verify read into BO: %s\n", strerror(-res));
			return 1;
		}
		if (pread(bdev_fd, hostref, 32 * MIB, file_off) == (ssize_t)(32 * MIB))
			printf("VERIFY %s (first 32MiB of the BO vs the same range in host RAM)\n",
			       memcmp(hostref, bo_map, 32 * MIB) ? "MISMATCH" : "MATCH");
		return 0;
	} else if (!strcmp(leg, "chunkreg")) {
		struct iovec iov;
		void *dummy;
		struct pr1_io_uring_regbuf_desc d;
		struct pr1_io_uring_rsrc_update2 up;
		long rc;

		drm_fd = open(drm, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0)
			die("open render node");
		chunk_fd = p2p_alloc(drm_fd, chunk_mib * MIB);
		if (chunk_fd < 0) {
			fprintf(stderr, "FATAL DRM_IOCTL_XE_P2P_ALLOC: %s\n", strerror(errno));
			return 3;
		}
		dummy = alloc_host(4096);
		iov.iov_base = dummy;
		iov.iov_len = 4096;
		ring_register_buffers2(&r, &iov, 1);
		memset(&d, 0, sizeof(d));
		d.type = PR1_IO_REGBUF_TYPE_DMABUF;
		d.dmabuf_fd = chunk_fd;
		d.target_fd = bdev_fd;
		memset(&up, 0, sizeof(up));
		up.offset = 0;
		up.flags = PR1_IORING_RSRC_UPDATE_EXTENDED;
		up.data = (uint64_t)(uintptr_t)&d;
		up.nr = 1;
		rc = syscall(__NR_io_uring_register, r.fd, IORING_REGISTER_BUFFERS_UPDATE, &up, (unsigned)sizeof(up));
		if (rc < 0)
			printf("EXPECTED-FAIL register chunk fd as dmabuf: %s (chunk fd is an anon inode, not a dma_buf)\n",
			       strerror(errno));
		else
			printf("UNEXPECTED-PASS: the chunk fd was accepted as a dma-buf\n");
		return 0;
	} else {
		usage();
	}

	c0 = cpu_ms();
	t0 = now_s();
	while (done < total) {
		uint64_t n = (total - done) > blk ? blk : (total - done);

		res = ring_read_fixed(&r, bdev_fd, buf_index, addr_base, (uint32_t)n, file_off + done);
		if (res < 0) {
			fprintf(stderr, "FATAL READ_FIXED: %s\n", strerror(-res));
			return 1;
		}
		if (!res)
			break;
		done += (uint64_t)res;
	}
	report(leg, done, now_s() - t0, c0);
	return 0;
}
