# Kernel architecture

**Guides:** [Home](../README.md) · [Build and run](BUILD.md) ·
[Shell and filesystem](FILESYSTEM.md) · [Networking](NETWORKING.md)

## Boot and runtime

The UEFI boot application discovers the graphics and ACPI information needed
by the kernel and prepares a memory map. It then exits UEFI boot services and
passes a `BOOT_INFO` structure to the kernel entry point. After that handoff,
the kernel does not use UEFI services: it initializes its own console,
interrupts, memory manager, input, storage, and networking code.

The implementation is freestanding. C++ exceptions, RTTI, and the standard
library are not used in the kernel. Rust code is compiled as a `staticlib` for
the Windows GNU target and linked into the EFI image; `src/krnl/memory/runtime.cpp`
provides the low-level memory and runtime symbols required by that library.

## Source layout

- `src/boot/` — UEFI entry point, boot protocol, and firmware handoff.
- `src/krnl/arch/` — CPU exceptions, PIC/PIT, and interrupt setup.
- `src/krnl/console/` — framebuffer and serial console implementations.
- `src/krnl/input/` — PS/2 keyboard and shell input protocol.
- `src/krnl/memory/` — physical page bitmap, heap, and compiler/runtime support.
- `src/krnl/net/` — Rust ARP, IPv4, TCP, and HTTP client.
- `src/krnl/power/` — QEMU-compatible poweroff.
- `src/krnl/shell/` — shell, editor, and filesystem command handling.
- `src/drv/` — ATA PIO and Intel e1000 drivers.
- `src/fs/` — FAT16 filesystem.
- `scripts/` — build, image creation, QEMU launch, and disk verification tools.

## Device and platform assumptions

The current QEMU configuration uses x86_64 UEFI, the `pc` (i440FX) machine,
legacy IDE storage, and an Intel e1000 virtual adapter. Hardware support is
purposefully narrow and has not been validated as a general physical-machine
OS. In particular, the ATA driver uses 28-bit LBA PIO, the keyboard uses
PS/2 set-1 scan codes, and the network driver polls descriptors.

## Further reading

- [Build and run](BUILD.md)
- [Shell and FAT16](FILESYSTEM.md)
- [Networking](NETWORKING.md)
