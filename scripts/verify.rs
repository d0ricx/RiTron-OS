use std::env;
use std::fs;
use std::path::Path;

const SECTOR_SIZE: usize = 512;
const FAT32_SIZE: usize = 64 * 1024 * 1024;
const VOLUME_ID: u32 = 0x4452_5446;

// Keep parsed geometry alongside a borrowed image; verification remains read-only.
struct FatVolume<'a> {
    image: &'a [u8],
    fat_start: usize,
    fat_sectors: usize,
    data_start: usize,
    root_cluster: usize,
    cluster_count: usize,
    sectors_per_cluster: usize,
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16, String> {
    let slice = bytes
        .get(offset..offset + 2)
        .ok_or_else(|| "truncated 16-bit field".to_string())?;
    Ok(u16::from_le_bytes([slice[0], slice[1]]))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32, String> {
    let slice = bytes
        .get(offset..offset + 4)
        .ok_or_else(|| "truncated 32-bit field".to_string())?;
    Ok(u32::from_le_bytes([slice[0], slice[1], slice[2], slice[3]]))
}

fn require(condition: bool, message: &str) -> Result<(), String> {
    if condition {
        Ok(())
    } else {
        Err(message.to_string())
    }
}

impl<'a> FatVolume<'a> {
    fn new(image: &'a [u8]) -> Result<Self, String> {
        require(image.len() >= SECTOR_SIZE, "disk image is smaller than one sector")?;
        let bytes_per_sector = read_u16(image, 11)? as usize;
        let sectors_per_cluster = image[13] as usize;
        let reserved_sectors = read_u16(image, 14)? as usize;
        let fat_count = image[16] as usize;
        let root_entries = read_u16(image, 17)? as usize;
        let total_sectors_16 = read_u16(image, 19)? as usize;
        let total_sectors = read_u32(image, 32)? as usize;
        let legacy_fat_sectors = read_u16(image, 22)? as usize;
        let fat_sectors = read_u32(image, 36)? as usize;
        let root_cluster = (read_u32(image, 44)? & 0x0FFF_FFFF) as usize;
        let data_start = reserved_sectors
            .checked_add(
                fat_count
                    .checked_mul(fat_sectors)
                    .ok_or_else(|| "FAT region size overflows".to_string())?,
            )
            .ok_or_else(|| "data-region offset overflows".to_string())?;

        require(image[510..512] == [0x55, 0xAA], "invalid FAT boot signature")?;
        require(bytes_per_sector == SECTOR_SIZE, "unsupported sector size")?;
        require(
            sectors_per_cluster.is_power_of_two() && sectors_per_cluster != 0,
            "invalid sectors-per-cluster value",
        )?;
        require(reserved_sectors != 0, "missing reserved sectors")?;
        require(fat_count == 2, "expected two FAT copies")?;
        require(
            total_sectors_16 == 0 && root_entries == 0 && legacy_fat_sectors == 0,
            "expected FAT32 BPB fields",
        )?;
        require(fat_sectors != 0, "missing FAT sectors")?;
        require(
            total_sectors.checked_mul(SECTOR_SIZE) == Some(image.len()),
            "BPB size does not match image",
        )?;
        require(data_start <= total_sectors, "data region starts beyond volume")?;
        require(
            read_u32(image, 67)? == VOLUME_ID,
            "unexpected data-disk volume ID",
        )?;
        require(
            &image[71..82] == b"KERNELDATA ",
            "unexpected volume label",
        )?;

        let cluster_count = (total_sectors - data_start) / sectors_per_cluster;
        require(
            (65_525..=0x0FFF_FFEE).contains(&cluster_count),
            "volume geometry is not FAT32",
        )?;
        require(
            fat_sectors * SECTOR_SIZE >= (cluster_count + 2) * 4,
            "FAT is too small for the data region",
        )?;
        require(data_start * SECTOR_SIZE <= image.len(), "data region is outside disk")?;
        require(image.len() == FAT32_SIZE, "FAT32 data disk must be 64 MiB")?;
        require(root_cluster >= 2 && root_cluster < cluster_count + 2,
                "invalid FAT32 root cluster")?;
        require(read_u16(image, 40)? & 0x0080 == 0,
                "active-FAT mode is unsupported")?;
        require(read_u16(image, 42)? == 0, "unsupported FAT32 version")?;

        let filesystem = Self {
            image,
            fat_start: reserved_sectors * SECTOR_SIZE,
            fat_sectors,
            data_start,
            root_cluster,
            cluster_count,
            sectors_per_cluster,
        };
        filesystem.verify_fat_copies()?;
        let first = filesystem.fat_entry(0)?;
        let second = filesystem.fat_entry(1)?;
        require(
            first >= 0x0FFF_FFF8 && second >= 0x0FFF_FFF8,
            "invalid reserved FAT entries",
        )?;
        Ok(filesystem)
    }

    fn verify_fat_copies(&self) -> Result<(), String> {
        // A disagreement can make the kernel and host tools see different chains.
        let copy_size = self.fat_sectors * SECTOR_SIZE;
        let first = &self.image[self.fat_start..self.fat_start + copy_size];
        let second_start = self.fat_start + copy_size;
        let second = &self.image[second_start..second_start + copy_size];
        require(first == second, "FAT copies do not match")
    }

    fn fat_entry(&self, cluster: usize) -> Result<u32, String> {
        let offset = self
            .fat_start
            .checked_add(
                cluster
                    .checked_mul(4)
                    .ok_or_else(|| "cluster index overflows".to_string())?,
            )
            .ok_or_else(|| "FAT entry offset overflows".to_string())?;
        Ok(read_u32(self.image, offset)? & 0x0FFF_FFFF)
    }

    fn cluster_offset(&self, cluster: usize) -> Result<usize, String> {
        require(
            (2..self.cluster_count + 2).contains(&cluster),
            "directory or file contains an invalid cluster",
        )?;
        let sector = self
            .data_start
            .checked_add((cluster - 2) * self.sectors_per_cluster)
            .ok_or_else(|| "cluster location overflows".to_string())?;
        Ok(sector * SECTOR_SIZE)
    }

    fn directory_entries(&self, first_cluster: usize) -> Result<Vec<[u8; 32]>, String> {
        let mut entries = Vec::new();
        // Track visited clusters so malformed loops fail instead of hanging CI.
        let mut cluster = first_cluster;
        let mut visited = vec![false; self.cluster_count + 2];
        loop {
            require(
                (2..self.cluster_count + 2).contains(&cluster),
                "invalid directory cluster chain",
            )?;
            require(!visited[cluster], "directory cluster chain loops")?;
            visited[cluster] = true;
            let offset = self.cluster_offset(cluster)?;
            let cluster_size = self.sectors_per_cluster * SECTOR_SIZE;
            let end = offset + cluster_size;
            for entry in self.image[offset..end].chunks_exact(32) {
                entries.push(entry.try_into().expect("entry size is fixed"));
            }
            let next = self.fat_entry(cluster)? as usize;
            if next >= 0x0FFF_FFF8 {
                break;
            }
            cluster = next;
        }
        Ok(entries)
    }

    fn find_entry(
        &self,
        directory: usize,
        short_name: &[u8; 11],
    ) -> Result<[u8; 32], String> {
        for entry in self.directory_entries(directory)? {
            if entry[0] == 0 {
                break;
            }
            if entry[0] == 0xE5 || entry[11] == 0x0F {
                continue;
            }
            if &entry[..11] == short_name {
                return Ok(entry);
            }
        }
        Err(format!(
            "missing FAT directory entry {}",
            String::from_utf8_lossy(short_name)
        ))
    }

    fn entry_cluster(&self, entry: &[u8; 32]) -> usize {
        let low = u16::from_le_bytes([entry[26], entry[27]]) as u32;
        ((low | ((u16::from_le_bytes([entry[20], entry[21]]) as u32) << 16))
            & 0x0FFF_FFFF) as usize
    }

    fn file_bytes(&self, entry: &[u8; 32]) -> Result<Vec<u8>, String> {
        let file_size = read_u32(entry, 28)? as usize;
        let mut remaining = file_size;
        let mut cluster = self.entry_cluster(entry);
        let mut visited = vec![false; self.cluster_count + 2];
        let mut bytes = Vec::with_capacity(file_size);

        // Validate every chain link while reading only the declared file length.
        require(file_size != 0, "system EFI file is empty")?;
        while remaining != 0 {
            require(
                (2..self.cluster_count + 2).contains(&cluster),
                "invalid system file cluster chain",
            )?;
            require(!visited[cluster], "system file cluster chain loops")?;
            visited[cluster] = true;
            let offset = self.cluster_offset(cluster)?;
            let take = remaining.min(self.sectors_per_cluster * SECTOR_SIZE);
            bytes.extend_from_slice(&self.image[offset..offset + take]);
            remaining -= take;
            if remaining != 0 {
                let next = self.fat_entry(cluster)? as usize;
                require(
                    (2..self.cluster_count + 2).contains(&next)
                        || next >= 0x0FFF_FFF8,
                    "system file cluster chain ends prematurely",
                )?;
                cluster = next;
            }
        }
        Ok(bytes)
    }
}

fn verify(path: &Path) -> Result<(), String> {
    // Verify the exact on-disk contract consumed by ATA/filesystem initialization.
    let image = fs::read(path).map_err(|error| format!("cannot read disk image: {error}"))?;
    let filesystem = FatVolume::new(&image)?;
    let volume_root = filesystem.root_cluster;
    let volume_label = filesystem.find_entry(volume_root, b"KERNELDATA ")?;
    require(volume_label[11] & 0x08 != 0, "missing FAT volume-label entry")?;

    let root = filesystem.find_entry(volume_root, b"ROOT       ")?;
    require(root[11] & 0x10 != 0, "ROOT entry is not a directory")?;
    let root_cluster = filesystem.entry_cluster(&root);
    let system = filesystem.find_entry(root_cluster, b"SYSTEM     ")?;
    let usr = filesystem.find_entry(root_cluster, b"USR        ")?;
    require(system[11] & 0x10 != 0, "SYSTEM entry is not a directory")?;
    require(usr[11] & 0x10 != 0, "USR entry is not a directory")?;

    let system_cluster = filesystem.entry_cluster(&system);
    let bootloader = filesystem.find_entry(system_cluster, b"BOOTX64 EFI")?;
    let kernel = filesystem.find_entry(system_cluster, b"KERNEL  EFI")?;
    require(bootloader[11] & 0x10 == 0, "BOOTX64.EFI is not a file")?;
    require(kernel[11] & 0x10 == 0, "KERNEL.EFI is not a file")?;

    for (name, entry) in [("BOOTX64.EFI", bootloader), ("KERNEL.EFI", kernel)] {
        let bytes = filesystem.file_bytes(&entry)?;
        require(bytes.starts_with(b"MZ"), "system file is not a PE image")?;
        let pe_offset = read_u32(&bytes, 0x3C)? as usize;
        require(
            bytes.get(pe_offset..pe_offset + 4) == Some(b"PE\0\0"),
            "system file has an invalid PE signature",
        )
        .map_err(|error| format!("{name}: {error}"))?;
    }

    println!(
        "Verified {}: FAT32, {} clusters, ROOT/SYSTEM/USR, and both EFI images",
        path.display(),
        filesystem.cluster_count
    );
    Ok(())
}

fn main() {
    let mut args = env::args_os();
    let program = args.next().unwrap_or_default();
    let Some(path) = args.next() else {
        eprintln!("Usage: {} <data-disk.img>", Path::new(&program).display());
        std::process::exit(2);
    };
    if args.next().is_some() {
        eprintln!("Usage: {} <data-disk.img>", Path::new(&program).display());
        std::process::exit(2);
    }
    if let Err(error) = verify(Path::new(&path)) {
        eprintln!("Disk verification failed: {error}");
        std::process::exit(1);
    }
}
