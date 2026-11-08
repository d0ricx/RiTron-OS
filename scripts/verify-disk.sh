#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
disk="${1:-build/data-disk.img}"
rustc="${RUSTC:-rustc}"
output="$project_root/build/verify-disk"

if [[ ! -f "$disk" ]]; then
    echo "FAT16 data disk not found: $disk" >&2
    exit 1
fi
if ! command -v "$rustc" >/dev/null 2>&1; then
    echo "Rust is required for verification. Install rustc or set RUSTC." >&2
    exit 1
fi

mkdir -p "$project_root/build"
"$rustc" --edition=2021 -O "$project_root/scripts/verify.rs" -o "$output"
"$output" "$disk"
