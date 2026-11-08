#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
output_directory="${1:-build}"
compiler="${CXX:-x86_64-w64-mingw32-g++}"
rust_compiler="${RUSTC:-rustc}"
efi_directory="$output_directory/EFI/BOOT"
rust_output_directory="$output_directory/krnl/net"

cd "$project_root"
mkdir -p "$efi_directory"
mkdir -p "$rust_output_directory"

flags=(
    -ffreestanding
    -fno-builtin
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-use-cxa-atexit
    -fno-stack-protector
    -fno-stack-check
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -mno-red-zone
    -fshort-wchar
    -Wall
    -Wextra
    -Werror
    -O2
    -mgeneral-regs-only
    -Isrc/boot
    -Isrc/krnl
    -Isrc/krnl/console
    -Isrc/krnl/input
    -Isrc/krnl/arch
    -Isrc/krnl/memory
    -Isrc/krnl/power
    -Isrc/krnl/shell
    -Isrc/krnl/net
    -Isrc/drv
    -Isrc/fs
)
rust_object="$rust_output_directory/http.a"
"$rust_compiler" \
    --target x86_64-pc-windows-gnu \
    --edition 2021 \
    --crate-type staticlib \
    -C panic=abort \
    -C opt-level=2 \
    -C no-redzone=yes \
    -C relocation-model=pic \
    src/krnl/net/http.rs \
    -o "$rust_object"
linker_flags=(
    -nostdlib
    -Wl,--subsystem,10
    -Wl,--enable-reloc-section
)
common_sources=(
    src/krnl/shell/shell.cpp
    src/fs/fat16.cpp
    src/krnl/console/console.cpp
    src/krnl/input/keyboard.cpp
    src/krnl/arch/cpu.cpp
    src/krnl/memory/memory.cpp
    src/krnl/memory/runtime.cpp
    src/drv/ata.cpp
    src/drv/e1000.cpp
    src/krnl/power/power.cpp
    src/boot/uefi.cpp
    "$rust_object"
)

"$compiler" "${flags[@]}" "${linker_flags[@]}" -Wl,-e,efi_main \
    src/boot/boot.cpp "${common_sources[@]}" \
    -o "$efi_directory/KERNEL.EFI"
"$compiler" "${flags[@]}" "${linker_flags[@]}" -Wl,-e,bootloader_main \
    src/boot/boot.cpp "${common_sources[@]}" \
    -o "$efi_directory/BOOTX64.EFI"

printf 'Built EFI/BOOT/BOOTX64.EFI and EFI/BOOT/KERNEL.EFI in %s\n' "$output_directory"
