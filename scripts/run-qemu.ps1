param(
    [string]$Qemu = "qemu-system-x86_64",
    [Parameter(Mandatory = $true)]
    [string]$Iso,
    [Parameter(Mandatory = $true)]
    [string]$Disk,
    [string]$OvmfCode,
    [string]$OvmfVars
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Iso -PathType Leaf)) {
    throw "Bootable ISO not found: $Iso"
}
if (-not (Test-Path -LiteralPath $Disk -PathType Leaf)) {
    throw "FAT data disk not found: $Disk. Run 'make disk' to create it."
}

$qemuCommand = Get-Command $Qemu -ErrorAction SilentlyContinue
if ($null -eq $qemuCommand) {
    throw "QEMU executable not found: $Qemu. Set QEMU to its path or add it to PATH."
}

if ([string]::IsNullOrWhiteSpace($OvmfCode) -or
    [string]::IsNullOrWhiteSpace($OvmfVars)) {
    $qemuDirectory = Split-Path -Parent $qemuCommand.Source
    $qemuPrefix = Split-Path -Parent $qemuDirectory
    $firmwareDirectories = @(
        (Join-Path $qemuPrefix "share\qemu"),
        "C:\msys64\ucrt64\share\qemu",
        "C:\msys64\mingw64\share\qemu",
        "C:\Program Files\qemu\share"
    ) | Select-Object -Unique

    if ([string]::IsNullOrWhiteSpace($OvmfCode)) {
        foreach ($directory in $firmwareDirectories) {
            foreach ($name in @("edk2-x86_64-code.fd", "OVMF_CODE.fd", "OVMF_CODE_4M.fd")) {
                $candidate = Join-Path $directory $name
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    $OvmfCode = $candidate
                    break
                }
            }
            if (-not [string]::IsNullOrWhiteSpace($OvmfCode)) {
                break
            }
        }
    }

    if ([string]::IsNullOrWhiteSpace($OvmfVars)) {
        foreach ($directory in $firmwareDirectories) {
            foreach ($name in @("edk2-x86_64-vars.fd", "edk2-i386-vars.fd", "OVMF_VARS.fd", "OVMF_VARS_4M.fd")) {
                $candidate = Join-Path $directory $name
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    $OvmfVars = $candidate
                    break
                }
            }
            if (-not [string]::IsNullOrWhiteSpace($OvmfVars)) {
                break
            }
        }
    }
}

if ([string]::IsNullOrWhiteSpace($OvmfCode) -or
    -not (Test-Path -LiteralPath $OvmfCode -PathType Leaf)) {
    throw "OVMF code firmware not found. Set OVMF_CODE to an x86_64 OVMF_CODE.fd or edk2-x86_64-code.fd file."
}
if ([string]::IsNullOrWhiteSpace($OvmfVars) -or
    -not (Test-Path -LiteralPath $OvmfVars -PathType Leaf)) {
    throw "OVMF variable-store template not found. Set OVMF_VARS to an OVMF_VARS.fd or edk2-i386-vars.fd file."
}

$codePath = (Resolve-Path -LiteralPath $OvmfCode).Path
$varsPath = (Resolve-Path -LiteralPath $OvmfVars).Path
$isoPath = (Resolve-Path -LiteralPath $Iso).Path
$diskPath = (Resolve-Path -LiteralPath $Disk).Path
$writableVars = Join-Path (Split-Path -Parent $isoPath) "OVMF_VARS.fd"
Copy-Item -LiteralPath $varsPath -Destination $writableVars -Force

& $qemuCommand.Source `
    -machine pc `
    -m 256M `
    -drive "if=pflash,format=raw,readonly=on,file=$codePath" `
    -drive "if=pflash,format=raw,file=$writableVars" `
    -drive "if=none,id=bootcd,format=raw,media=cdrom,file=$isoPath" `
    -device "ide-cd,drive=bootcd,bus=ide.1,bootindex=1" `
    -drive "file=$diskPath,format=raw,if=ide,index=1" `
    -boot order=d `
    -serial stdio `
    -monitor none

if ($LASTEXITCODE -ne 0) {
    throw "QEMU exited with code $LASTEXITCODE."
}
