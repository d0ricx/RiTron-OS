param(
    [string]$OutputDirectory = "build",
    [string]$Compiler = "g++"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$sourceDirectory = Join-Path $projectRoot "src"
$bootDirectory = Join-Path $sourceDirectory "boot"
$kernelDirectory = Join-Path $sourceDirectory "krnl"
$driverDirectory = Join-Path $sourceDirectory "drv"
$filesystemDirectory = Join-Path $sourceDirectory "fs"
$resolvedOutputDirectory = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory
} else {
    Join-Path $projectRoot $OutputDirectory
}
$flags = @(
    "-ffreestanding",
    "-fno-builtin",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-threadsafe-statics",
    "-fno-use-cxa-atexit",
    "-fno-stack-protector",
    "-fno-stack-check",
    "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables",
    "-mno-red-zone",
    "-fshort-wchar",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-O2",
    "-mgeneral-regs-only",
    "-I$bootDirectory",
    "-I$kernelDirectory",
    "-I$driverDirectory",
    "-I$filesystemDirectory"
)
$efiDirectory = Join-Path $resolvedOutputDirectory "EFI\BOOT"
New-Item -ItemType Directory -Force -Path $efiDirectory | Out-Null

$linkerFlags = @(
    "-nostdlib",
    "-Wl,--subsystem,10",
    "-Wl,--enable-reloc-section"
)
$commonSources = @(
    (Join-Path $kernelDirectory "krnl.cpp"),
    (Join-Path $filesystemDirectory "fat16.cpp"),
    (Join-Path $kernelDirectory "console.cpp"),
    (Join-Path $kernelDirectory "keyboard.cpp"),
    (Join-Path $kernelDirectory "cpu.cpp"),
    (Join-Path $kernelDirectory "memory.cpp"),
    (Join-Path $driverDirectory "ata.cpp"),
    (Join-Path $kernelDirectory "power.cpp"),
    (Join-Path $bootDirectory "uefi.cpp")
)
$bootloaderSource = Join-Path $bootDirectory "boot.cpp"
$kernelTarget = Join-Path $efiDirectory "KERNEL.EFI"
& $Compiler @flags @linkerFlags "-Wl,-e,efi_main" `
    $bootloaderSource @commonSources "-o" $kernelTarget
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build KERNEL.EFI (g++ exit code $LASTEXITCODE)."
}

$bootloaderTarget = Join-Path $efiDirectory "BOOTX64.EFI"
& $Compiler @flags @linkerFlags "-Wl,-e,bootloader_main" `
    $bootloaderSource @commonSources "-o" $bootloaderTarget
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build boot/boot.cpp (g++ exit code $LASTEXITCODE)."
}

Write-Host "Built EFI\BOOT\BOOTX64.EFI and EFI\BOOT\KERNEL.EFI in $resolvedOutputDirectory"
