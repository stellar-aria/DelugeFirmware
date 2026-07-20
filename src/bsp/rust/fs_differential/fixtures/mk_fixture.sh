#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; tree="$here/tree"; out="${1:-$here}"
# FAT32: >= 2.5 GB / 32 KB clusters (matches src/bsp/rust/src/sd_image.rs)
truncate -s 2560M "$out/fat32.img"
mformat -F -c 64 -i "$out/fat32.img" ::
mcopy -s -i "$out/fat32.img" "$tree"/* ::
# FAT16: 64 MB, let mtools choose FAT16
truncate -s 64M "$out/fat16.img"
mformat -c 4 -i "$out/fat16.img" ::
mcopy -s -i "$out/fat16.img" "$tree"/* ::
echo "wrote $out/fat16.img $out/fat32.img"
