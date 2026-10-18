# Results captures (2026-10-18)

Raw command output behind the table in the top-level README. Nothing here is hand-written.

| file | produced by |
| --- | --- |
| `environment.txt` | kernel release, carve-out dmesg lines, `xe` module params, `p2pmem` enumeration, perf PMU list, DRAM-metric check, CPU/GPU/NVMe identification |
| `throughput-512mib.txt` | `SIZE_MIB=512 BLOCK_MIB=64 CHUNK_MIB=128 ./run-p2p-bench.sh` |
| `throughput-4gib.txt` | `SIZE_MIB=4096 BLOCK_MIB=64 CHUNK_MIB=512 ./run-p2p-bench.sh` |
| `throughput-4gib-card1.txt` | `DRM=/dev/dri/renderD129 SIZE_MIB=4096 BLOCK_MIB=64 CHUNK_MIB=512 ./run-p2p-bench.sh` |
| `dram-host.txt`, `dram-pread.txt`, `dram-vrambo.txt` | `sudo perf stat -a -M nps1_die_to_dram -- ./p2p_direct_bench --drm /dev/dri/renderD128 --bdev /dev/nvme0n1 --size-mib 4096 --block-mib 64 --chunk-mib 512 --file-off 4294967296 --leg <leg>` |

Machine: kernel `7.3.0-rc7-xe-perf-pr1-p2p` (PR head built locally), 2x Intel Arc Pro B60
(`8086:e211`), a MAP1602-class DRAM-less PCIe 4.0 NVMe, AMD Ryzen 7 5700X3D, P2P carve-out
`p2p_carveout_mb=2048` per tile.

How to read them:

- Each `LEG` line is `bytes wall GBps cpu_ms maxrss_kb`, one line per leg.
- `VERIFY MATCH` is a byte comparison of two independent reads of the same range (host RAM vs VRAM),
  so it is the correctness evidence, not a throughput figure.
- `EXPECTED-FAIL register chunk fd as dmabuf: Invalid argument` is the intended negative control: the
  carve-out chunk fd is an anon inode, not a dma-buf, so it must be rejected.
- In the `dram-*.txt` files the `nps1_die_to_dram` line is the DRAM bytes moved during the whole run,
  summed over the data-fabric channels; the per-channel `dram_channel_data_controller_N` lines are the
  raw counters.

Sanitization: host names, user names, home paths, email addresses and internal project names were
stripped from these captures, and the SSD's brand was redacted. Hardware models (CPU, GPU, NVMe
controller) and PCI addresses were kept, because throughput numbers cannot be read without them;
device serials were not captured.
