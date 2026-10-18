#!/bin/bash
set -uo pipefail
echo "== kernel: $(uname -r)"
echo "== expected: 7.3.0-rc7-xe-perf-pr1-p2p"
echo
echo "== xe carve-out param:"
for p in /sys/module/xe/parameters/p2p_carveout_mb /sys/module/xe/parameters/vram_bar_size; do
	[ -e "$p" ] && echo "   $(basename $p) = $(cat $p)" || echo "   $(basename $p) = MISSING (xe not loaded, or not this kernel)"
done
echo
echo "== p2pmem sysfs entries (registration worked if > 0):"
ls -d /sys/bus/pci/devices/*/p2pmem 2>/dev/null || echo "   none"
echo
echo "== p2p-related dmesg:"
sudo -n dmesg | grep -iE 'p2p|carve|DMABUF' | tail -8 || echo "   (nothing)"
echo
echo "== uapi present in the running kernel (EINVAL = missing, ENODEV = carve-out off):"
modinfo xe 2>/dev/null | grep -c p2p_carveout_mb
echo
exec "$(cd "$(dirname "$0")" && pwd)/run-p2p-bench.sh"
