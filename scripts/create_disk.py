import argparse
import os
import struct
import tempfile
from pathlib import Path

SECTOR_SIZE = 512
TOTAL_SECTORS = 32768
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 1
FAT_COUNT = 2
FAT_SECTORS = 128
ROOT_ENTRY_COUNT = 512
ROOT_SECTORS = ROOT_ENTRY_COUNT * 32 // SECTOR_SIZE
VOLUME_ID = 0x44525446
VOLUME_LABEL = b"KERNELDATA "
END_OF_CHAIN = 0xFFFF


def create_fat16_image():
    image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)
    boot = memoryview(image)[:SECTOR_SIZE]
    boot[0:3] = b"\xeb\x3c\x90"
    boot[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", boot, 11, SECTOR_SIZE)
    boot[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", boot, 14, RESERVED_SECTORS)
    boot[16] = FAT_COUNT
    struct.pack_into("<H", boot, 17, ROOT_ENTRY_COUNT)
    struct.pack_into("<H", boot, 19, TOTAL_SECTORS)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, FAT_SECTORS)
    struct.pack_into("<H", boot, 24, 63)
    struct.pack_into("<H", boot, 26, 255)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, VOLUME_ID)
    boot[43:54] = VOLUME_LABEL
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xaa"

    fat_start = RESERVED_SECTORS * SECTOR_SIZE
    struct.pack_into("<HH", image, fat_start, 0xFFF8, 0xFFFF)
    second_fat = fat_start + FAT_SECTORS * SECTOR_SIZE
    image[second_fat : second_fat + FAT_SECTORS * SECTOR_SIZE] = image[
        fat_start : fat_start + FAT_SECTORS * SECTOR_SIZE
    ]

    root = RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS
    label = bytearray(32)
    label[0:11] = VOLUME_LABEL
    label[11] = 0x08
    image[root * SECTOR_SIZE : root * SECTOR_SIZE + 32] = label
    return image


class Fat16Image:
    def __init__(self, image):
        self.image = image
        boot = image[:SECTOR_SIZE]
        if (
            boot[510:512] != b"\x55\xaa"
            or struct.unpack_from("<H", boot, 11)[0] != SECTOR_SIZE
            or struct.unpack_from("<I", boot, 39)[0] != VOLUME_ID
            or boot[43:54] != VOLUME_LABEL
        ):
            raise ValueError("Existing disk is not the expected FAT16 volume.")

        self.sectors_per_cluster = boot[13]
        self.reserved_sectors = struct.unpack_from("<H", boot, 14)[0]
        self.fat_count = boot[16]
        self.root_entry_count = struct.unpack_from("<H", boot, 17)[0]
        total_sectors = struct.unpack_from("<H", boot, 19)[0]
        if not total_sectors:
            total_sectors = struct.unpack_from("<I", boot, 32)[0]
        self.total_sectors = total_sectors
        self.fat_sectors = struct.unpack_from("<H", boot, 22)[0]
        if (
            self.sectors_per_cluster == 0
            or self.fat_count not in (1, 2)
            or self.root_entry_count == 0
            or self.fat_sectors == 0
        ):
            raise ValueError("Existing disk has invalid FAT16 geometry.")
        self.root_sectors = (
            self.root_entry_count * 32 + SECTOR_SIZE - 1
        ) // SECTOR_SIZE
        self.root_start = self.reserved_sectors + self.fat_count * self.fat_sectors
        self.data_start = self.root_start + self.root_sectors
        self.cluster_count = (
            total_sectors - self.data_start
        ) // self.sectors_per_cluster
        self.cluster_size = self.sectors_per_cluster * SECTOR_SIZE
        self.fat_start = self.reserved_sectors * SECTOR_SIZE
        self.modified = False

    @staticmethod
    def short_name(name):
        parts = name.upper().split(".")
        if len(parts) > 2 or not parts[0] or len(parts[0]) > 8:
            raise ValueError(f"Invalid FAT 8.3 name: {name}")
        extension = parts[1] if len(parts) == 2 else ""
        if len(extension) > 3 or not all(
            character.isascii() and (character.isalnum() or character in "_-$~!#&'()@^`{}")
            for character in parts[0] + extension
        ):
            raise ValueError(f"Invalid FAT 8.3 name: {name}")
        return parts[0].ljust(8).encode("ascii") + extension.ljust(3).encode("ascii")

    def fat_entry(self, cluster):
        return struct.unpack_from("<H", self.image, self.fat_start + cluster * 2)[0]

    def set_fat_entry(self, cluster, value):
        offset = cluster * 2
        for index in range(self.fat_count):
            struct.pack_into(
                "<H",
                self.image,
                self.fat_start + index * self.fat_sectors * SECTOR_SIZE + offset,
                value,
            )
        self.modified = True

    def allocate_cluster(self):
        for cluster in range(2, self.cluster_count + 2):
            if self.fat_entry(cluster) == 0:
                self.set_fat_entry(cluster, END_OF_CHAIN)
                self.write_cluster(cluster, b"")
                return cluster
        raise ValueError("FAT16 disk is full.")

    def cluster_lba(self, cluster):
        return self.data_start + (cluster - 2) * self.sectors_per_cluster

    def write_cluster(self, cluster, data):
        if len(data) > self.cluster_size:
            raise ValueError("Internal error: cluster data is too large.")
        start = self.cluster_lba(cluster) * SECTOR_SIZE
        self.image[start : start + self.cluster_size] = (
            data + bytes(self.cluster_size - len(data))
        )
        self.modified = True

    def directory_offsets(self, directory):
        if directory is None:
            start = self.root_start * SECTOR_SIZE
            end = start + self.root_entry_count * 32
            return list(range(start, end, 32))
        offsets = []
        cluster = directory
        visited = set()
        while cluster < 0xFFF8:
            if cluster < 2 or cluster >= self.cluster_count + 2 or cluster in visited:
                raise ValueError("Corrupt FAT16 directory cluster chain.")
            visited.add(cluster)
            start = self.cluster_lba(cluster) * SECTOR_SIZE
            offsets.extend(range(start, start + self.cluster_size, 32))
            cluster = self.fat_entry(cluster)
        return offsets

    def find_entry(self, directory, name):
        wanted = self.short_name(name)
        for offset in self.directory_offsets(directory):
            entry = self.image[offset : offset + 32]
            if entry[0] == 0:
                break
            if entry[0] == 0xE5 or entry[11] == 0x0F:
                continue
            if entry[:11] == wanted:
                return offset, entry
        return None, None

    def free_entry_offset(self, directory):
        offsets = self.directory_offsets(directory)
        for offset in offsets:
            if self.image[offset] in (0, 0xE5):
                return offset
        if directory is None:
            raise ValueError("FAT16 root directory is full.")
        tail = directory
        for _ in range(self.cluster_count):
            next_cluster = self.fat_entry(tail)
            if next_cluster >= 0xFFF8:
                new_cluster = self.allocate_cluster()
                self.set_fat_entry(tail, new_cluster)
                return self.cluster_lba(new_cluster) * SECTOR_SIZE
            tail = next_cluster
        raise ValueError("Corrupt FAT16 directory cluster chain.")

    def add_entry(self, directory, name, attributes, cluster, size=0):
        offset, existing = self.find_entry(directory, name)
        if existing is not None:
            raise ValueError(f"Entry already exists: {name}")
        offset = self.free_entry_offset(directory)
        entry = bytearray(32)
        entry[:11] = self.short_name(name)
        entry[11] = attributes
        struct.pack_into("<H", entry, 26, cluster)
        struct.pack_into("<I", entry, 28, size)
        self.image[offset : offset + 32] = entry
        self.modified = True
        return offset

    def ensure_directory(self, parent, name):
        offset, entry = self.find_entry(parent, name)
        if entry is not None:
            if entry[11] & 0x10 == 0:
                raise ValueError(f"Expected a directory: {name}")
            return struct.unpack_from("<H", entry, 26)[0], False

        cluster = self.allocate_cluster()
        parent_cluster = 0 if parent is None else parent
        entries = bytearray(self.cluster_size)
        entries[0:11] = b".          "
        entries[11] = 0x10
        struct.pack_into("<H", entries, 26, cluster)
        entries[32:43] = b"..         "
        entries[43] = 0x10
        struct.pack_into("<H", entries, 58, parent_cluster)
        self.write_cluster(cluster, entries)
        self.add_entry(parent, name, 0x10, cluster)
        return cluster, True

    def read_file(self, directory, name):
        offset, entry = self.find_entry(directory, name)
        if entry is None:
            return None
        if entry[11] & 0x10:
            raise ValueError(f"Expected a file: {name}")
        size = struct.unpack_from("<I", entry, 28)[0]
        cluster = struct.unpack_from("<H", entry, 26)[0]
        data = bytearray()
        visited = set()
        while len(data) < size:
            if cluster < 2 or cluster >= self.cluster_count + 2 or cluster in visited:
                raise ValueError(f"Corrupt FAT16 file chain: {name}")
            visited.add(cluster)
            start = self.cluster_lba(cluster) * SECTOR_SIZE
            data.extend(self.image[start : start + self.cluster_size])
            cluster = self.fat_entry(cluster)
        return bytes(data[:size])

    def free_chain(self, cluster):
        visited = set()
        while cluster >= 2 and cluster < 0xFFF8:
            if cluster in visited or cluster >= self.cluster_count + 2:
                raise ValueError("Corrupt FAT16 cluster chain.")
            visited.add(cluster)
            next_cluster = self.fat_entry(cluster)
            self.set_fat_entry(cluster, 0)
            cluster = next_cluster

    def write_file(self, directory, name, data):
        offset, entry = self.find_entry(directory, name)
        if entry is not None and entry[11] & 0x10:
            raise ValueError(f"Expected a file: {name}")
        old_cluster = (
            struct.unpack_from("<H", entry, 26)[0] if entry is not None else 0
        )
        first_cluster = 0
        previous_cluster = 0
        for start in range(0, len(data), self.cluster_size):
            cluster = self.allocate_cluster()
            if first_cluster == 0:
                first_cluster = cluster
            if previous_cluster != 0:
                self.set_fat_entry(previous_cluster, cluster)
            self.write_cluster(cluster, data[start : start + self.cluster_size])
            previous_cluster = cluster

        if entry is None:
            self.add_entry(directory, name, 0x20, first_cluster, len(data))
        else:
            replacement = bytearray(entry)
            replacement[11] = 0x20
            struct.pack_into("<H", replacement, 26, first_cluster)
            struct.pack_into("<I", replacement, 28, len(data))
            self.image[offset : offset + 32] = replacement
            self.modified = True
        if old_cluster != 0:
            self.free_chain(old_cluster)

    def move_volume_root_entries_to_usr(self, usr_cluster, root_cluster):
        offsets = self.directory_offsets(None)
        entries = []
        root_name = self.short_name("ROOT")
        for offset in offsets:
            entry = bytes(self.image[offset : offset + 32])
            if entry[0] == 0:
                break
            if (
                entry[0] != 0xE5
                and entry[11] != 0x0F
                and entry[11] & 0x08 == 0
                and entry[:11] != root_name
            ):
                entries.append((offset, entry))
        for old_offset, entry in entries:
            name = entry[:11]
            if any(
                candidate[0] != 0
                and candidate[0] != 0xE5
                and candidate[11] != 0x0F
                and candidate[:11] == name
                for candidate in (
                    self.image[offset : offset + 32]
                    for offset in self.directory_offsets(usr_cluster)
                )
            ):
                raise ValueError(
                    "Cannot migrate existing root files: an entry already "
                    "exists in ROOT/USR."
                )
            new_offset = self.free_entry_offset(usr_cluster)
            self.image[new_offset : new_offset + 32] = entry
            self.image[old_offset] = 0xE5
            self.modified = True
            if entry[11] & 0x10:
                cluster = struct.unpack_from("<H", entry, 26)[0]
                dotdot = self.cluster_lba(cluster) * SECTOR_SIZE + 32
                struct.pack_into("<H", self.image, dotdot + 26, usr_cluster)
                self.modified = True

        root_offset, root_entry = self.find_entry(None, "ROOT")
        if root_entry is None or struct.unpack_from("<H", root_entry, 26)[0] != root_cluster:
            raise ValueError("ROOT directory changed during disk initialization.")


def seed_system_files(image, bootloader, kernel):
    filesystem = Fat16Image(image)
    root_cluster, root_created = filesystem.ensure_directory(None, "ROOT")
    system_cluster, _ = filesystem.ensure_directory(root_cluster, "SYSTEM")
    usr_cluster, _ = filesystem.ensure_directory(root_cluster, "USR")
    if root_created:
        filesystem.move_volume_root_entries_to_usr(usr_cluster, root_cluster)
    filesystem.write_file(system_cluster, "BOOTX64.EFI", bootloader.read_bytes())
    filesystem.write_file(system_cluster, "KERNEL.EFI", kernel.read_bytes())
    return filesystem.modified


def main():
    parser = argparse.ArgumentParser(
        description="Create or update a persistent FAT16 OS data disk."
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bootloader", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    bootloader = args.bootloader.resolve()
    kernel = args.kernel.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    if not bootloader.is_file() or not kernel.is_file():
        raise SystemExit("Build BOOTX64.EFI and KERNEL.EFI before creating the disk.")
    if output.exists():
        if output.stat().st_size != TOTAL_SECTORS * SECTOR_SIZE:
            raise SystemExit(
                f"Existing disk has an unexpected size; refusing to overwrite: {output}"
            )
        image = bytearray(output.read_bytes())
    else:
        image = create_fat16_image()

    try:
        seed_system_files(image, bootloader, kernel)
    except ValueError as error:
        raise SystemExit(f"Could not update OS files on the FAT16 disk: {error}") from error

    with tempfile.NamedTemporaryFile(
        dir=output.parent, prefix=output.name + ".", suffix=".tmp", delete=False
    ) as temporary:
        temporary_path = Path(temporary.name)
        temporary.write(image)
        temporary.flush()
        os.fsync(temporary.fileno())
    try:
        os.replace(temporary_path, output)
    finally:
        if temporary_path.exists():
            temporary_path.unlink()
    print(f"Updated persistent FAT16 OS disk: {output}")


if __name__ == "__main__":
    main()
