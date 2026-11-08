# UEFI C++ kernel

This project builds an x86_64 UEFI bootloader and a freestanding C++ kernel.
The loader collects framebuffer, ACPI, and memory-map information, then exits
UEFI boot services before entering the kernel. The kernel owns the display,
keyboard, disk I/O, serial logging, and QEMU poweroff instead of calling UEFI
services while running.

This is an early OS development project, not a general-purpose operating
system. It still relies on UEFI for the initial boot and GOP framebuffer
handoff.

## Features

- UEFI x86_64 bootable ISO
- Freestanding C++ kernel and FAT16 filesystem implementation
- Kernel-owned GOP framebuffer console, polled PS/2 keyboard, and ATA PIO disk driver
- Interactive text shell with a protected OS directory and writable user area
- Persistent 16 MiB FAT16 QEMU data disk
- Bootloader and kernel binaries stored in the read-only system directory
- Kernel-owned COM1 serial logging
- `quit` command that requests QEMU-compatible ACPI shutdown
- Rust host utility that checks the data disk and its stored EFI images

## Project layout

- `src\boot\boot.cpp`, `src\boot\uefi.cpp` — UEFI entry point, boot-info
  collection, and `ExitBootServices` handoff.
- `src\krnl\krnl.cpp` — shell, editor, and filesystem command handling.
- `src\krnl\console.cpp`, `src\krnl\keyboard.cpp` — framebuffer/serial
  console and polled PS/2 keyboard drivers.
- `src\drv\ata.cpp`, `src\krnl\power.cpp` — ATA PIO storage and native
  QEMU-compatible poweroff.
- `src\fs\fat16.cpp`, `src\fs\fat16.h` — FAT16 volume and directory operations.
- `scripts\build.ps1` — builds the EFI applications with MinGW-w64 G++.
- `scripts\create_iso.py` — creates the UEFI-bootable ISO.
- `scripts\create_disk.py` — creates or updates the FAT16 virtual disk,
  installs the EFI binaries, and preserves user files.
- `scripts\run-qemu.ps1` — locates OVMF, configures QEMU, and runs the ISO.
- `scripts\verify.rs` — Rust FAT16 and EFI-image verifier.
- `scripts\verify-disk.ps1` — compiles and runs the Rust verifier.
- `Makefile` — build, ISO, data disk, QEMU, and cleanup targets.
- `build\` — generated EFI files, ISO, persistent data disk, and firmware
  variables.

## Requirements

- GNU Make
- x86_64 MinGW-w64 G++ (for example, MSYS2 UCRT64)
- Python 3
- `xorriso` to create the bootable ISO
- QEMU and OVMF firmware to run the emulator
- Rust is optional; install `rustc` to use `make verify`

For MSYS2, install `xorriso` with:

```sh
pacman -S xorriso
```

The ISO script checks common MSYS2 installation paths if `xorriso` is not on
`PATH`. QEMU and OVMF are detected from common locations; custom executable
and firmware paths can be supplied as Make variables.

## Build

Build the EFI applications:

```powershell
make
```

The output is in `build\EFI\BOOT`:

- `BOOTX64.EFI` is the UEFI removable-media boot application. It contains the
  bootloader and statically linked kernel.
- `KERNEL.EFI` is a standalone EFI image containing the kernel.

The sources are compiled as freestanding C++ with the standard library,
exceptions, and RTTI disabled. The kernel uses UEFI only during the initial
boot handoff; keyboard input, display output, disk access, and shutdown are
handled by kernel-owned drivers after `ExitBootServices`.

## Create a bootable ISO

```powershell
make iso
```

This produces `build\c-kernel.iso`. The ISO contains an El Torito UEFI boot
image with the EFI applications. Python's standard library creates the FAT
boot image; `xorriso` packages it into the ISO.

To select a different output path or `xorriso` executable:

```powershell
make iso ISO=dist\my-kernel.iso XORRISO=C:\tools\xorriso.exe
```

## Verify the data disk

After creating or updating the persistent FAT16 disk, run:

```powershell
make verify
```

This compiles and runs the Rust verifier. It checks the FAT16 geometry and
copies, volume identity, `/ROOT/SYSTEM` and `/ROOT/USR`, and the PE signatures
of the stored `BOOTX64.EFI` and `KERNEL.EFI` files. Rust is needed only for
this optional check; normal image creation uses Python's standard library.

## Run in QEMU

```powershell
make qemu
```

This builds the ISO and EFI applications, creates or updates the data disk,
copies the OVMF variable-store template to a writable `build\OVMF_VARS.fd`,
and launches QEMU.
The QEMU window shows the shell. Kernel serial output is sent to the terminal
running `make qemu`.
QEMU uses the `pc` (i440FX) machine because the native disk driver currently
supports legacy IDE/ATA PIO. The bootloader uses UEFI GOP to pass the
framebuffer to the kernel, which then draws the text console itself.

If QEMU or OVMF is installed outside the locations searched by the launcher,
specify the paths:

```powershell
make qemu QEMU=C:\tools\qemu\qemu-system-x86_64.exe OVMF_CODE=C:\firmware\OVMF_CODE.fd OVMF_VARS=C:\firmware\OVMF_VARS.fd
```

The QEMU disk image is `build\data-disk.img`. It is a virtual disk file inside
the project build directory; it does not write to a host physical disk. The
kernel mounts only the project's 16 MiB FAT16 volume, identified by its
`KERNELDATA` label and volume ID. The shell's filesystem root is `/ROOT`;
the underlying FAT volume is hidden from shell commands.

The filesystem layout is:

```text
/ROOT/
  SYSTEM/
    BOOTX64.EFI
    KERNEL.EFI
  USR/
    (your files and directories)
```

`SYSTEM` is read-only through the OS shell. It contains the built EFI
bootloader and kernel and is refreshed by `make disk` / `make qemu`. The
shell starts in `/ROOT/USR`, which is writable. Existing files in an older
data disk's volume root are moved into `/ROOT/USR` the first time `/ROOT` is
created; their file contents are preserved.

The data disk persists across runs. `make disk` creates it if missing and
updates only the OS binaries under `/ROOT/SYSTEM` when it already exists;
files in `/ROOT/USR` are preserved. Back up the image if its contents matter.
`make clean` deletes the entire `build\` directory, including the persistent
disk and its contents.

## Shell commands

Commands are entered in the QEMU window at the `>> ` prompt. The shell starts
at `/ROOT/USR`; use `pwd` to see the current path and `cd ..` to reach `/ROOT`.
Use `help` to print the command list.

| Command | Description |
| --- | --- |
| `help` | Show available commands. |
| `ls` | List entries in the current directory. Directories have a trailing `/`. |
| `pwd` | Print the current directory path. |
| `cd <path>` | Change to a directory using a relative or absolute path. |
| `cd ..` | Move to the parent directory. At `/ROOT`, remain there. |
| `cd /` | Return to `/ROOT`. |
| `mkdir <path>` | Create a directory, including under a nested path. |
| `rmdir <path>` | Remove an empty directory. |
| `rmdir -rf <path>` | Recursively remove a directory and its contents. `/ROOT/SYSTEM` cannot be removed. |
| `mkfile <path>` | Create a new empty file. |
| `cat <file>` | Print a text file's contents. |
| `rm <file>` | Delete a file. Directories must be removed with `rmdir`. |
| `open <file>` | Open a file in the built-in text editor. If the file does not exist, it starts a new unsaved file. |
| `quit` | Request native QEMU-compatible poweroff. |

Paths accept `/` or `\` separators and FAT 8.3 names. Relative paths start at
the current directory; absolute paths start at `/ROOT`. Both `/ROOT/USR` and
`/USR` refer to the same directory. For example, from `/ROOT/USR`:

```text
mkdir NOTES
open NOTES/TODO.TXT
cat NOTES/TODO.TXT
cd /ROOT/USR/NOTES
rm TODO.TXT
cd /
```

`cat` supports text files up to 64 KiB; non-printable bytes are shown as `?`.
Paths cannot navigate above `/ROOT`, and the `/ROOT/SYSTEM` directory remains
read-only for create, edit, and delete operations.

File and directory names use FAT 8.3 short-name rules: up to 8 characters,
optionally followed by a period and up to 3 extension characters. Names are
case-insensitive and displayed in uppercase. `mkfile` creates an empty file;
use `open` to read or edit its contents. New files are created on disk when
you save them for the first time.

### Text editor

`open <filename>` clears the console and opens the file in the editor. Type
printable ASCII text; Enter inserts a newline, Tab inserts a tab, and
Backspace/Delete remove text. The arrow keys move the cursor; Home and End
move to the start or end of the current line. CRLF and CR line endings are
normalized to LF when a file is opened.

The shortcut bar at the bottom of the screen shows the key bindings:

- **Ctrl+S** saves the file to the FAT16 disk.
- **Ctrl+X** closes the editor. Unsaved changes are discarded.
- **Tab** inserts a tab character.

The editor can edit files up to 64 KiB. Files larger than this remain on disk,
but cannot be opened in the editor. Files under `/ROOT/SYSTEM` cannot be
opened for editing or saved; use `/ROOT/USR` for writable files.

`rmdir -rf` is destructive: it removes the named directory and all entries
under it from the virtual data disk. It refuses to remove `/ROOT/SYSTEM`, the
filesystem root, or the current directory and its parents.

## Current limitations

- The ATA driver currently supports legacy 28-bit LBA PIO disks and identifies
  the data disk by its FAT16 volume ID and label; AHCI and other controllers
  are not supported.
- The PS/2 keyboard driver uses polled set-1 scan codes; USB-only keyboards
  and interrupt-driven input are not supported.
- The poweroff sequence targets QEMU's ACPI/Bochs-compatible power ports.
- The filesystem supports FAT16 directory and file content I/O, but not long
  filenames or arbitrary mounted volumes.
- The text editor currently supports printable ASCII rather than Unicode.
- The QEMU data disk is 16 MiB. A filesystem full of entries or clusters will
  report that no space is available.
- The shell does not currently have pipelines, quoting, command history, or
  file completion.

## Cleanup

```powershell
make clean
```

This removes generated build products and the persistent QEMU data disk.
