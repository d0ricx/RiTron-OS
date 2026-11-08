param(
    [string]$OutputDirectory = "build",
    [string]$Compiler = "g++",
    [string]$RustCompiler = "rustc"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$sourceDirectory = Join-Path $projectRoot "src"
$bootDirectory = Join-Path $sourceDirectory "boot"
$kernelDirectory = Join-Path $sourceDirectory "krnl"
$consoleDirectory = Join-Path $kernelDirectory "console"
$inputDirectory = Join-Path $kernelDirectory "input"
$architectureDirectory = Join-Path $kernelDirectory "arch"
$memoryDirectory = Join-Path $kernelDirectory "memory"
$powerDirectory = Join-Path $kernelDirectory "power"
$shellDirectory = Join-Path $kernelDirectory "shell"
$networkDirectory = Join-Path $kernelDirectory "net"
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
    "-I$consoleDirectory",
    "-I$inputDirectory",
    "-I$architectureDirectory",
    "-I$memoryDirectory",
    "-I$powerDirectory",
    "-I$shellDirectory",
    "-I$networkDirectory",
    "-I$driverDirectory",
    "-I$filesystemDirectory"
)
$efiDirectory = Join-Path $resolvedOutputDirectory "EFI\BOOT"
$rustOutputDirectory = Join-Path $resolvedOutputDirectory "krnl\net"
New-Item -ItemType Directory -Force -Path $efiDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $rustOutputDirectory | Out-Null

$rustObject = Join-Path $rustOutputDirectory "http.a"
$rustSource = Join-Path $networkDirectory "http.rs"
$rustFlags = @(
    "--target", "x86_64-pc-windows-gnu",
    "--edition", "2021",
    "--crate-type", "staticlib",
    "-C", "panic=abort",
    "-C", "opt-level=2",
    "-C", "no-redzone=yes",
    "-C", "relocation-model=pic"
)
& $RustCompiler @rustFlags $rustSource "-o" $rustObject
if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile the Rust HTTP client (rustc exit code $LASTEXITCODE)."
}

$linkerFlags = @(
    "-nostdlib",
    "-Wl,--subsystem,10",
    "-Wl,--enable-reloc-section"
)
$commonSources = @(
    (Join-Path $shellDirectory "shell.cpp"),
    (Join-Path $filesystemDirectory "fat16.cpp"),
    (Join-Path $consoleDirectory "console.cpp"),
    (Join-Path $inputDirectory "keyboard.cpp"),
    (Join-Path $architectureDirectory "cpu.cpp"),
    (Join-Path $memoryDirectory "memory.cpp"),
    (Join-Path $memoryDirectory "runtime.cpp"),
    (Join-Path $driverDirectory "ata.cpp"),
    (Join-Path $driverDirectory "e1000.cpp"),
    (Join-Path $powerDirectory "power.cpp"),
    (Join-Path $bootDirectory "uefi.cpp"),
    $rustObject
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
