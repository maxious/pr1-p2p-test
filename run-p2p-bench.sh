#!/bin/bash
set -uo pipefail
BIN="${BIN:-$(cd "$(dirname "$0")" && pwd)/p2p_direct_bench}"
DRM="${DRM:-/dev/dri/renderD128}"
BDEV="${BDEV:-/dev/nvme0n1}"
SIZE_MIB="${SIZE_MIB:-2048}"
BLOCK_MIB="${BLOCK_MIB:-32}"
CHUNK_MIB="${CHUNK_MIB:-256}"
FILE_OFF="${FILE_OFF:-4294967296}"
SUDO="${SUDO:-sudo -n}"
ARGS="$BIN --drm $DRM --bdev $BDEV --size-mib $SIZE_MIB --block-mib $BLOCK_MIB --chunk-mib $CHUNK_MIB --file-off $FILE_OFF"

echo "== kernel: $(uname -r)"
echo "== device: $BDEV render=$DRM size=${SIZE_MIB}MiB block=${BLOCK_MIB}MiB chunk=${CHUNK_MIB}MiB off=$FILE_OFF"
echo
echo "-- 1. baseline: device -> host RAM (io_uring READ_FIXED, O_DIRECT)"
$SUDO $ARGS --leg host
echo
echo "-- 2. PR route B: device -> VRAM BO dma-buf (io_uring registered dmabuf buffer)"
$SUDO $ARGS --leg vrambo
echo
echo "-- 3. correctness: same range read into host RAM and into VRAM, byte compare"
$SUDO $ARGS --leg verify
echo
echo "-- 4. PR route A: pread() straight into the carve-out chunk mapping"
$SUDO $ARGS --leg pread
echo
echo "-- 5. negative control: the chunk fd is an anon inode, so registering it as a dmabuf must fail"
$SUDO $ARGS --leg chunkreg
echo
echo "-- 6. carve-out state"
ls -l /sys/bus/pci/devices/*/p2pmem/ 2>/dev/null || echo "no p2pmem sysfs entries"
