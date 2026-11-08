param(
    [Parameter(Mandatory = $true)]
    [string]$Disk,
    [string]$Rustc = "rustc"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$diskPath = if ([System.IO.Path]::IsPathRooted($Disk)) {
    $Disk
} else {
    Join-Path $projectRoot $Disk
}
if (-not (Test-Path -LiteralPath $diskPath -PathType Leaf)) {
    throw "FAT16 data disk not found: $diskPath"
}

$rustcCommand = Get-Command $Rustc -ErrorAction SilentlyContinue
if ($null -eq $rustcCommand) {
    $installedRustc = Join-Path $env:USERPROFILE ".cargo\bin\rustc.exe"
    if (Test-Path -LiteralPath $installedRustc -PathType Leaf) {
        $rustcPath = $installedRustc
    } else {
        throw "Rust is required for verification. Install rustc, add it to PATH, or pass -Rustc."
    }
} else {
    $rustcPath = $rustcCommand.Source
}

$toolPath = Join-Path (Split-Path -Parent $diskPath) "verify-disk.exe"
$sourcePath = Join-Path $PSScriptRoot "verify.rs"
& $rustcPath "--edition=2021" "-O" $sourcePath "-o" $toolPath
if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile the Rust disk verifier (rustc exit code $LASTEXITCODE)."
}

& $toolPath $diskPath
if ($LASTEXITCODE -ne 0) {
    throw "FAT16 disk verification failed (exit code $LASTEXITCODE)."
}
