#requires -Version 7.2

param(
    [Parameter(Mandatory = $true)]
    [string]$VcpkgRoot,

    [string]$SourceRepository = 'https://gitlab.com/Mr_Goldberg/goldberg_emulator.git',

    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$emulatorRoot = Split-Path -Parent $PSScriptRoot
$upstreamCommit = '475342f0d8b2bd7eb0d93bd7cfdd61e3ae7cda24'
$patchPath = Join-Path $emulatorRoot 'launcher/steam_compat/goldberg-v161.patch'
$launcherSourceDirectory = Join-Path $emulatorRoot 'launcher/src'
$zoneUnlockSources = @(
    'zone_unlock.hpp',
    'zone_unlock.cpp',
    'zone_unlock_inproc.hpp',
    'zone_unlock_inproc.cpp',
    'process_memory.hpp',
    'encoding.hpp',
    'encoding.cpp'
)
$vcpkgToolchain = Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake'
$installedDirectory = Join-Path $emulatorRoot 'out/build/windows/vcpkg_installed'
$dependencyDirectory = Join-Path $installedDirectory 'x64-windows/bin'

if (-not (Test-Path -LiteralPath $vcpkgToolchain -PathType Leaf)) {
    throw 'The vcpkg toolchain is missing. Pass the existing vcpkg root.'
}
if (-not (Test-Path -LiteralPath $patchPath -PathType Leaf)) {
    throw 'The reviewed Steam compatibility patch is missing.'
}
foreach ($dependency in @('libprotobuf.dll', 'abseil_dll.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $dependencyDirectory $dependency) -PathType Leaf)) {
        throw "The pinned vcpkg dependency is missing: $dependency"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $name = 'steam-compat-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
    $OutputDirectory = Join-Path $emulatorRoot (Join-Path 'out' $name)
}

$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputRoot) {
    throw "The output directory already exists: $outputRoot"
}

$sourceRoot = Join-Path $outputRoot 'source'
$buildRoot = Join-Path $outputRoot 'build'
$payloadRoot = Join-Path $outputRoot 'payload'
$steamDirectory = Join-Path $payloadRoot 'Engine/Binaries/ThirdParty/Steamworks/Steamv161/Win64'
$clientDirectory = Join-Path $payloadRoot 'X5/Binaries/Win64'
$settingsDirectory = Join-Path $steamDirectory 'steam_settings'
$licenseDirectory = Join-Path $payloadRoot 'licenses'
$sourceDirectory = Join-Path $payloadRoot 'source'

New-Item -ItemType Directory -Path $outputRoot | Out-Null
& git clone --no-checkout $SourceRepository $sourceRoot
if ($LASTEXITCODE -ne 0) {
    throw 'Could not clone the pinned Steam compatibility source.'
}
& git -C $sourceRoot checkout --detach $upstreamCommit
if ($LASTEXITCODE -ne 0) {
    throw 'The pinned Steam compatibility revision is unavailable.'
}
& git -C $sourceRoot apply --check $patchPath
if ($LASTEXITCODE -ne 0) {
    throw 'The reviewed Steam compatibility patch does not apply.'
}
& git -C $sourceRoot apply $patchPath
if ($LASTEXITCODE -ne 0) {
    throw 'Could not apply the reviewed Steam compatibility patch.'
}

# The in-process zone unlock reuses the launcher's reviewed sources, so there is one copy of the
# researched client layout. The library build compiles every dll/*.cpp file.
foreach ($sharedSource in $zoneUnlockSources) {
    $sharedSourcePath = Join-Path $launcherSourceDirectory $sharedSource
    if (-not (Test-Path -LiteralPath $sharedSourcePath -PathType Leaf)) {
        throw "The zone unlock source is missing: $sharedSource"
    }
    Copy-Item -LiteralPath $sharedSourcePath -Destination (Join-Path $sourceRoot 'dll')
}

. (Join-Path $PSScriptRoot 'dev_shell.ps1')

& cmake -S $sourceRoot -B $buildRoot -G 'Ninja Multi-Config' `
    '-DCMAKE_CXX_COMPILER=cl.exe' '-DCMAKE_C_COMPILER=cl.exe' `
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain" `
    "-DVCPKG_INSTALLED_DIR=$installedDirectory" `
    '-DVCPKG_MANIFEST_MODE=OFF' `
    '-DVCPKG_TARGET_TRIPLET=x64-windows'
if ($LASTEXITCODE -ne 0) {
    throw 'Steam compatibility configuration failed.'
}
& cmake --build $buildRoot --config Release --target steam_api64 --parallel 4
if ($LASTEXITCODE -ne 0) {
    throw 'Steam compatibility build failed.'
}

$builtLibrary = Join-Path $buildRoot 'Release/steam_api64.dll'
if (-not (Test-Path -LiteralPath $builtLibrary -PathType Leaf)) {
    throw "The Steam compatibility build did not produce $builtLibrary."
}

New-Item -ItemType Directory -Path $settingsDirectory,$clientDirectory,$licenseDirectory,$sourceDirectory | Out-Null
Copy-Item -LiteralPath $builtLibrary -Destination (Join-Path $steamDirectory 'steam_api64.dll')
foreach ($dependency in @('libprotobuf.dll', 'abseil_dll.dll')) {
    $binary = Join-Path $dependencyDirectory $dependency
    Copy-Item -LiteralPath $binary -Destination (Join-Path $steamDirectory $dependency)
    Copy-Item -LiteralPath $binary -Destination (Join-Path $clientDirectory $dependency)
}

[IO.File]::WriteAllText((Join-Path $settingsDirectory 'steam_appid.txt'), "5280930`n")
[IO.File]::WriteAllBytes((Join-Path $settingsDirectory 'disable_networking.txt'), [byte[]]@())
[IO.File]::WriteAllBytes((Join-Path $settingsDirectory 'disable_overlay.txt'), [byte[]]@())
Copy-Item -LiteralPath (Join-Path $sourceRoot 'LICENSE') `
    -Destination (Join-Path $licenseDirectory 'goldberg-LGPL-3.0.txt')
Copy-Item -LiteralPath $patchPath -Destination (Join-Path $sourceDirectory 'goldberg-v161.patch')
foreach ($sharedSource in $zoneUnlockSources) {
    Copy-Item -LiteralPath (Join-Path $launcherSourceDirectory $sharedSource) -Destination $sourceDirectory
}
$baseArchive = Join-Path $sourceDirectory 'goldberg-base.zip'
& git -C $sourceRoot archive --format=zip "--output=$baseArchive" HEAD
if ($LASTEXITCODE -ne 0) {
    throw 'Could not archive the corresponding upstream source.'
}

$metadata = [ordered]@{
    upstream_repository = 'https://gitlab.com/Mr_Goldberg/goldberg_emulator'
    upstream_commit = $upstreamCommit
    patch_sha256 = (Get-FileHash -LiteralPath $patchPath -Algorithm SHA256).Hash.ToLowerInvariant()
    steam_api64_sha256 = (Get-FileHash -LiteralPath $builtLibrary -Algorithm SHA256).Hash.ToLowerInvariant()
    game_build = 'v26.10.18.734'
    steam_app_id = '5280930'
    validation = 'Fresh local launcher trial without steam.exe reached the original-client realm and playable world using the guarded user-mode helper. Hosted world entry remains unverified.'
}
$metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $payloadRoot 'compatibility.json') -Encoding utf8

Write-Output "Payload: $payloadRoot"
Write-Output "Steam API SHA256: $($metadata.steam_api64_sha256)"
