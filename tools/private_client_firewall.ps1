#requires -Version 7.2

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Enable', 'Disable', 'Status')]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [string]$PrivateRoot
)

$ErrorActionPreference = 'Stop'

function Get-RuleGroup {
    param([string]$Root)

    $normalizedRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\').ToLowerInvariant()
    $pathBytes = [Text.Encoding]::UTF8.GetBytes($normalizedRoot)
    $digest = [Security.Cryptography.SHA256]::HashData($pathBytes)
    $suffix = [Convert]::ToHexString($digest).Substring(0, 16)

    return "ArcheAgeLocalClient-$suffix"
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)

    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator privileges are required to manage Windows Firewall rules.'
    }
}

function Get-PrivatePrograms {
    param([string]$Root)

    $markerPath = Join-Path $Root '.local-client-copy.json'
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) {
        throw "Private-client marker is missing: $markerPath"
    }

    $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
    if ($marker.version -ne 1 -or -not $marker.source) {
        throw "Private-client marker is invalid: $markerPath"
    }

    $sourceRoot = [IO.Path]::GetFullPath([string]$marker.source).TrimEnd('\')
    if ($Root.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The private client root must differ from the installed game root.'
    }

    $clientExecutable = Join-Path $Root 'X5\Binaries\Win64\ArcheAgeChronicles.exe'
    if (-not (Test-Path -LiteralPath $clientExecutable -PathType Leaf)) {
        throw "Private game executable is missing: $clientExecutable"
    }

    $actualHash = (Get-FileHash -LiteralPath $clientExecutable -Algorithm SHA256).Hash
    if (-not $actualHash.Equals(
            [string]$marker.client_executable_sha256,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The private executable does not match its preparation marker.'
    }

    $programs = @(
        Get-ChildItem -LiteralPath $Root -Recurse -File |
            Where-Object { $_.Extension -in @('.exe', '.xem') } |
            Sort-Object -Property FullName
    )
    if ($programs.Count -eq 0) {
        throw 'No private executables were found.'
    }

    return $programs
}

$resolvedRoot = [IO.Path]::GetFullPath($PrivateRoot).TrimEnd('\')
$ruleGroup = Get-RuleGroup -Root $resolvedRoot
$existingRules = @(Get-NetFirewallRule -Group $ruleGroup -ErrorAction SilentlyContinue)

if ($Action -eq 'Status') {
    [pscustomobject]@{
        PrivateRoot = $resolvedRoot
        RuleGroup = $ruleGroup
        RuleCount = $existingRules.Count
        Enabled = $existingRules.Count -gt 0 -and
            @($existingRules | Where-Object { $_.Enabled -ne 'True' }).Count -eq 0
    }
    return
}

Assert-Administrator

if ($Action -eq 'Disable') {
    if ($existingRules.Count -gt 0) {
        $existingRules | Remove-NetFirewallRule
    }

    Write-Output "Removed $($existingRules.Count) private-client firewall rules."
    return
}

if ($existingRules.Count -gt 0) {
    throw "Firewall rules already exist for this private root. Inspect with -Action Status first."
}

$programs = @(Get-PrivatePrograms -Root $resolvedRoot)
try {
    foreach ($program in $programs) {
        $displayName = "ArcheAge local only: $($program.Name)"
        New-NetFirewallRule -DisplayName $displayName `
            -Group $ruleGroup `
            -Direction Outbound `
            -Program $program.FullName `
            -RemoteAddress Any `
            -Action Block `
            -Profile Any | Out-Null
    }

    $activeRules = @(Get-NetFirewallRule -Group $ruleGroup -ErrorAction Stop)
    $coveredPaths = @(
        $activeRules | Get-NetFirewallApplicationFilter | Select-Object -ExpandProperty Program
    )
    $missingPaths = @(
        $programs | Where-Object { $_.FullName -notin $coveredPaths }
    )
    if ($activeRules.Count -ne $programs.Count -or $missingPaths.Count -gt 0) {
        throw 'Firewall rules did not cover every private executable.'
    }
} catch {
    $partialRules = @(Get-NetFirewallRule -Group $ruleGroup -ErrorAction SilentlyContinue)
    if ($partialRules.Count -gt 0) {
        $partialRules | Remove-NetFirewallRule -ErrorAction SilentlyContinue
    }

    throw
}

Write-Output "Blocked external traffic for $($programs.Count) private-client programs."
Write-Output "Windows Firewall loopback behavior was tested locally; verify these rules before launch."
