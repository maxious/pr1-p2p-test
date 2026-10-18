# Testing valarauca PR 1 (dmabuf-io v9 + xe2 P2PDMA direct storage)

Author's claim (PR comment, 2026-10-18): tested on a 9800X3D + single B70 + single SN8100,
"loading tensors at ~14.2GB/s without touching host memory", XFS/EXT4/mdadm fine, code "absolutely fucked".

Reproduced and compared on a Battlemage test machine: 2x Intel Arc Pro B60 (`8086:e211`), one
MAP1602-class DRAM-less PCIe 4.0 NVMe, kernel `7.3.0-rc7-xe-perf-pr1-p2p` (the PR head, built locally),
carve-out active at `p2p_carveout_mb=2048` per tile with `p2pmem` published on both cards. Raw
captures of the runs below are in [`results/`](results/).

## Results

Reads from `/dev/nvme0n1` at offset 4 GiB (inside an LVM partition, read-only), 64 MiB blocks:

| leg | 4 GiB | 512 MiB | cpu_ms (4 GiB) | peak RSS | DRAM moved per 4 GiB | verify |
| --- | --- | --- | --- | --- | --- | --- |
| `host` device -> host RAM | 4.75 GB/s | 3.10 GB/s | 38 | 67 MB | **5139.5 MiB** | n/a |
| `pread` route A (carve-out chunk) | 4.85 GB/s | 3.86 GB/s | 48 | 67 MB ¹ | **177.4 MiB** | MATCH |
| `vrambo` route B (VRAM BO dma-buf) | 5.07 GB/s | 4.11 GB/s | 7 | 2.1 MB | **433.3 MiB** | MATCH |

Second card (renderD129), 4 GiB: `host` 5.25 GB/s / `vrambo` 5.06 GB/s, peak RSS ~2.3 MB, MATCH.

¹ route A's 67 MB `maxrss` is a transient peak from accounting the ZONE_DEVICE mapping early on:
mid-transfer its VmRSS is ~4 MB (libc only), like route B. No host buffer is held by either route.

Readings:

- The VRAM routes match or edge out the host-RAM route on identical offsets/sizes, at roughly 5x lower
  CPU (7 ms vs 38 ms per 4 GiB) and with a ~2 MB footprint instead of a 64 MiB buffer.
- "Without touching host memory" is directly evidenced: per 4 GiB transferred the host path moves
  5139.5 MiB of DRAM, route A 177.4 MiB and route B 433.3 MiB (route B's extra traffic is consistent
  with its 32 MiB verification pass plus descriptor handling). A P2P NVMe -> VRAM transfer should not
  move DRAM at all, and it essentially does not.
- The ~3-5 GB/s ceiling is the SSD at these offsets, not the P2P path. The author's absolute 14.2 GB/s
  came off an SN8100 (PCIe 5.0); this machine's drive is a different class, so the claim that
  reproduces is "same throughput as the host path, without host memory", not his GB/s.
- Correctness is byte-verified both ways: route A `VERIFY MATCH host_first=3b204d6f vram_first=3b204d6f`
  (host read vs chunk read of the same range), route B `VERIFY MATCH` (BO read through io_uring vs a
  `pread` of the same range into host RAM).

## Two routes, both working

**Route A - the PR's own uapi example** (`xe_drm.h`): allocate a carve-out chunk, mmap it, then a
plain `pread()` with `O_DIRECT` into that mapping. The chunk is page-backed (p2pmem pages inserted
by the `vmf_insert_page_mkwrite()` fault path), so the O_DIRECT read pins those pages and DMAs
straight into VRAM. No io_uring involved. Harness leg: `pread`.

**Route B - the dmabuf-io/io_uring path**: register a **dma-buf** as an io_uring buffer and issue
`IORING_OP_READ_FIXED`. The buffer must be a real dma-buf; the carve-out *chunk* fd is an anon
inode (`anon_inode_create_getfile("xe_p2p", &p2p_chunk_fops, ...)`) and is rejected with `-EINVAL`
by `dma_buf_get()`'s `!is_dma_buf_file()` check - leg `chunkreg` demonstrates exactly that. The
correct buffer is a VRAM BO exported with PRIME:

    GEM_CREATE{placement = 1 << DRM_XE_MEM_REGION_CLASS_VRAM, NEEDS_VISIBLE_VRAM, WC, size = 64 KiB multiple}
    -> DRM_IOCTL_PRIME_HANDLE_TO_FD -> IORING_REGISTER_BUFFERS_UPDATE{EXTENDED, type=IO_REGBUF_TYPE_DMABUF,
       dmabuf_fd = that fd, target_fd = the O_DIRECT block-device fd}

Harness leg: `vrambo`. Registration itself needs: an existing slot (`IORING_REGISTER_BUFFERS2` with an
iovec first, since registration only takes iovecs), `uaddr`/`size` zero, `nr_args =
sizeof(struct io_uring_rsrc_register/update2)`, and reads issued against the *same* file passed as
`target_fd` (`-EBADF` otherwise). For dmabuf buffers the SQE `addr` is an offset *within* the buffer
(the base is 0), while for uaddr buffers it is the absolute user address.

Common constraints: `O_DIRECT` mandatory; only block devices implementing `init_dma_buf_io_ctx`
work (in this tree: NVMe; no dm/md fop exists), so target the raw device at the FIEMAP extents of a
real file; carve-out must be enabled (`p2p_carveout_mb`), chunk size page-aligned and <= the carve-out
(`-ENODEV` when off, `-ENOMEM` when too big, `-EBUSY` while suspending).

## Running it

    ./run-p2p-bench.sh
    DRM=/dev/dri/renderD129 SIZE_MIB=4096 BLOCK_MIB=64 CHUNK_MIB=512 ./run-p2p-bench.sh

Legs: `host` (device -> host RAM baseline), `vrambo` (route B), `verify` (host vs chunk byte compare),
`pread` (route A), `chunkreg` (negative control). Reads only, `--file-off` defaults to 4 GiB. Every leg
in the results above ran as root (`sudo -n`), because the raw block device is `root:disk` and `perf`
needs it. The DRM side does not: `DRM_IOCTL_XE_P2P_ALLOC` is `DRM_RENDER_ALLOW`, so **route A against
a regular file needs no privileges at all** - verified as an unprivileged user (member of the `render`
group) reading a 128 MiB file on the ext4 root with `--bdev <file>`: `LEG pread bytes=134217728
cpu_ms=14`. A non-block target makes the harness fall back to assuming 512-byte logical blocks (it
prints a note).

## Getting the DRAM numbers (AMD uncore perf counters)

The DRAM column comes from the AMD data-fabric counters:

    sudo perf stat -a -M nps1_die_to_dram -- ./p2p_direct_bench ... --leg <leg>

That metric is defined in perf's amdzen3 JSON as the sum of eight `dram_channel_data_controller_N`
events (`"Unit": "DFPMC"`), which only exist when `CONFIG_PERF_EVENTS_AMD_UNCORE` is enabled - it was
off, and `perf list` advertises the metric anyway because the JSON catalog ships inside the perf
binary. It is now enabled in the kernel configs used here (`=m`), and on the running kernel it is live
as a module:

    sudo insmod <kernel-tree>/arch/x86/events/amd/amd-uncore.ko
    ls /sys/bus/event_source/devices/   # amd_df, amd_l3 appear; dmesg: 4 amd_df + 6 amd_l3 counters

Until a rebuilt kernel carries it, that `insmod` must be repeated after each reboot. Note that
`make M=arch/x86/events/amd` builds an *unsigned* module that taints the kernel (`tainted = 12288` =
out-of-tree + unsigned); sign it in place with
`scripts/sign-file sha512 certs/signing_key.pem certs/signing_key.x509 <module>`, or use a full
`make modules` / `make modules_install`, which signs and marks modules properly. Energy is available
too: `perf stat -a -e power/energy-pkg/` returns real joules.

## Artifacts

`results/` holds the raw captures behind the table: `environment.txt`, `throughput-512mib.txt`,
`throughput-4gib.txt`, `throughput-4gib-card1.txt`, and `dram-{host,pread,vrambo}.txt`. See
[`results/README.md`](results/README.md) for how each was produced. Host names, user names, home
paths, email addresses and internal project names were stripped from the captures; hardware models
(CPU, GPU, NVMe controller) were kept because they are what the throughput numbers have to be read
against, with the drive's brand redacted.

## Caveats

- All measurements ran as root (raw block device); the data path does not depend on that, and route A
  against a regular file was verified to work rootless (see above).
- Sparse files lie: a freshly `fallocate`d file has unwritten extents whose reads return zeros without
  touching the disk (the rootless check above measured ~9 GB/s that way). Measure throughput on real
  extents only.
- Reads only, at a fixed raw offset. For fidelity to the author's workload, FIEMAP a real model file
  and feed its physical extents to `--file-off`.
- The PR kernel is built from the PR head with `-march=znver3` via icecc; the machine's previous
  kernel remains available as a boot fallback.
