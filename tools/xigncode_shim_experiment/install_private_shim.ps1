#requires -Version 7.2

param(
    [Parameter(Mandatory)]
    [ValidateSet('Install', 'Restore', 'Status')]
    [string]$Action,
    [Parameter(Mandatory)]
    [string]$PrivateRoot,
    [string]$Source,
    [string]$ShimPath = (Join-Path $PSScriptRoot '..\..\out\xigncode-shim\Release\x3_x64.xem')
)

$ErrorActionPreference = 'Stop'
$moduleRelativePath = 'X5\Binaries\Win64\XIGNCODE\x3_x64.xem'
$clientRelativePath = 'X5\Binaries\Win64\ArcheAgeChronicles.exe'
$markerName = '.local-client-copy.json'
$backupName = 'x3_x64.xem.local-original-backup'
$stateName = '.x3_x64.local-shim-state.json'

function Get-AbsolutePath {
    param([string]$Path)

    return [IO.Path]::GetFullPath($Path).TrimEnd('\')
}

function Test-PathInside {
    param([string]$Path, [string]$Root)

    $prefix = $Root.TrimEnd('\') + '\'
    return $Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-PlainPath {
    param([string]$Root, [string]$Path)

    if (-not (Test-PathInside $Path $Root) -and
        -not $Path.Equals($Root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes the private client root: $Path"
    }

    $current = $Path
    while ($current -and ((Test-PathInside $current $Root) -or
            $current.Equals($Root, [StringComparison]::OrdinalIgnoreCase))) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Private client path is a reparse point: $current"
            }
        }

        if ($current.Equals($Root, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }

        $current = [IO.Path]::GetDirectoryName($current)
    }
}

function Get-FileSha256 {
    param([string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Hash {
    param([string]$Path, [string]$ExpectedHash)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }

    $actualHash = Get-FileSha256 $Path
    if (-not $actualHash.Equals($ExpectedHash, [StringComparison]::OrdinalIgnoreCase)) {
        throw "File hash differs from the private-copy record: $Path"
    }
}

function Get-ManifestEntry {
    param([object]$Manifest, [string]$RelativePath)

    $entries = @($Manifest.files | Where-Object {
            $_.relative_path.Equals($RelativePath, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($entries.Count -ne 1 -or $entries[0].sha256 -notmatch '^[a-fA-F0-9]{64}$') {
        throw "Private marker lacks a unique file entry: $RelativePath"
    }

    return $entries[0]
}

function Write-State {
    param([string]$Path, [object]$State)

    $temporaryPath = "$Path.pending"
    $State | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $temporaryPath -Encoding utf8
    Move-Item -LiteralPath $temporaryPath -Destination $Path -Force
}

$privatePath = Get-AbsolutePath $PrivateRoot
if (-not $Source) {
    $marker = Join-Path $privatePath $markerName
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        throw 'Supply -Source or an existing private-client marker.'
    }
    $Source = (Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json).source
    if (-not $Source) {
        throw 'The private-client marker does not identify its source.'
    }
}
$sourcePath = Get-AbsolutePath $Source
$shimFile = Get-AbsolutePath $ShimPath
$modulePath = Get-AbsolutePath (Join-Path $privatePath $moduleRelativePath)
$sourceModulePath = Get-AbsolutePath (Join-Path $sourcePath $moduleRelativePath)
$privateClientPath = Get-AbsolutePath (Join-Path $privatePath $clientRelativePath)
$markerPath = Get-AbsolutePath (Join-Path $privatePath $markerName)
$moduleDirectory = [IO.Path]::GetDirectoryName($modulePath)
$backupPath = Get-AbsolutePath (Join-Path $moduleDirectory $backupName)
$statePath = Get-AbsolutePath (Join-Path $moduleDirectory $stateName)
$rollbackPath = Get-AbsolutePath (Join-Path $moduleDirectory 'x3_x64.xem.local-replace-rollback')

if ($privatePath.Equals($sourcePath, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-PathInside $privatePath $sourcePath) -or
    (Test-PathInside $sourcePath $privatePath)) {
    throw 'Source and private client must be separate, non-nested directories'
}

if (-not (Test-Path -LiteralPath $privatePath -PathType Container)) {
    throw 'Private client root is missing'
}

foreach ($path in @($privatePath, $markerPath, $privateClientPath,
        $moduleDirectory, $modulePath, $backupPath, $statePath, $rollbackPath)) {
    Assert-PlainPath $privatePath $path
}

if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) {
    throw 'Private client marker is missing'
}

$manifest = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
if ($manifest.version -ne 1 -or $manifest.pak_mode -notin @('Copy', 'ReadOnlyLink') -or
    $manifest.client_executable_sha256 -notmatch '^[a-fA-F0-9]{64}$') {
    throw 'Private client marker is invalid'
}

$recordedSource = Get-AbsolutePath $manifest.source
if (-not $recordedSource.Equals($sourcePath, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Private client marker source differs from the expected source'
}

$moduleEntry = Get-ManifestEntry $manifest $moduleRelativePath
$clientEntry = Get-ManifestEntry $manifest $clientRelativePath
if (-not $clientEntry.sha256.Equals($manifest.client_executable_sha256,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Private marker has inconsistent client executable hashes'
}

Assert-Hash $privateClientPath $manifest.client_executable_sha256

if ($Action -eq 'Install') {
    # Installation checks the source snapshot. Status/restore validate the private
    # executable, original backup and installed module below; Steam may update its
    # separate source installation without invalidating this existing private copy.
    Assert-Hash $sourceModulePath $moduleEntry.sha256
    if (Test-Path -LiteralPath $statePath) {
        throw 'A shim state record already exists; inspect Status or Restore first'
    }
    if (Test-Path -LiteralPath $backupPath) {
        throw 'An original-module backup already exists; refusing to overwrite it'
    }
    if (Test-Path -LiteralPath $rollbackPath) {
        throw 'A module replacement rollback file already exists'
    }
    if (-not $shimFile.StartsWith(
            (Get-AbsolutePath (Join-Path $PSScriptRoot '..\..\out')) + '\',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The experimental shim must come from this emulator output directory'
    }
    if ([IO.Path]::GetFileName($shimFile) -ne 'x3_x64.xem') {
        throw 'The experimental shim must be named x3_x64.xem'
    }

    Assert-Hash $modulePath $moduleEntry.sha256
    if (-not (Test-Path -LiteralPath $shimFile -PathType Leaf)) {
        throw 'Built experimental shim is missing'
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $python) {
        throw 'Python is required to verify the experimental bootstrap marker'
    }

    $verifier = Join-Path $PSScriptRoot 'verify_bootstrap_integrity.py'
    & $python.Source $verifier --require-match $shimFile | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'Experimental shim has an invalid bootstrap marker'
    }

    $shimHash = Get-FileSha256 $shimFile
    if ($shimHash -eq $moduleEntry.sha256) {
        throw 'The selected shim is identical to the original module'
    }

    Copy-Item -LiteralPath $modulePath -Destination $backupPath -ErrorAction Stop
    Assert-Hash $backupPath $moduleEntry.sha256

    $state = [PSCustomObject]@{
        version = 1
        phase = 'prepared'
        source = $sourcePath
        private_root = $privatePath
        original_sha256 = $moduleEntry.sha256.ToLowerInvariant()
        shim_sha256 = $shimHash
    }
    Write-State $statePath $state

    $stagingPath = Get-AbsolutePath (Join-Path $moduleDirectory 'x3_x64.xem.local-staged')
    Assert-PlainPath $privatePath $stagingPath
    if (Test-Path -LiteralPath $stagingPath) {
        throw 'A staged shim file already exists; inspect the private directory'
    }

    Copy-Item -LiteralPath $shimFile -Destination $stagingPath -ErrorAction Stop
    Assert-Hash $stagingPath $shimHash
    [IO.File]::Replace($stagingPath, $modulePath, $rollbackPath, $true)
    Assert-Hash $modulePath $shimHash
    Assert-Hash $rollbackPath $moduleEntry.sha256
    Remove-Item -LiteralPath $rollbackPath -Force

    $state.phase = 'installed'
    Write-State $statePath $state
} else {
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
        if ($Action -eq 'Restore') {
            throw 'No shim state record exists; refusing to replace the module'
        }

        Assert-Hash $modulePath $moduleEntry.sha256
        [PSCustomObject]@{
            Action = 'Status'
            State = 'original'
            PrivateRoot = $privatePath
            ModuleSha256 = $moduleEntry.sha256
        }
        return
    }

    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if ($state.version -ne 1 -or $state.phase -notin @('prepared', 'installed') -or
        -not $state.source.Equals($sourcePath, [StringComparison]::OrdinalIgnoreCase) -or
        -not $state.private_root.Equals($privatePath, [StringComparison]::OrdinalIgnoreCase) -or
        -not $state.original_sha256.Equals($moduleEntry.sha256,
            [StringComparison]::OrdinalIgnoreCase) -or
        $state.shim_sha256 -notmatch '^[a-fA-F0-9]{64}$') {
        throw 'Shim state record is inconsistent with the private marker'
    }

    Assert-Hash $backupPath $moduleEntry.sha256
    $currentHash = Get-FileSha256 $modulePath
    if ($currentHash -notin @($state.original_sha256, $state.shim_sha256)) {
        throw 'Private module changed unexpectedly; refusing to overwrite it'
    }

    if ($Action -eq 'Status') {
        $currentState = if ($currentHash -eq $state.shim_sha256) {
            'experimental-shim'
        } else {
            'original-with-pending-state'
        }

        [PSCustomObject]@{
            Action = 'Status'
            State = $currentState
            PrivateRoot = $privatePath
            ModuleSha256 = $currentHash
        }
        return
    }

    if ($currentHash -eq $state.shim_sha256) {
        if (Test-Path -LiteralPath $rollbackPath) {
            throw 'A module replacement rollback file already exists'
        }

        $stagingPath = Get-AbsolutePath (Join-Path $moduleDirectory 'x3_x64.xem.local-staged')
        Assert-PlainPath $privatePath $stagingPath
        if (Test-Path -LiteralPath $stagingPath) {
            throw 'A staged module file already exists; inspect the private directory'
        }

        Copy-Item -LiteralPath $backupPath -Destination $stagingPath -ErrorAction Stop
        Assert-Hash $stagingPath $moduleEntry.sha256
        [IO.File]::Replace($stagingPath, $modulePath, $rollbackPath, $true)
        Assert-Hash $modulePath $moduleEntry.sha256
        Assert-Hash $rollbackPath $state.shim_sha256
        Remove-Item -LiteralPath $rollbackPath -Force
    }

    Remove-Item -LiteralPath $statePath -Force
    Remove-Item -LiteralPath $backupPath -Force
}

[PSCustomObject]@{
    Action = $Action
    State = if ($Action -eq 'Install') { 'experimental-shim' } else { 'original' }
    PrivateRoot = $privatePath
    ModuleSha256 = Get-FileSha256 $modulePath
}
