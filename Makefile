ifeq ($(OS),Windows_NT)
POWERSHELL ?= powershell
PYTHON ?= python
else
PYTHON ?= python3
MINGW_CXX ?= x86_64-w64-mingw32-g++
endif
RUSTC ?= rustc
XORRISO ?= xorriso
QEMU ?= qemu-system-x86_64
OVMF_CODE ?=
OVMF_VARS ?=
ISO ?= build/daneels-ritron.iso
DISK ?= build/data-disk.img

.PHONY: all build iso disk verify qemu clean

all: build

ifeq ($(OS),Windows_NT)
build:
	$(POWERSHELL) -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -RustCompiler "$(RUSTC)"
else
build:
	CXX="$(MINGW_CXX)" RUSTC="$(RUSTC)" bash scripts/build.sh
endif

iso: build
	$(PYTHON) scripts/create_iso.py --bootloader build/EFI/BOOT/BOOTX64.EFI --kernel build/EFI/BOOT/KERNEL.EFI --output "$(ISO)" --xorriso "$(XORRISO)"

disk: build
	$(PYTHON) scripts/create_disk.py --output "$(DISK)" \
		--bootloader build/EFI/BOOT/BOOTX64.EFI \
		--kernel build/EFI/BOOT/KERNEL.EFI

ifeq ($(OS),Windows_NT)
verify: disk
		$(POWERSHELL) -NoProfile -ExecutionPolicy Bypass -File scripts/verify-disk.ps1 -Disk "$(DISK)" -Rustc "$(RUSTC)"

qemu: iso disk
		$(POWERSHELL) -NoProfile -ExecutionPolicy Bypass -File scripts/run-qemu.ps1 -Qemu "$(QEMU)" -Iso "$(ISO)" -Disk "$(DISK)" -OvmfCode "$(OVMF_CODE)" -OvmfVars "$(OVMF_VARS)"

clean:
		$(POWERSHELL) -NoProfile -Command "if (Test-Path -LiteralPath 'build') { Remove-Item -LiteralPath 'build' -Recurse -Force }"
else
verify: disk
		RUSTC="$(RUSTC)" bash scripts/verify-disk.sh "$(DISK)"

qemu: iso disk
		QEMU="$(QEMU)" OVMF_CODE="$(OVMF_CODE)" OVMF_VARS="$(OVMF_VARS)" bash scripts/run-qemu.sh "$(ISO)" "$(DISK)"

clean:
		rm -rf build
endif
