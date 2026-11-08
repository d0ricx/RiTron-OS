#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <iso> <disk>" >&2
    exit 2
fi

iso="$1"
disk="$2"
qemu="${QEMU:-qemu-system-x86_64}"
ovmf_code="${OVMF_CODE:-}"
ovmf_vars="${OVMF_VARS:-}"

if [[ ! -f "$iso" ]]; then
    echo "Bootable ISO not found: $iso" >&2
    exit 1
fi
if [[ ! -f "$disk" ]]; then
    echo "FAT data disk not found: $disk. Run 'make disk' to create it." >&2
    exit 1
fi
if ! command -v "$qemu" >/dev/null 2>&1; then
    echo "QEMU executable not found: $qemu. Set QEMU or add it to PATH." >&2
    exit 1
fi

if [[ -z "$ovmf_code" || -z "$ovmf_vars" ]]; then
    for directory in /usr/share/OVMF /usr/share/ovmf /usr/share/edk2/x64 /usr/share/qemu; do
        [[ -d "$directory" ]] || continue
        for suffix in 4M ""; do
            code="$directory/OVMF_CODE${suffix:+_$suffix}.fd"
            vars="$directory/OVMF_VARS${suffix:+_$suffix}.fd"
            if [[ -f "$code" && -f "$vars" ]]; then
                [[ -n "$ovmf_code" ]] || ovmf_code="$code"
                [[ -n "$ovmf_vars" ]] || ovmf_vars="$vars"
                break 2
            fi
        done
        for prefix in edk2-x86_64 edk2-i386; do
            code="$directory/${prefix}-code.fd"
            vars="$directory/${prefix}-vars.fd"
            if [[ -f "$code" && -f "$vars" ]]; then
                [[ -n "$ovmf_code" ]] || ovmf_code="$code"
                [[ -n "$ovmf_vars" ]] || ovmf_vars="$vars"
                break 2
            fi
        done
    done
fi

if [[ ! -f "$ovmf_code" ]]; then
    echo "OVMF code firmware not found. Set OVMF_CODE to an OVMF_CODE.fd file." >&2
    exit 1
fi
if [[ ! -f "$ovmf_vars" ]]; then
    echo "OVMF variable-store template not found. Set OVMF_VARS to an OVMF_VARS.fd file." >&2
    exit 1
fi

mkdir -p "$(dirname -- "$iso")"
writable_vars="$(dirname -- "$iso")/OVMF_VARS.fd"
cp -- "$ovmf_vars" "$writable_vars"

exec "$qemu" \
    -machine pc \
    -m 256M \
    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
    -drive "if=pflash,format=raw,file=$writable_vars" \
    -drive "if=none,id=bootcd,format=raw,media=cdrom,file=$iso" \
    -device ide-cd,drive=bootcd,bus=ide.1,bootindex=1 \
    -drive "file=$disk,format=raw,if=ide,index=1" \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -boot order=d \
    -serial stdio \
    -monitor none
