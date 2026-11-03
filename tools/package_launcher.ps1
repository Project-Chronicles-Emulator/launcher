param(
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [string]$DistributionProfile
)

$ErrorActionPreference = 'Stop'
$emulatorRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $emulatorRoot 'out/build/windows'
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $emulatorRoot 'out/distribution'
}

$buildRoot = [System.IO.Path]::GetFullPath($BuildDirectory)
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$executable = Join-Path $buildRoot 'bin/Release/launcher/chronicles-launcher.exe'

if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw 'Build chronicles-launcher in Release before packaging.'
}

if ([string]::IsNullOrWhiteSpace($DistributionProfile) -or
    -not (Test-Path -LiteralPath $DistributionProfile -PathType Leaf)) {
    throw 'A distribution profile is required for a player package.'
}

$profile = Get-Content -LiteralPath $DistributionProfile -Raw | ConvertFrom-Json
$origin = [Uri]$profile.server_url
if (-not $origin.IsAbsoluteUri -or
    ($origin.Scheme -ne 'https' -and -not ($origin.Scheme -eq 'http' -and $origin.IsLoopback)) -or
    $profile.patch_public_key -notmatch '^[a-f0-9]{64}$' -or
    $profile.patch_channel -notmatch '^[a-z0-9-]{1,32}$') {
    throw 'The distribution profile must pin a valid API origin, public key and patch channel.'
}

$packageName = 'chronicles-launcher-win64-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$packageDirectory = Join-Path $outputRoot $packageName
$archivePath = $packageDirectory + '.zip'

# Never merge into an earlier release or collect ignored runtime state.
if ((Test-Path -LiteralPath $packageDirectory) -or (Test-Path -LiteralPath $archivePath)) {
    throw "Package destination already exists: $packageDirectory"
}

New-Item -ItemType Directory -Path $packageDirectory | Out-Null
& cmake --install $buildRoot --config Release --component Launcher --prefix $packageDirectory
if ($LASTEXITCODE -ne 0) {
    throw "CMake launcher installation failed with exit code $LASTEXITCODE"
}

$expectedFiles = @(
    'chronicles-launcher.exe',
    'chronicles-world-tls-local.exe',
    'resources/legacy-game-runtime-files.json',
    'prerequisites/MicrosoftEdgeWebview2Setup.exe',
    'WebView2Loader.dll',
    'libcrypto-3-x64.dll',
    'web/index.html',
    'web/app.css',
    'web/app.js',
    'web/portal.js',
    'web/assets/world-harbor.png',
    'web/assets/fonts/InterVariable.woff2',
    'web/assets/fonts/Cinzel-Variable.ttf',
    'web/assets/fonts/Inter-LICENSE.txt',
    'web/assets/fonts/Cinzel-OFL.txt',
    'DISTRIBUTION.md'
)

foreach ($relativePath in $expectedFiles) {
    $packageFile = Join-Path $packageDirectory $relativePath
    if (-not (Test-Path -LiteralPath $packageFile -PathType Leaf)) {
        throw "Required package file is missing: $relativePath"
    }
}

$profile | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $packageDirectory 'launcher-config.json') -Encoding utf8

$forbiddenFiles = Get-ChildItem -LiteralPath $packageDirectory -Recurse -File | Where-Object {
    $_.Name -match '(?i)^(playsharp|.*\.sys$|.*mcp.*\.exe$)'
}
if ($forbiddenFiles) {
    throw 'The player package contains a bridge, MCP executable or driver.'
}

Compress-Archive -LiteralPath $packageDirectory -DestinationPath $archivePath
$hash = Get-FileHash -LiteralPath $archivePath -Algorithm SHA256

Write-Output "Launcher directory: $packageDirectory"
Write-Output "Launcher archive: $archivePath"
Write-Output "SHA256: $($hash.Hash)"
