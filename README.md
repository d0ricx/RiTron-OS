# Daneel's RiTron

Daneel's RiTron is an experimental x86_64 operating system that boots through
UEFI into a freestanding C++ kernel. The project includes kernel-owned display,
keyboard, memory, ATA storage, and e1000 networking drivers; a FAT16 filesystem;
an interactive shell; and a small Rust implementation of an IPv4/HTTP client.

This is an early development project, not a general-purpose OS. It currently
targets QEMU and legacy IDE/e1000 devices.

## Get started

Build the EFI applications, bootable ISO, and persistent FAT16 data disk:

```sh
make iso disk
```

Run the OS in QEMU (requires QEMU and OVMF firmware):

```sh
make qemu
```

For prerequisites, Linux and Windows setup, Docker build instructions, and
output details, see [Building and running Daneel's RiTron](docs/BUILD.md).

Community contributions and vulnerability reports are covered by
[CONTRIBUTING.md](CONTRIBUTING.md), [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md),
and [SECURITY.md](SECURITY.md).

## Guides

- [Kernel architecture and source layout](docs/ARCHITECTURE.md)
- [Shell and FAT16 filesystem](docs/FILESYSTEM.md)
- [Networking and HTTP client](docs/NETWORKING.md)

## Key features

- UEFI bootloader and freestanding C++ kernel
- GOP framebuffer console, interrupt-driven PS/2 keyboard, PIT/PIC timer, and
  basic page allocator and heap
- ATA PIO data disk and FAT16 with ASCII long filenames, directories, file
  copy/move, and ordered file replacement
- Interactive shell with command history, cursor editing, quoting, and Tab
  completion
- e1000 Ethernet with DHCP and a Rust-based plain HTTP/1.0 GET client
- Host utility to verify the generated FAT16 disk and stored EFI images

See the guides above for supported commands, limitations, and implementation
details. The OS is experimental: back up `build/data-disk.img` if its contents
matter. `make clean` removes the entire `build/` directory, including that
persistent disk.
