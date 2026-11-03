#requires -Version 7.2
param([string]$BuildDirectory = 'out/build/windows')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
$component = 'launcher'
$package = Join-Path $root "out/release/$component-win64"
if (Test-Path -LiteralPath $package) {
    throw "Package directory already exists: $package. Use a fresh output directory."
}
New-Item -ItemType Directory -Path $package -Force | Out-Null

function Copy-Required {
    param([string]$Source, [string]$Destination = $package)

    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Required release input is missing: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
}

$binary = Join-Path $BuildDirectory 'bin/Release/launcher'
foreach ($filename in @('chronicles-launcher.exe', 'chronicles-world-tls-local.exe', 'chronicles-zone-unlock.exe')) {
    Copy-Required "$binary/$filename"
}
Get-ChildItem -LiteralPath $binary -Filter '*.dll' -File | Copy-Item -Destination $package
Copy-Required 'launcher/web'
Copy-Required 'launcher/resources'
Copy-Required 'config/launcher.local.example.json' "$package/launcher-config.json"
New-Item -ItemType Directory -Path "$package/tools/xigncode_shim_experiment" -Force | Out-Null
foreach ($name in @('prepare_local_client.py', 'binary_config_codec.py', 'launch_private_client.ps1', 'private_client_firewall.ps1')) {
    Copy-Required "tools/$name" "$package/tools"
}
Copy-Required 'tools/xigncode_shim_experiment/install_private_shim.ps1' "$package/tools/xigncode_shim_experiment"

Copy-Required 'README.md'
if (Test-Path -LiteralPath 'LICENSE') {
    Copy-Required 'LICENSE'
}
# Package only explicitly selected binaries, configuration examples and tool source.
# Never copy the checkout, data/generated, private configuration or research exports.
$archive = "$package.zip"
Compress-Archive -Path "$package/*" -DestinationPath $archive
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
Write-Output $archive
