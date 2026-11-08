POWERSHELL ?= powershell
PYTHON ?= python
RUSTC ?= rustc
XORRISO ?= xorriso
QEMU ?= qemu-system-x86_64
OVMF_CODE ?=
OVMF_VARS ?=
ISO ?= build/c-kernel.iso
DISK ?= build/data-disk.img

.PHONY: all build iso disk verify qemu clean

all: build

build:
	$(POWERSHELL) -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1

iso: build
	$(PYTHON) scripts/create_iso.py --bootloader build/EFI/BOOT/BOOTX64.EFI --kernel build/EFI/BOOT/KERNEL.EFI --output "$(ISO)" --xorriso "$(XORRISO)"

disk: build
	$(PYTHON) scripts/create_disk.py --output "$(DISK)" \
		--bootloader build/EFI/BOOT/BOOTX64.EFI \
		--kernel build/EFI/BOOT/KERNEL.EFI

verify: disk
	$(POWERSHELL) -NoProfile -ExecutionPolicy Bypass -File scripts/verify-disk.ps1 -Disk "$(DISK)" -Rustc "$(RUSTC)"

qemu: iso disk
	$(POWERSHELL) -NoProfile -ExecutionPolicy Bypass -File scripts/run-qemu.ps1 -Qemu "$(QEMU)" -Iso "$(ISO)" -Disk "$(DISK)" -OvmfCode "$(OVMF_CODE)" -OvmfVars "$(OVMF_VARS)"

clean:
	$(POWERSHELL) -NoProfile -Command "if (Test-Path -LiteralPath 'build') { Remove-Item -LiteralPath 'build' -Recurse -Force }"
