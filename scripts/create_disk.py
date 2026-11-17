import argparse
import os
import struct
import tempfile
from pathlib import Path

# This fixed geometry is the persistent user disk; changing its identity would
# make the kernel's ATA probe reject existing images.
SECTOR_SIZE = 512
TOTAL_SECTORS = 131072
LEGACY_FAT16_SECTORS = 32768
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 32
FAT_COUNT = 2
FAT_SECTORS = 0
VOLUME_ID = 0x44525446
VOLUME_LABEL = b"KERNELDATA "
END_OF_CHAIN = 0x0FFFFFFF
FAT16_END_OF_CHAIN = 0xFFFF


def fat32_fat_sectors():
    """Solve FAT32's self-referential allocation-table size for this geometry."""
    denominator = (SECTOR_SIZE // 4) * SECTORS_PER_CLUSTER + FAT_COUNT
    numerator = TOTAL_SECTORS - RESERVED_SECTORS + 2 * SECTORS_PER_CLUSTER
    sectors = (numerator + denominator - 1) // denominator
    while True:
        cluster_count = (
            TOTAL_SECTORS - RESERVED_SECTORS - FAT_COUNT * sectors
        ) // SECTORS_PER_CLUSTER
        required = (cluster_count + 2) * 4
        required = (required + SECTOR_SIZE - 1) // SECTOR_SIZE
        if required <= sectors:
            return sectors
        sectors = required


FAT_SECTORS = fat32_fat_sectors()


def create_fat32_image():
    """Create a 64 MiB FAT32 volume with mirrored FATs and FSInfo hints."""
    image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)
    boot = memoryview(image)[:SECTOR_SIZE]
    boot[0:3] = b"\xeb\x3c\x90"
    boot[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", boot, 11, SECTOR_SIZE)
    boot[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", boot, 14, RESERVED_SECTORS)
    boot[16] = FAT_COUNT
    struct.pack_into("<H", boot, 17, 0)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, 0)
    struct.pack_into("<H", boot, 24, 63)
    struct.pack_into("<H", boot, 26, 255)
    struct.pack_into("<I", boot, 32, TOTAL_SECTORS)
    struct.pack_into("<I", boot, 36, FAT_SECTORS)
    struct.pack_into("<H", boot, 40, 0)
    struct.pack_into("<H", boot, 42, 0)
    struct.pack_into("<I", boot, 44, 2)
    struct.pack_into("<H", boot, 48, 1)
    struct.pack_into("<H", boot, 50, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    struct.pack_into("<I", boot, 67, VOLUME_ID)
    boot[71:82] = VOLUME_LABEL
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xaa"

    # FAT32's FSInfo counts are advisory; the kernel can recover by scanning.
    fsinfo = memoryview(image)[SECTOR_SIZE : 2 * SECTOR_SIZE]
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    cluster_count = (
        TOTAL_SECTORS - RESERVED_SECTORS - FAT_COUNT * FAT_SECTORS
    ) // SECTORS_PER_CLUSTER
    struct.pack_into("<I", fsinfo, 488, 0xFFFFFFFF)
    struct.pack_into("<I", fsinfo, 492, 0xFFFFFFFF)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)
    backup = memoryview(image)[6 * SECTOR_SIZE : 7 * SECTOR_SIZE]
    backup[:] = boot
    image[7 * SECTOR_SIZE : 8 * SECTOR_SIZE] = fsinfo

    fat_start = RESERVED_SECTORS * SECTOR_SIZE
    struct.pack_into("<III", image, fat_start, 0x0FFFFFF8, 0xFFFFFFFF,
                     END_OF_CHAIN)
    fat_size = FAT_SECTORS * SECTOR_SIZE
    second_fat = fat_start + fat_size
    image[second_fat : second_fat + fat_size] = image[fat_start : fat_start + fat_size]

    root = RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS
    label = bytearray(32)
    label[0:11] = VOLUME_LABEL
    label[11] = 0x08
    root_data = root * SECTOR_SIZE
    image[root_data : root_data + 32] = label
    return image


class Fat16Image:
    """Small host-side FAT16 editor used to preserve files during image updates."""

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
        # This seeder writes only the system's fixed 8.3 names, not VFAT long names.
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
        # Mark and clear the cluster before it is linked into a directory or file.
        for cluster in range(2, self.cluster_count + 2):
            if self.fat_entry(cluster) == 0:
                self.set_fat_entry(cluster, FAT16_END_OF_CHAIN)
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
        # Validate directory chains while walking them to avoid looping on corruption.
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

    @staticmethod
    def short_name_text(entry):
        base = entry[0:8].decode("ascii", errors="replace").rstrip()
        extension = entry[8:11].decode("ascii", errors="replace").rstrip()
        return f"{base}.{extension}" if extension else base

    @staticmethod
    def lfn_checksum(short_name):
        checksum = 0
        for byte in short_name:
            checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 0xFF
        return checksum

    def snapshot_directory(self, directory=None, visited=None):
        """Read a FAT16 subtree into memory before replacing the old image."""
        if visited is None:
            visited = set()
        if directory is not None:
            if directory in visited:
                raise ValueError("Corrupt FAT16 directory tree contains a cycle.")
            visited.add(directory)
        snapshots = []
        lfn = {}
        lfn_total = 0
        lfn_checksum = None
        offsets = self.directory_offsets(directory)
        for offset in offsets:
            entry = bytes(self.image[offset : offset + 32])
            if entry[0] == 0:
                break
            if entry[0] == 0xE5:
                lfn.clear()
                lfn_total = 0
                lfn_checksum = None
                continue
            if entry[11] == 0x0F:
                ordinal = entry[0] & 0x1F
                if entry[0] & 0x40:
                    lfn.clear()
                    lfn_total = ordinal
                    lfn_checksum = entry[13]
                if ordinal == 0 or ordinal > 20 or entry[13] != lfn_checksum:
                    lfn.clear()
                    lfn_total = 0
                    lfn_checksum = None
                else:
                    units = []
                    for part in (entry[1:11], entry[14:26], entry[28:32]):
                        units.extend(
                            struct.unpack("<" + "H" * (len(part) // 2), part)
                        )
                    lfn[ordinal] = units
                continue
            if entry[11] & 0x08:
                lfn.clear()
                lfn_total = 0
                lfn_checksum = None
                continue

            name = self.short_name_text(entry)
            if (
                lfn_total
                and len(lfn) == lfn_total
                and all(ordinal in lfn for ordinal in range(1, lfn_total + 1))
                and lfn_checksum == self.lfn_checksum(entry[:11])
            ):
                units = []
                for ordinal in range(1, lfn_total + 1):
                    units.extend(lfn[ordinal])
                characters = []
                for unit in units:
                    if unit in (0, 0xFFFF):
                        break
                    characters.append(unit)
                try:
                    long_name = bytes().join(
                        struct.pack("<H", unit) for unit in characters
                    ).decode("utf-16le")
                    if long_name:
                        name = long_name
                except UnicodeDecodeError:
                    pass
            lfn.clear()
            lfn_total = 0
            lfn_checksum = None

            if name in (".", ".."):
                continue
            first_cluster = struct.unpack_from("<H", entry, 26)[0]
            if entry[11] & 0x10:
                if first_cluster < 2:
                    raise ValueError("Corrupt FAT16 directory has no cluster.")
                children = self.snapshot_directory(first_cluster, visited)
                snapshots.append((name, entry[11], children))
                continue

            size = struct.unpack_from("<I", entry, 28)[0]
            cluster = first_cluster
            data = bytearray()
            chain = set()
            while len(data) < size:
                if (
                    cluster < 2
                    or cluster >= self.cluster_count + 2
                    or cluster in chain
                ):
                    raise ValueError("Corrupt FAT16 file cluster chain.")
                chain.add(cluster)
                start = self.cluster_lba(cluster) * SECTOR_SIZE
                data.extend(self.image[start : start + self.cluster_size])
                cluster = self.fat_entry(cluster)
            snapshots.append((name, entry[11], bytes(data[:size])))
        return snapshots

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
        # Stage a replacement chain before switching the directory entry.
        # Existing data stays reachable until the new contents are published.
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
            assert offset is not None
            replacement = bytearray(entry)
            replacement[11] = 0x20
            struct.pack_into("<H", replacement, 26, first_cluster)
            struct.pack_into("<I", replacement, 28, len(data))
            self.image[offset : offset + 32] = replacement
            self.modified = True
        if old_cluster != 0:
            self.free_chain(old_cluster)

    def move_volume_root_entries_to_usr(self, usr_cluster, root_cluster):
        # Migrate legacy root files once, preserving each existing FAT entry and chain.
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


class Fat32Image:
    """Host-side FAT32 writer used to seed and migrate the persistent data disk."""

    LFN_OFFSETS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)

    def __init__(self, image):
        self.image = image
        if (
            len(image) != TOTAL_SECTORS * SECTOR_SIZE
            or image[510:512] != b"\x55\xaa"
            or struct.unpack_from("<H", image, 11)[0] != SECTOR_SIZE
            or image[13] == 0
            or image[13] & (image[13] - 1) != 0
            or image[16] != FAT_COUNT
            or struct.unpack_from("<H", image, 14)[0] == 0
            or struct.unpack_from("<I", image, 36)[0] == 0
            or struct.unpack_from("<H", image, 22)[0] != 0
            or struct.unpack_from("<H", image, 17)[0] != 0
            or struct.unpack_from("<I", image, 67)[0] != VOLUME_ID
            or image[71:82] != VOLUME_LABEL
        ):
            raise ValueError("Existing disk is not the expected FAT32 volume.")
        self.sectors_per_cluster = image[13]
        self.reserved_sectors = struct.unpack_from("<H", image, 14)[0]
        self.fat_count = image[16]
        self.fat_sectors = struct.unpack_from("<I", image, 36)[0]
        self.total_sectors = struct.unpack_from("<I", image, 32)[0]
        self.root_cluster = struct.unpack_from("<I", image, 44)[0] & 0x0FFFFFFF
        self.data_start = self.reserved_sectors + self.fat_count * self.fat_sectors
        self.cluster_count = (
            self.total_sectors - self.data_start
        ) // self.sectors_per_cluster
        if (
            self.sectors_per_cluster == 0
            or self.fat_count != FAT_COUNT
            or self.cluster_count < 65525
            or self.fat_sectors * SECTOR_SIZE < (self.cluster_count + 2) * 4
            or self.root_cluster < 2
            or self.root_cluster >= self.cluster_count + 2
        ):
            raise ValueError("Existing disk has invalid FAT32 geometry.")
        self.cluster_size = self.sectors_per_cluster * SECTOR_SIZE
        self.fat_start = self.reserved_sectors * SECTOR_SIZE
        self.next_free = 3
        self.modified = False

    @staticmethod
    def short_alias(name, existing, allow_existing=False):
        allowed = "_-$~!#&'()@^`{}"
        parts = name.upper().split(".")
        base = parts[0]
        extension = parts[1] if len(parts) == 2 else ""
        if (
            len(parts) <= 2
            and 1 <= len(base) <= 8
            and len(extension) <= 3
            and all(
                character.isascii()
                and (character.isalnum() or character in allowed)
                for character in base + extension
            )
        ):
            candidate = base.ljust(8).encode("ascii") + extension.ljust(3).encode("ascii")
            if candidate not in existing or allow_existing:
                return candidate

        clean_base = "".join(
            character
            for character in base
            if character.isascii() and (character.isalnum() or character in allowed)
        )
        clean_extension = "".join(
            character
            for character in extension
            if character.isascii()
            and (character.isalnum() or character in allowed)
        )[:3]
        clean_base = clean_base or "_"
        for suffix in range(1, 1_000_000):
            ending = f"~{suffix}"
            prefix = clean_base[: 8 - len(ending)]
            candidate = (prefix + ending).ljust(8).encode("ascii")
            candidate += clean_extension.ljust(3).encode("ascii")
            if candidate not in existing:
                return candidate
        raise ValueError(f"Could not create a unique FAT short alias for {name!r}.")

    @staticmethod
    def short_name_text(short_name):
        base = short_name[:8].decode("ascii").rstrip()
        extension = short_name[8:11].decode("ascii").rstrip()
        return f"{base}.{extension}" if extension else base

    @staticmethod
    def lfn_checksum(short_name):
        checksum = 0
        for byte in short_name:
            checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 0xFF
        return checksum

    def fat_entry(self, cluster):
        offset = self.fat_start + cluster * 4
        return struct.unpack_from("<I", self.image, offset)[0] & 0x0FFFFFFF

    def set_fat_entry(self, cluster, value):
        for index in range(self.fat_count):
            offset = (
                self.fat_start
                + index * self.fat_sectors * SECTOR_SIZE
                + cluster * 4
            )
            previous = struct.unpack_from("<I", self.image, offset)[0]
            struct.pack_into(
                "<I",
                self.image,
                offset,
                (previous & 0xF0000000) | (value & 0x0FFFFFFF),
            )
        self.modified = True

    def cluster_lba(self, cluster):
        return self.data_start + (cluster - 2) * self.sectors_per_cluster

    def cluster_offset(self, cluster):
        if cluster < 2 or cluster >= self.cluster_count + 2:
            raise ValueError(f"Invalid FAT32 cluster: {cluster}")
        return self.cluster_lba(cluster) * SECTOR_SIZE

    def write_cluster(self, cluster, data):
        if len(data) > self.cluster_size:
            raise ValueError("Internal error: cluster data is too large.")
        start = self.cluster_offset(cluster)
        self.image[start : start + self.cluster_size] = data.ljust(
            self.cluster_size, b"\0"
        )
        self.modified = True

    def allocate_cluster(self):
        for offset in range(self.cluster_count):
            cluster = 2 + (self.next_free - 2 + offset) % self.cluster_count
            if self.fat_entry(cluster) == 0:
                self.set_fat_entry(cluster, END_OF_CHAIN)
                self.write_cluster(cluster, b"")
                self.next_free = 2 + (cluster - 1) % self.cluster_count
                return cluster
        raise ValueError("FAT32 disk is full.")

    def directory_chain(self, directory):
        chain = []
        cluster = directory
        visited = set()
        while True:
            if (
                cluster < 2
                or cluster >= self.cluster_count + 2
                or cluster in visited
            ):
                raise ValueError("Corrupt FAT32 directory cluster chain.")
            visited.add(cluster)
            chain.append(cluster)
            following = self.fat_entry(cluster)
            if following >= 0x0FFFFFF8:
                return chain
            cluster = following

    def directory_offsets(self, directory):
        offsets = []
        for cluster in self.directory_chain(directory):
            start = self.cluster_offset(cluster)
            offsets.extend(range(start, start + self.cluster_size, 32))
        return offsets

    def snapshot_directory(self, directory=None, visited=None):
        """Read a FAT32 subtree for migration verification without modifying it."""
        if directory is None:
            directory = self.root_cluster
        if visited is None:
            visited = set()
        if directory in visited:
            raise ValueError("Corrupt FAT32 directory tree contains a cycle.")
        visited.add(directory)
        snapshots = []
        lfn = {}
        lfn_total = 0
        lfn_checksum = None
        for offset in self.directory_offsets(directory):
            entry = bytes(self.image[offset : offset + 32])
            if entry[0] == 0:
                break
            if entry[0] == 0xE5:
                lfn.clear()
                lfn_total = 0
                lfn_checksum = None
                continue
            if entry[11] == 0x0F:
                ordinal = entry[0] & 0x1F
                if entry[0] & 0x40:
                    lfn.clear()
                    lfn_total = ordinal
                    lfn_checksum = entry[13]
                if ordinal == 0 or ordinal > 20 or entry[13] != lfn_checksum:
                    lfn.clear()
                    lfn_total = 0
                    lfn_checksum = None
                else:
                    units = []
                    for part in (entry[1:11], entry[14:26], entry[28:32]):
                        units.extend(
                            struct.unpack("<" + "H" * (len(part) // 2), part)
                        )
                    lfn[ordinal] = units
                continue
            if entry[11] & 0x08:
                lfn.clear()
                lfn_total = 0
                lfn_checksum = None
                continue

            name = self.short_name_text(entry[:11])
            if (
                lfn_total
                and len(lfn) == lfn_total
                and all(ordinal in lfn for ordinal in range(1, lfn_total + 1))
                and lfn_checksum == self.lfn_checksum(entry[:11])
            ):
                units = []
                for ordinal in range(1, lfn_total + 1):
                    units.extend(lfn[ordinal])
                characters = []
                for unit in units:
                    if unit in (0, 0xFFFF):
                        break
                    characters.append(unit)
                try:
                    encoded = b"".join(struct.pack("<H", unit) for unit in characters)
                    name = encoded.decode("utf-16le") or name
                except UnicodeDecodeError:
                    pass
            lfn.clear()
            lfn_total = 0
            lfn_checksum = None
            if name in (".", ".."):
                continue

            cluster = (
                (struct.unpack_from("<H", entry, 20)[0] << 16)
                | struct.unpack_from("<H", entry, 26)[0]
            ) & 0x0FFFFFFF
            if entry[11] & 0x10:
                if cluster < 2:
                    raise ValueError("Corrupt FAT32 directory has no cluster.")
                snapshots.append(
                    (name, entry[11], self.snapshot_directory(cluster, visited))
                )
                continue

            size = struct.unpack_from("<I", entry, 28)[0]
            data = bytearray()
            chain = set()
            while len(data) < size:
                if (
                    cluster < 2
                    or cluster >= self.cluster_count + 2
                    or cluster in chain
                ):
                    raise ValueError("Corrupt FAT32 file cluster chain.")
                chain.add(cluster)
                start = self.cluster_offset(cluster)
                data.extend(self.image[start : start + self.cluster_size])
                cluster = self.fat_entry(cluster)
            snapshots.append((name, entry[11], bytes(data[:size])))
        return snapshots

    def short_aliases(self, directory):
        aliases = set()
        for offset in self.directory_offsets(directory):
            entry = self.image[offset : offset + 32]
            if entry[0] == 0:
                break
            if entry[0] != 0xE5 and entry[11] != 0x0F:
                aliases.add(bytes(entry[:11]))
        return aliases

    def find_entry(self, directory, name):
        aliases = self.short_aliases(directory)
        wanted = self.short_alias(name, aliases, allow_existing=True)
        for offset in self.directory_offsets(directory):
            entry = self.image[offset : offset + 32]
            if entry[0] == 0:
                break
            if entry[0] != 0xE5 and entry[11] != 0x0F and entry[:11] == wanted:
                return offset, bytes(entry)
        return None, None

    def free_entry_offsets(self, directory, count):
        offsets = self.directory_offsets(directory)
        free_run = []
        end_marker_seen = False
        for offset in offsets:
            first = self.image[offset]
            if first == 0:
                end_marker_seen = True
            if end_marker_seen or first == 0xE5:
                free_run.append(offset)
                if len(free_run) == count:
                    return free_run
            else:
                free_run = []

        chain = self.directory_chain(directory)
        tail = chain[-1]
        while len(free_run) < count:
            cluster = self.allocate_cluster()
            self.set_fat_entry(tail, cluster)
            tail = cluster
            start = self.cluster_offset(cluster)
            for offset in range(start, start + self.cluster_size, 32):
                free_run.append(offset)
                if len(free_run) == count:
                    return free_run
        return free_run[-count:]

    def add_entry(self, directory, name, attributes, cluster, size=0):
        if not name or len(name.encode("utf-16le")) // 2 > 255:
            raise ValueError(f"Invalid FAT32 filename: {name!r}")
        if any(character in '/\\\0' or ord(character) < 0x20 for character in name):
            raise ValueError(f"Invalid FAT32 filename: {name!r}")
        aliases = self.short_aliases(directory)
        short_name = self.short_alias(name, aliases)
        existing, _ = self.find_entry_by_alias(directory, short_name)
        if existing is not None:
            raise ValueError(f"Entry already exists: {name}")

        units = list(struct.unpack("<" + "H" * (len(name.encode("utf-16le")) // 2),
                                   name.encode("utf-16le")))
        units.append(0)
        lfn_count = (len(units) + 12) // 13
        locations = self.free_entry_offsets(directory, lfn_count + 1)
        checksum = self.lfn_checksum(short_name)
        for index in range(lfn_count):
            ordinal = lfn_count - index
            entry = bytearray([0xFF] * 32)
            entry[0] = ordinal | (0x40 if ordinal == lfn_count else 0)
            entry[11] = 0x0F
            entry[12] = 0
            entry[13] = checksum
            struct.pack_into("<H", entry, 26, 0)
            for character_index, field_offset in enumerate(self.LFN_OFFSETS):
                position = (ordinal - 1) * 13 + character_index
                character = units[position] if position < len(units) else 0xFFFF
                struct.pack_into("<H", entry, field_offset, character)
            self.image[locations[index] : locations[index] + 32] = entry

        short_entry = bytearray(32)
        short_entry[:11] = short_name
        short_entry[11] = attributes
        struct.pack_into("<H", short_entry, 20, cluster >> 16)
        struct.pack_into("<H", short_entry, 26, cluster & 0xFFFF)
        struct.pack_into("<I", short_entry, 28, size)
        short_offset = locations[lfn_count]
        self.image[short_offset : short_offset + 32] = short_entry
        self.modified = True
        return short_offset

    def find_entry_by_alias(self, directory, alias):
        for offset in self.directory_offsets(directory):
            entry = self.image[offset : offset + 32]
            if entry[0] == 0:
                break
            if entry[0] != 0xE5 and entry[11] != 0x0F and entry[:11] == alias:
                return offset, bytes(entry)
        return None, None

    def ensure_directory(self, parent, name):
        offset, entry = self.find_entry(parent, name)
        if entry is not None:
            if entry[11] & 0x10 == 0:
                raise ValueError(f"Expected a directory: {name}")
            cluster = (struct.unpack_from("<H", entry, 20)[0] << 16) | struct.unpack_from(
                "<H", entry, 26
            )[0]
            return cluster & 0x0FFFFFFF, False

        cluster = self.allocate_cluster()
        parent_entries = bytearray(self.cluster_size)
        parent_cluster = 0 if parent == self.root_cluster else parent
        parent_entries[0:11] = b".          "
        parent_entries[11] = 0x10
        struct.pack_into("<H", parent_entries, 20, cluster >> 16)
        struct.pack_into("<H", parent_entries, 26, cluster & 0xFFFF)
        parent_entries[32:43] = b"..         "
        parent_entries[43] = 0x10
        struct.pack_into("<H", parent_entries, 52, parent_cluster >> 16)
        struct.pack_into("<H", parent_entries, 58, parent_cluster & 0xFFFF)
        self.write_cluster(cluster, parent_entries)
        self.add_entry(parent, name, 0x10, cluster)
        return cluster, True

    def free_chain(self, cluster):
        visited = set()
        while cluster >= 2 and cluster < 0x0FFFFFF8:
            if cluster >= self.cluster_count + 2 or cluster in visited:
                raise ValueError("Corrupt FAT32 cluster chain.")
            visited.add(cluster)
            following = self.fat_entry(cluster)
            self.set_fat_entry(cluster, 0)
            cluster = following

    def write_file(self, directory, name, data, attributes=0x20):
        offset, entry = self.find_entry(directory, name)
        if entry is not None and entry[11] & 0x10:
            raise ValueError(f"Expected a file: {name}")
        old_cluster = 0
        if entry is not None:
            old_cluster = (
                struct.unpack_from("<H", entry, 20)[0] << 16
            ) | struct.unpack_from("<H", entry, 26)[0]
            old_cluster &= 0x0FFFFFFF
            attributes = entry[11]

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
            self.add_entry(directory, name, attributes, first_cluster, len(data))
        else:
            assert offset is not None
            replacement = bytearray(entry)
            struct.pack_into("<H", replacement, 20, first_cluster >> 16)
            struct.pack_into("<H", replacement, 26, first_cluster & 0xFFFF)
            struct.pack_into("<I", replacement, 28, len(data))
            self.image[offset : offset + 32] = replacement
            self.modified = True
        if old_cluster != 0:
            self.free_chain(old_cluster)


def migrate_legacy_disk(image):
    """Copy a FAT16 volume tree to the new FAT32 image without dropping names/data."""
    legacy = Fat16Image(image)
    tree = legacy.snapshot_directory()
    upgraded = Fat32Image(create_fat32_image())
    root_entry = next(
        (
            (name, attributes, contents)
            for name, attributes, contents in tree
            if name.casefold() == "root" and attributes & 0x10
        ),
        None,
    )
    if root_entry is None:
        if any(name.casefold() == "root" for name, _, _ in tree):
            raise ValueError("Legacy ROOT entry exists but is not a directory.")
        tree = [
            (
                "ROOT",
                0x10,
                [
                    ("SYSTEM", 0x10, []),
                    ("USR", 0x10, tree),
                ],
            )
        ]

    def copy_directory(parent, children):
        for name, attributes, contents in children:
            if attributes & 0x10:
                child, _ = upgraded.ensure_directory(parent, name)
                copy_directory(child, contents)
            else:
                upgraded.write_file(parent, name, contents, attributes & 0x27)

    root = upgraded.root_cluster
    copy_directory(root, tree)
    if normalized_tree(tree) != normalized_tree(upgraded.snapshot_directory()):
        raise ValueError("FAT16-to-FAT32 migration did not preserve the volume tree.")
    return upgraded.image


def normalized_tree(tree):
    """Compare names and contents while ignoring host-seeder attribute changes."""
    return sorted(
        (
            name,
            bool(attributes & 0x10),
            normalized_tree(contents) if attributes & 0x10 else contents,
        )
        for name, attributes, contents in tree
    )


def seed_system_files(image, bootloader, kernel):
    """Ensure the virtual-root layout exists and refresh both protected EFI files."""
    filesystem = Fat32Image(image)
    root_cluster, _ = filesystem.ensure_directory(
        filesystem.root_cluster, "ROOT"
    )
    system_cluster, _ = filesystem.ensure_directory(root_cluster, "SYSTEM")
    filesystem.ensure_directory(root_cluster, "USR")
    filesystem.write_file(system_cluster, "BOOTX64.EFI", bootloader.read_bytes())
    filesystem.write_file(system_cluster, "KERNEL.EFI", kernel.read_bytes())
    return filesystem.modified


def main():
    parser = argparse.ArgumentParser(
        description="Create or update a persistent FAT32 OS data disk."
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
    # Never silently replace an unexpected existing disk image.
    if output.exists():
        existing = output.read_bytes()
        if len(existing) == TOTAL_SECTORS * SECTOR_SIZE:
            image = bytearray(existing)
            try:
                Fat32Image(image)
            except ValueError as error:
                raise SystemExit(
                    f"Existing disk is not a valid project FAT32 volume: {error}"
                ) from error
        elif len(existing) == LEGACY_FAT16_SECTORS * SECTOR_SIZE:
            try:
                image = migrate_legacy_disk(existing)
            except (ValueError, struct.error) as error:
                raise SystemExit(
                    f"Could not safely migrate the existing FAT16 disk: {error}"
                ) from error
        else:
            raise SystemExit(
                f"Existing disk has an unexpected size; refusing to overwrite: {output}"
            )
    else:
        image = create_fat32_image()

    try:
        seed_system_files(image, bootloader, kernel)
    except (ValueError, struct.error) as error:
        raise SystemExit(f"Could not update OS files on the FAT32 disk: {error}") from error

    # Replace the disk only after the complete updated image has reached a temp file.
    # Stage the entire modified image beside the destination, then atomically replace it.
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
    print(f"Updated persistent FAT32 OS disk: {output}")


if __name__ == "__main__":
    main()
