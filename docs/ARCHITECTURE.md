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

## Boot protocol and ownership transition

The loader and kernel are built as x86_64 UEFI PE/COFF applications. The boot
application is entered through `bootloader_main` (with `efi_main` retained as
an alternate entry point); both call the same boot routine. The loader:

1. Locates a Graphics Output Protocol (GOP) handle and reads the active mode.
   The handoff includes framebuffer base and size, pixel dimensions, scan-line
   stride, pixel format, and channel masks for the bit-mask format.
2. Finds the ACPI 2.0 configuration-table entry when available, falling back
   to ACPI 1.0. The value is an ACPI Root System Description Pointer (RSDP),
   not the XSDT/RSDT itself. It is carried in `BOOT_INFO`; the current power
   implementation does not parse or use the ACPI tables.
3. Allocates storage for the UEFI memory map, obtains the map and its key, then
   calls `ExitBootServices`. Because the map key is invalidated by allocations,
   the loader retries the map/exit sequence up to three times when firmware
   reports the stale-key status.
4. Calls `kernel_main(&BootInfo)` only after a successful exit. UEFI boot
   services must not be called after this transition.

`BOOT_INFO` is a versioned C structure. The framebuffer initializer checks its
magic value, version, and structure size; the memory initializer separately
validates the memory-map pointer, byte length, descriptor stride, and descriptor
version. Its physical addresses are used directly: this implementation relies
on the firmware's identity-accessible addresses and does not install a new
virtual-address mapping. The structure carries the memory-map byte length and
descriptor stride separately; descriptor iteration advances by the
firmware-provided stride rather than assuming descriptors are tightly packed
at the kernel's compile-time structure size.

### Initialization order

The order in `kernel_main` is significant:

1. Initialize serial diagnostics.
2. Validate the boot memory map and initialize the physical-page bitmap.
3. Exercise the heap with a small allocation/free before depending on it.
4. Install exception handlers while interrupts are disabled.
5. Initialize framebuffer output from the GOP data.
6. Initialize the PS/2 keyboard controller and queue.
7. Remap the PIC, program the PIT, install IRQ handlers, then enable interrupts.
8. Initialize emulator-compatible power-off support, attempt to identify and
   mount the FAT32 disk, initialize the e1000 device, and attempt DHCP.
9. Enter the interactive shell even if storage or network initialization fails;
   those features report unavailable rather than preventing the shell from
   starting.

Serial is initialized first so later failures can be diagnosed without a
working framebuffer.

## Physical memory and heap

The page allocator represents physical memory with one bit per 4 KiB page:

- The bitmap has 1,048,576 bits (128 KiB), which covers a 4 GiB physical
  address range.
- Addresses below 1 MiB are not managed. The allocator also cannot return
  pages at or above 4 GiB.
- Initialization marks only UEFI conventional-memory descriptors as free.
  Descriptor intervals are clipped to the supported range and rounded inward
  to full pages; firmware-reserved and partially overlapping pages remain
  unavailable.
- A first-fit scan finds contiguous page runs. Single-page requests back the
  physical page API; larger runs extend the heap.

The heap is a linked list of blocks stored inside allocated page runs.
Requests are rounded up to 16-byte boundaries. Allocation uses first fit and
splits a block only when enough space remains for a header and an aligned
payload. Release coalesces physically adjacent free blocks, but separately
allocated page runs are not merged. The allocator is a simple single-kernel
allocator: it has no virtual-memory layer, guard pages, per-process heaps, or
SMP synchronization.

## Exceptions, interrupts, and time

The IDT contains 256 entries. CPU exception vectors 0–31 have dedicated
handlers; the remaining entries initially point to a fail-stop fallback. An
exception is written to COM1 with its vector, error code, and instruction
pointer; page faults additionally report CR2. There is no exception recovery
or process isolation, so handlers disable interrupts and halt.

The legacy dual 8259 PIC is remapped so hardware IRQ vectors begin at 0x20 and
0x28, above the CPU exception range. The PIT channel 0 uses divisor 11,932,
which is approximately 100 interrupts per second from the conventional PIT
input clock. The timer only supplies coarse delay ticks; it is not a
high-resolution clock or scheduler. IRQ0 increments the tick counter and IRQ1
feeds keyboard scan codes into a 64-slot ring buffer. One slot distinguishes
full from empty, so at most 63 key events can be queued; input arriving while
full is dropped.

## Console and input paths

The text console renders glyphs into the UEFI-provided framebuffer, respecting
the reported pixels-per-scan-line stride and pixel format. COM1 is a separate
diagnostic stream. The kernel does not rely on UEFI's text input/output
services after the boot-services exit.

The keyboard path is:

```text
PS/2 controller -> IRQ1 -> set-1 scan-code translation
                 -> 64-slot key queue -> shell/editor polling loop
```

The translation tables target a US set-1 layout and the code handles the
modifier and navigation keys needed by the shell/editor. It is not a general
keyboard-layout or Unicode input subsystem.

## Hardware paths and bounds

### ATA

The storage driver probes the primary and secondary legacy IDE channels and
their master/slave positions using IDENTIFY. It accepts a device only when
its first sector has the expected FAT boot signature, volume label, and volume
ID. This protects the data-disk selection from depending on enumeration order.
Reads and writes use one 512-byte sector per ATA PIO command, with the
28-bit LBA range enforced and bounded polling for device status. A cache-flush
command is exposed to the filesystem for explicit write ordering. There is no
DMA, AHCI, GPT/partition discovery, or 48-bit LBA support.

Power-off is also emulator-specific: the kernel writes the common QEMU and
Bochs shutdown values to I/O ports `0x604` and `0xB004`. It does not yet parse
the ACPI RSDP/FADT or implement standards-based ACPI power management; the
RSDP is only preserved in the boot handoff.

### Intel e1000

PCI discovery scans bus 0 and recognizes a limited set of Intel e1000-family
device IDs. The driver configures a memory-mapped BAR, reads the MAC from
registers or EEPROM, and uses statically allocated descriptor rings:

- Eight RX and eight TX descriptors.
- RX buffers are 2,048 bytes each; TX frames are capped at 1,514 bytes.
- Descriptor and buffer addresses must be below 4 GiB because only low DMA
  base registers are programmed.
- Transmission waits for descriptor completion; reception polls the ring.
  This is synchronous polling, not an interrupt-driven network stack.

The driver configures and exposes Ethernet frames and performs DHCP. Higher
network layers are implemented separately in Rust. `dns.rs`, `tcp.rs`, and
`http_codec.rs` hold protocol parsing/building and sequence helpers that can be
tested on the host; `http.rs` connects them to the synchronous e1000 frame API.

## Source layout

- `src/boot/` — UEFI entry point, boot protocol, and firmware handoff.
- `src/krnl/arch/` — CPU exceptions, PIC/PIT, and interrupt setup.
- `src/krnl/console/` — framebuffer and serial console implementations.
- `src/krnl/input/` — PS/2 keyboard and shell input protocol.
- `src/krnl/memory/` — physical page bitmap, heap, and compiler/runtime support.
- `src/krnl/net/` — Rust DNS, ARP/IPv4/TCP frame handling, and HTTP/1.1 codec/client.
- `src/krnl/power/` — QEMU-compatible poweroff.
- `src/krnl/shell/` — shell, editor, and filesystem command handling.
- `src/drv/` — ATA PIO and Intel e1000 drivers.
- `src/fs/` — FAT32 filesystem implementation.
- `scripts/` — build, image creation, QEMU launch, and disk verification tools.

## Device and platform assumptions

The current QEMU configuration uses x86_64 UEFI, the `pc` (i440FX) machine,
legacy IDE storage, and an Intel e1000 virtual adapter. Hardware support is
purposefully narrow and has not been validated as a general physical-machine
OS. In particular, the ATA driver uses 28-bit LBA PIO, the keyboard uses
PS/2 set-1 scan codes, and the network driver polls descriptors.

The kernel is single-address-space and single-CPU in its current design. It
has no scheduler, user/kernel process boundary, syscall ABI, virtual-memory
manager, SMP support, or recovery mechanism. “Network ready” means the
supported device initialized and DHCP configured IPv4; it does not imply
general Internet compatibility. These constraints are architectural, not
configuration switches.

## Further reading

- [Build and run](BUILD.md)
- [Shell and FAT32](FILESYSTEM.md)
- [Networking](NETWORKING.md)
