import argparse
import math
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

SECTOR_SIZE = 512
ROOT_ENTRY_COUNT = 512
FAT16_MIN_CLUSTERS = 8192
FAT16_MAX_CLUSTERS = 65524
END_OF_CHAIN = 0xFFFF

def directory_entry(name, extension, attributes, first_cluster, size=0):
    entry = bytearray(32)
    entry[0:8] = name.ljust(8, b" ")
    entry[8:11] = extension.ljust(3, b" ")
    entry[11] = attributes
    struct.pack_into("<H", entry, 20, first_cluster >> 16)
    struct.pack_into("<H", entry, 26, first_cluster & 0xFFFF)
    struct.pack_into("<I", entry, 28, size)
    return entry

def fat16_image(bootloader_path, kernel_path):
    files = [
        (b"BOOTX64", b"EFI", bootloader_path.read_bytes()),
        (b"KERNEL", b"EFI", kernel_path.read_bytes()),
    ]
    if any(not contents for _, _, contents in files):
        raise ValueError("EFI images must not be empty")

    for sectors_per_cluster in (1, 2, 4, 8, 16, 32, 64):
        clusters_per_file = [
            math.ceil(len(contents) / (SECTOR_SIZE * sectors_per_cluster))
            for _, _, contents in files
        ]
        cluster_count = max(FAT16_MIN_CLUSTERS, 3 + sum(clusters_per_file))
        if cluster_count <= FAT16_MAX_CLUSTERS:
            break
    else:
        raise ValueError("EFI files are too large for the generated FAT16 image")

    fat_sectors = math.ceil((cluster_count + 2) * 2 / SECTOR_SIZE)
    root_sectors = ROOT_ENTRY_COUNT * 32 // SECTOR_SIZE
    data_start = 1 + 2 * fat_sectors + root_sectors
    total_sectors = data_start + cluster_count * sectors_per_cluster
    image = bytearray(total_sectors * SECTOR_SIZE)

    boot_sector = memoryview(image)[:SECTOR_SIZE]
    boot_sector[0:3] = b"\xeb\x3c\x90"
    boot_sector[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", boot_sector, 11, SECTOR_SIZE)
    boot_sector[13] = sectors_per_cluster
    struct.pack_into("<H", boot_sector, 14, 1)
    boot_sector[16] = 2
    struct.pack_into("<H", boot_sector, 17, ROOT_ENTRY_COUNT)
    struct.pack_into("<H", boot_sector, 19, total_sectors if total_sectors <= 0xFFFF else 0)
    boot_sector[21] = 0xF8
    struct.pack_into("<H", boot_sector, 22, fat_sectors)
    struct.pack_into("<H", boot_sector, 24, 63)
    struct.pack_into("<H", boot_sector, 26, 255)
    struct.pack_into("<I", boot_sector, 32, total_sectors)
    boot_sector[36] = 0x80
    boot_sector[38] = 0x29
    struct.pack_into("<I", boot_sector, 39, 0x434B4552)
    boot_sector[43:54] = b"C KERNEL   "
    boot_sector[54:62] = b"FAT16   "
    boot_sector[510:512] = b"\x55\xaa"

    fat = bytearray(fat_sectors * SECTOR_SIZE)
    struct.pack_into("<HH", fat, 0, 0xFFF8, END_OF_CHAIN)
    struct.pack_into("<HH", fat, 2 * 2, END_OF_CHAIN, END_OF_CHAIN)
    next_cluster = 4
    file_chains = []
    for cluster_count_for_file in clusters_per_file:
        chain = list(range(next_cluster, next_cluster + cluster_count_for_file))
        for current, following in zip(chain, chain[1:] + [END_OF_CHAIN]):
            struct.pack_into("<H", fat, current * 2, following)
        next_cluster += cluster_count_for_file
        file_chains.append(chain)

    for fat_index in range(2):
        start = (1 + fat_index * fat_sectors) * SECTOR_SIZE
        image[start : start + len(fat)] = fat

    root_start = (1 + 2 * fat_sectors) * SECTOR_SIZE
    root = directory_entry(b"EFI", b"", 0x10, 2)
    image[root_start : root_start + len(root)] = root

    cluster_size = SECTOR_SIZE * sectors_per_cluster

    def write_cluster(cluster, contents):
        offset = (
            data_start + (cluster - 2) * sectors_per_cluster
        ) * SECTOR_SIZE
        image[offset : offset + cluster_size] = contents.ljust(cluster_size, b"\0")

    write_cluster(2, directory_entry(b".", b"", 0x10, 2) +
                  directory_entry(b"..", b"", 0x10, 0) +
                  directory_entry(b"BOOT", b"", 0x10, 3))
    boot_directory = bytearray(cluster_size)
    boot_directory[0:32] = directory_entry(b".", b"", 0x10, 3)
    boot_directory[32:64] = directory_entry(b"..", b"", 0x10, 2)

    for index, ((name, extension, contents), chain) in enumerate(
        zip(files, file_chains)
    ):
        for chunk_index, cluster in enumerate(chain):
            start = chunk_index * cluster_size
            write_cluster(cluster, contents[start : start + cluster_size])
        entry_offset = 64 + index * 32
        boot_directory[entry_offset : entry_offset + 32] = directory_entry(
            name, extension, 0x20, chain[0], len(contents)
        )
    write_cluster(3, boot_directory)

    return image

def posix_path(path, cygpath):
    if cygpath is None:
        return os.fspath(path)
    return subprocess.run(
        [cygpath, "-u", os.fspath(path)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()

def find_xorriso(executable):
    resolved = shutil.which(executable)
    if resolved:
        return resolved

    explicit_path = Path(executable)
    if explicit_path.is_file():
        return os.fspath(explicit_path.resolve())

    if os.name == "nt":
        roots = [
            os.environ.get("MSYS2_ROOT"),
            os.environ.get("MSYS_ROOT"),
            r"C:\msys64",
            r"C:\msys32",
        ]
        for root in roots:
            if root:
                candidate = Path(root) / "usr" / "bin" / "xorriso.exe"
                if candidate.is_file():
                    return os.fspath(candidate)

    raise FileNotFoundError(executable)

def main():
    parser = argparse.ArgumentParser(
        description="Build a UEFI-bootable ISO from the C++ kernel EFI images."
    )
    parser.add_argument("--bootloader", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--xorriso", default="xorriso")
    args = parser.parse_args()

    bootloader = args.bootloader.resolve(strict=True)
    kernel = args.kernel.resolve(strict=True)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    try:
        xorriso = find_xorriso(args.xorriso)
    except FileNotFoundError as error:
        raise SystemExit(
            "xorriso is required to create the ISO. Install it (for MSYS2, "
            "run 'pacman -S xorriso') and retry, or set XORRISO to its "
            "executable path."
        ) from error
    subprocess.run(
        [xorriso, "-version"],
        check=True,
        stdout=subprocess.DEVNULL,
    )

    cygpath = None
    if os.name == "nt":
        sibling_cygpath = Path(xorriso).with_name("cygpath.exe")
        if sibling_cygpath.is_file():
            cygpath = os.fspath(sibling_cygpath)

    with tempfile.TemporaryDirectory(prefix="c-kernel-iso-") as temporary:
        staging = Path(temporary)
        boot_directory = staging / "EFI" / "BOOT"
        boot_directory.mkdir(parents=True)
        shutil.copyfile(bootloader, boot_directory / "BOOTX64.EFI")
        shutil.copyfile(kernel, boot_directory / "KERNEL.EFI")
        (boot_directory / "efiboot.img").write_bytes(
            fat16_image(bootloader, kernel)
        )

        subprocess.run(
            [
                xorriso,
                "-as",
                "mkisofs",
                "-R",
                "-J",
                "-V",
                "C_KERNEL",
                "-o",
                posix_path(output, cygpath),
                "-e",
                "EFI/BOOT/efiboot.img",
                "-no-emul-boot",
                posix_path(staging, cygpath),
            ],
            check=True,
        )

    print(f"Created bootable UEFI ISO: {output}")

if __name__ == "__main__":
    main()
