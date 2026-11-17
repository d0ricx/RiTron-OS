# Building and running Daneel's RiTron

**Guides:** [Home](../README.md) · [Architecture](ARCHITECTURE.md) ·
[Shell and filesystem](FILESYSTEM.md) · [Networking](NETWORKING.md)

## Prerequisites

The kernel and bootloader are UEFI PE/COFF applications. Building on Linux
therefore requires MinGW-w64 rather than the host C++ compiler. Both platforms
also need Python 3, GNU Make, Rust, and the Rust target
`x86_64-pc-windows-gnu`. Creating an ISO additionally requires `xorriso`.

There are no C++ or Rust package dependencies fetched from a package registry
for the kernel itself. The build uses platform toolchains and the Rust target
library; the Python image tools use the Python standard library. QEMU and OVMF
are runtime/test dependencies, not build dependencies. Docker packages the
build tools but does not emulate or boot the result.

| Tool | Role | Required for |
| --- | --- | --- |
| GNU Make | Selects and sequences build, image, verification, and launch targets. | All `make` workflows |
| MinGW-w64 x86_64 GCC/G++ | Compiles and links UEFI PE/COFF binaries for Windows GNU ABI. | Kernel/EFI build |
| Rust (`rustc`, `rustup`) | Builds `src/krnl/net/http.rs` as a freestanding static library. | Kernel/EFI build |
| Rust target `x86_64-pc-windows-gnu` | Provides Rust core/runtime artifacts for the EFI target. | Rust client build |
| Python 3 | Creates/updates the FAT32 image and constructs the ISO command. | `disk`, `iso` |
| `xorriso` | Writes the bootable ISO filesystem. | `iso` |
| QEMU and OVMF firmware | Runs the resulting EFI application in a virtual machine. | `qemu` only |

The toolchain versions are not all locked by a dependency lockfile. For
repeatable builds, record the compiler, Rust, Python, GNU Make, and xorriso
versions used. The Dockerfile pins its Rust base image to `1.85.1` and uses
Debian Bookworm packages for the remaining tools; Linux distribution package
versions may still change over time.

Install the Rust target once:

```sh
rustup target add x86_64-pc-windows-gnu
```

### Linux (Debian or Ubuntu)

```sh
sudo apt update
sudo apt install make g++-mingw-w64-x86-64 python3 xorriso rustup
rustup target add x86_64-pc-windows-gnu
```

To run under QEMU, also install `qemu-system-x86` and `ovmf`.

### Windows

Use MSYS2 with its UCRT64 toolchain and install GNU Make, Python, and xorriso:

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc make python xorriso
```

Install Rust with rustup and add the Windows GNU target as shown above. QEMU
and OVMF are needed only to run the OS. If tools are not on `PATH`, Make
variables such as `MINGW_CXX`, `RUSTC`, `XORRISO`, `QEMU`, `OVMF_CODE`, and
`OVMF_VARS` can specify their paths.

## Build on the host

```sh
make             # Build EFI applications
make net-test    # Run host-side Rust networking protocol tests
make disk-test   # Test FAT32 images, EFI boot media, and legacy disk migration
make iso disk    # Also create the bootable ISO and data disk
make verify      # Validate the FAT data disk and stored EFI files
make qemu        # Build, then run with QEMU and OVMF
```

The generated files are placed in `build/`:

- `build/EFI/BOOT/BOOTX64.EFI` — removable-media boot application containing
  the bootloader and statically linked kernel.
- `build/EFI/BOOT/KERNEL.EFI` — standalone kernel EFI image.
- `build/daneels-ritron.iso` — UEFI-bootable ISO.
- `build/data-disk.img` — persistent 64 MiB FAT32 data disk.

The data disk persists between runs. Its on-disk volume identity is retained
for compatibility with existing user disks. `make clean` deletes all of
`build/`, including the data disk and its user files; back it up first if needed.

### Build pipeline

The host build has distinct compilation and image stages:

1. `rustc` compiles the networking module for `x86_64-pc-windows-gnu` as a
   `staticlib` using Rust edition 2021, optimization level 2, position-
   independent code, no red zone, and abort-on-panic behavior.
2. MinGW-w64 compiles the C++ kernel and links that archive into both
   `KERNEL.EFI` and `BOOTX64.EFI`. The build is freestanding, omits the standard
   runtime, exceptions, RTTI, stack protector, and unwind tables, enables
   `-Wall -Wextra -Werror`, and uses `-O2`.
3. `make iso` lays out UEFI boot files in a compact FAT16 El Torito image and
   invokes xorriso to create the ISO. This boot-only image remains FAT16 for
   firmware compatibility; the kernel's persistent filesystem is FAT32-only.
4. `make disk` creates a fixed-geometry FAT32 image or updates the existing
   one, retaining user files while replacing `/ROOT/SYSTEM` EFI binaries. It
   migrates the previous 16 MiB project FAT16 image to FAT32 before replacing it.
5. `make verify` runs a host-side Rust verifier over the data-disk image.

The same kernel sources are linked into two EFI entry-point variants, each
containing the kernel code: `BOOTX64.EFI` uses the removable-media entry point,
while `KERNEL.EFI` uses the alternate EFI entry point. The disk seeder stores
both under `/ROOT/SYSTEM`; the current runtime does not load `KERNEL.EFI` from
disk as a second-stage kernel. Both binaries are independently bootable EFI
applications built from the same kernel sources.

### Make targets

| Target | Inputs / effects |
| --- | --- |
| `make` or `make build` | Compile the Rust archive and EFI binaries into `build/`. |
| `make iso` | Build EFI binaries if needed, then create `build/daneels-ritron.iso`. |
| `make disk` | Build EFI binaries if needed, then create/update `build/data-disk.img`. |
| `make verify` | Create/update the disk, then validate its FAT layout and EFI entries. |
| `make net-test` | Compile and run host-side DNS, TCP sequence, and HTTP parser tests. |
| `make disk-test` | Test FAT32 image geometry, EFI boot media, and legacy migration. |
| `make qemu` | Build ISO and disk, then launch QEMU with OVMF. |
| `make clean` | Delete the complete `build/` directory, including persistent user data. |

## Build with Docker

The Docker image contains only the build toolchain. Source and generated
artifacts stay on the host through the mounted workspace; QEMU and OVMF remain
host-side requirements.

Build the image from the project root:

```sh
docker build -t daneels-ritron-builder .
```

On Linux, build as the current host user to avoid root-owned output files:

```sh
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -v "$PWD:/workspace" \
  -w /workspace \
  daneels-ritron-builder make iso disk
```

On Windows PowerShell:

```powershell
docker run --rm -v "${PWD}:/workspace" -w /workspace daneels-ritron-builder make iso disk
```

The ISO and data disk will appear under the host's `build/` directory. To
compile only the EFI applications or run the disk check, replace `make iso
disk` with `make build` or `make verify`. Docker does not run the OS.

The Compose configuration runs the host-side networking and disk migration
tests, then builds and verifies the boot images in one command:

```sh
docker compose run --build --rm builder
```

GitHub Actions runs this Compose build on pushes, pull requests, and manual
dispatches. Successful runs upload the ISO and persistent data disk as the
`daneels-ritron-boot-images` workflow artifact.

The container is a build environment, not part of the boot chain: it does not
include QEMU/OVMF or pass hardware through to the guest. The current Compose
configuration mounts the repository at `/workspace`, so artifacts stay in the
host's `build/` directory.

## Run in QEMU

With QEMU and OVMF installed on the host:

```sh
make qemu
```

The QEMU window displays the shell; serial diagnostics appear in the terminal.
The launcher uses the `pc` machine because the storage driver currently
supports legacy IDE/ATA PIO. It attaches an e1000 network adapter to QEMU's
user-mode network.

Firmware and executable locations can be supplied explicitly:

```sh
make qemu QEMU=/path/to/qemu-system-x86_64 \
  OVMF_CODE=/path/to/OVMF_CODE.fd \
  OVMF_VARS=/path/to/OVMF_VARS.fd
```

`make verify` checks the FAT32 volume identity and geometry, FAT copies,
directory structure, and stored EFI image signatures. Verification is an
optional host utility and is not required for normal booting.
The verifier rejects FAT16 data disks. FAT16 is also used by the ISO's compact
UEFI El Torito boot image, which is separate from the persistent data volume.
This verification checks the generated disk's structural invariants; it is not
a kernel test suite, runtime filesystem consistency check, EFI signature
verification, or proof of behavior on physical hardware. A successful build
only establishes that the configured toolchain completed the build and image
steps. Use QEMU boot and feature-specific smoke tests to validate runtime
behavior.
