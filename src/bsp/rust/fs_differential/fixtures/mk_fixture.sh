#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; tree="$here/tree"; out="${1:-$here}"
# A deterministic multi-cluster file (1 MiB) so the Lens-1 transfer-overhead
# proxy (tests/efatfs_core.rs) exercises a real cluster chain — the small tree
# fixtures are all <= 1 cluster. Content is a fixed byte (0x55); only its
# length/cluster-count matters. Generated (not committed) to keep the repo lean.
big="$(mktemp)"; trap 'rm -f "$big"' EXIT
head -c 1048576 /dev/zero | tr '\0' '\125' > "$big"
# FAT32: >= 2.5 GB / 32 KB clusters (matches src/bsp/rust/src/sd_image.rs)
truncate -s 2560M "$out/fat32.img"
mformat -F -c 64 -i "$out/fat32.img" ::
mcopy -s -i "$out/fat32.img" "$tree"/* ::
mcopy -i "$out/fat32.img" "$big" ::/SAMPLES/big_multicluster.bin
# FAT16: 64 MB, let mtools choose FAT16
truncate -s 64M "$out/fat16.img"
mformat -c 4 -i "$out/fat16.img" ::
mcopy -s -i "$out/fat16.img" "$tree"/* ::
mcopy -i "$out/fat16.img" "$big" ::/SAMPLES/big_multicluster.bin
echo "wrote $out/fat16.img $out/fat32.img"
