import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import create_disk
import create_iso


SECTOR_SIZE = create_disk.SECTOR_SIZE


def legacy_fat16_image():
    total_sectors = 32768
    fat_sectors = 128
    root_entries = 512
    image = bytearray(total_sectors * SECTOR_SIZE)
    boot = memoryview(image)[:SECTOR_SIZE]
    boot[0:3] = b"\xeb\x3c\x90"
    boot[3:11] = b"MSWIN4.1"
    struct_pack = create_disk.struct.pack_into
    struct_pack("<H", boot, 11, SECTOR_SIZE)
    boot[13] = 1
    struct_pack("<H", boot, 14, 1)
    boot[16] = 2
    struct_pack("<H", boot, 17, root_entries)
    struct_pack("<H", boot, 19, total_sectors)
    boot[21] = 0xF8
    struct_pack("<H", boot, 22, fat_sectors)
    struct_pack("<I", boot, 39, create_disk.VOLUME_ID)
    boot[43:54] = create_disk.VOLUME_LABEL
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xaa"

    fat_start = SECTOR_SIZE
    struct_pack("<HH", image, fat_start, 0xFFF8, 0xFFFF)
    second_fat = fat_start + fat_sectors * SECTOR_SIZE
    image[second_fat : second_fat + fat_sectors * SECTOR_SIZE] = image[
        fat_start : fat_start + fat_sectors * SECTOR_SIZE
    ]
    root_start = (1 + 2 * fat_sectors) * SECTOR_SIZE
    label = bytearray(32)
    label[:11] = create_disk.VOLUME_LABEL
    label[11] = 0x08
    image[root_start : root_start + 32] = label
    return image


def add_legacy_long_name_file(filesystem, directory, name, data):
    short_name = b"LONGNO~1TXT"
    cluster = filesystem.allocate_cluster()
    filesystem.write_cluster(cluster, data)
    units = list(create_disk.struct.unpack(
        "<" + "H" * (len(name.encode("utf-16le")) // 2), name.encode("utf-16le")
    ))
    units.append(0)
    count = (len(units) + 12) // 13
    checksum = create_disk.Fat16Image.lfn_checksum(short_name)
    first_offset = filesystem.free_entry_offset(directory)
    offsets = [first_offset + 32 * index for index in range(count + 1)]
    locations = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
    for index in range(count):
        ordinal = count - index
        entry = bytearray([0xFF] * 32)
        entry[0] = ordinal | (0x40 if ordinal == count else 0)
        entry[11] = 0x0F
        entry[12] = 0
        entry[13] = checksum
        create_disk.struct.pack_into("<H", entry, 26, 0)
        for character_index, field_offset in enumerate(locations):
            position = (ordinal - 1) * 13 + character_index
            character = units[position] if position < len(units) else 0xFFFF
            create_disk.struct.pack_into("<H", entry, field_offset, character)
        filesystem.image[offsets[index] : offsets[index] + 32] = entry

    entry = bytearray(32)
    entry[:11] = short_name
    entry[11] = 0x20
    create_disk.struct.pack_into("<H", entry, 26, cluster)
    create_disk.struct.pack_into("<I", entry, 28, len(data))
    filesystem.image[offsets[-1] : offsets[-1] + 32] = entry
    filesystem.modified = True


def find_snapshot(tree, path):
    current = tree
    for index, component in enumerate(path):
        found = next(entry for entry in current if entry[0] == component)
        if index == len(path) - 1:
            return found
        current = found[2]
    raise AssertionError("empty path")


def fake_pe():
    image = bytearray(68)
    image[:2] = b"MZ"
    create_disk.struct.pack_into("<I", image, 0x3C, 64)
    image[64:68] = b"PE\0\0"
    return bytes(image)


class ImageFile:
    def __init__(self, data):
        self.data = data

    def read_bytes(self):
        return self.data


class Fat32DiskTests(unittest.TestCase):
    def test_efi_boot_image_uses_compact_fat16_geometry(self):
        image = create_iso.fat16_image(
            ImageFile(fake_pe()), ImageFile(fake_pe())
        )
        sectors_per_cluster = image[13]
        fat_sectors = create_disk.struct.unpack_from("<H", image, 22)[0]
        root_entries = create_disk.struct.unpack_from("<H", image, 17)[0]
        root_sectors = root_entries * 32 // SECTOR_SIZE
        data_start = 1 + 2 * fat_sectors + root_sectors
        total_sectors = create_disk.struct.unpack_from("<I", image, 32)[0]
        cluster_count = (total_sectors - data_start) // sectors_per_cluster

        self.assertEqual(image[54:62], b"FAT16   ")
        self.assertGreaterEqual(cluster_count, 8192)
        self.assertLessEqual(cluster_count, 65524)
        self.assertLessEqual(total_sectors, 0xFFFF)
        self.assertEqual(total_sectors * SECTOR_SIZE, len(image))

        fat_start = SECTOR_SIZE
        fat_size = fat_sectors * SECTOR_SIZE
        first_fat = image[fat_start : fat_start + fat_size]
        second_fat = image[fat_start + fat_size : fat_start + 2 * fat_size]
        self.assertEqual(first_fat, second_fat)

        def cluster_bytes(cluster):
            start = (data_start + (cluster - 2) * sectors_per_cluster) * SECTOR_SIZE
            return image[start : start + sectors_per_cluster * SECTOR_SIZE]

        root_directory = cluster_bytes(2)
        boot_directory = cluster_bytes(3)
        self.assertEqual(root_directory[0:11], b".          ")
        self.assertEqual(root_directory[64:75], b"BOOT       ")
        self.assertEqual(boot_directory[64:72], b"BOOTX64 ")
        self.assertEqual(boot_directory[72:75], b"EFI")
        self.assertEqual(boot_directory[96:104], b"KERNEL  ")
        self.assertEqual(boot_directory[104:107], b"EFI")

        for offset, expected in ((64, fake_pe()), (96, fake_pe())):
            entry = boot_directory[offset : offset + 32]
            cluster = create_disk.struct.unpack_from("<H", entry, 26)[0]
            file_size = create_disk.struct.unpack_from("<I", entry, 28)[0]
            contents = bytearray()
            while cluster < 0xFFF8:
                self.assertLess(cluster, cluster_count + 2)
                contents.extend(cluster_bytes(cluster))
                cluster = create_disk.struct.unpack_from(
                    "<H", first_fat, cluster * 2
                )[0]
            self.assertEqual(file_size, len(expected))
            self.assertEqual(contents[:file_size], expected)

    def test_new_image_has_fat32_geometry(self):
        image = create_disk.create_fat32_image()
        filesystem = create_disk.Fat32Image(image)

        self.assertEqual(len(image), 64 * 1024 * 1024)
        self.assertGreaterEqual(filesystem.cluster_count, 65525)
        self.assertEqual(filesystem.root_cluster, 2)
        self.assertEqual(filesystem.fat_entry(2), create_disk.END_OF_CHAIN)

    def test_legacy_migration_preserves_nested_and_long_name_files(self):
        legacy_image = legacy_fat16_image()
        legacy = create_disk.Fat16Image(legacy_image)
        root, _ = legacy.ensure_directory(None, "ROOT")
        _, _ = legacy.ensure_directory(root, "SYSTEM")
        user, _ = legacy.ensure_directory(root, "USR")
        notes, _ = legacy.ensure_directory(user, "NOTES")
        legacy.write_file(notes, "SHORT.TXT", b"fat32 migration data")
        add_legacy_long_name_file(
            legacy, notes, "Long note.txt", b"long-name contents"
        )

        expected = legacy.snapshot_directory()
        migrated = create_disk.migrate_legacy_disk(legacy_image)
        actual = create_disk.Fat32Image(migrated).snapshot_directory()
        self.assertEqual(
            create_disk.normalized_tree(expected),
            create_disk.normalized_tree(actual),
        )

        create_disk.seed_system_files(
            migrated, ImageFile(fake_pe()), ImageFile(fake_pe())
        )
        filesystem = create_disk.Fat32Image(migrated)
        migrated_tree = filesystem.snapshot_directory()
        self.assertEqual(
            find_snapshot(migrated_tree, ["ROOT", "USR", "NOTES", "SHORT.TXT"])[2],
            b"fat32 migration data",
        )
        self.assertEqual(
            find_snapshot(migrated_tree, ["ROOT", "USR", "NOTES", "Long note.txt"])[2],
            b"long-name contents",
        )


if __name__ == "__main__":
    unittest.main()
