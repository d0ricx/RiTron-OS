# Building and running Daneel's RiTron

## Prerequisites

The kernel and bootloader are UEFI PE/COFF applications. Building on Linux
therefore requires MinGW-w64 rather than the host C++ compiler. Both platforms
also need Python 3, GNU Make, Rust, and the Rust target
`x86_64-pc-windows-gnu`. Creating an ISO additionally requires `xorriso`.

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
make iso disk    # Also create the bootable ISO and data disk
make verify      # Validate the FAT16 data disk and stored EFI files
make qemu        # Build, then run with QEMU and OVMF
```

The generated files are placed in `build/`:

- `build/EFI/BOOT/BOOTX64.EFI` — removable-media boot application containing
  the bootloader and statically linked kernel.
- `build/EFI/BOOT/KERNEL.EFI` — standalone kernel EFI image.
- `build/daneels-ritron.iso` — UEFI-bootable ISO.
- `build/data-disk.img` — persistent 16 MiB FAT16 data disk.

The data disk persists between runs. Its on-disk volume identity is retained
for compatibility with existing user disks. `make clean` deletes all of
`build/`, including the data disk and its user files; back it up first if needed.

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

The Compose configuration builds the image and creates and verifies the boot
images in one command:

```sh
docker compose run --build --rm builder
```

GitHub Actions runs this Compose build on pushes, pull requests, and manual
dispatches. Successful runs upload the ISO and persistent data disk as the
`daneels-ritron-boot-images` workflow artifact.

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

`make verify` checks the FAT16 volume identity and geometry, FAT copies,
directory structure, and stored EFI image signatures. It is an optional host
utility and is not required for normal booting.
