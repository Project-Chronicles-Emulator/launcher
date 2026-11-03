#requires -Version 7.2

param(
    [Parameter(Mandatory = $true)]
    [string]$PrivateRoot,

    [Parameter(Mandatory = $true)]
    [string]$BootstrapArchive,

    [string]$SteamAppId = '5280930',

    [switch]$CheckOnly,

    # Opt-in: the client keeps %LOCALAPPDATA%\X5\Saved_<suffix> instead of the shared Saved
    # folder. The engine drops every character outside [_0-9A-Za-z], so only those are accepted.
    [ValidatePattern('^[_0-9A-Za-z]+$')]
    [string]$SavedDirSuffix
)

$ErrorActionPreference = 'Stop'

function Assert-LocalClient {
    param([string]$Root)

    $shimHelper = Join-Path $PSScriptRoot 'xigncode_shim_experiment\install_private_shim.ps1'
    $shimStatus = & $shimHelper -Action Status -PrivateRoot $Root
    if ($shimStatus.State -ne 'experimental-shim') {
        throw 'The private client does not have the verified local shim installed.'
    }

    $firewallHelper = Join-Path $PSScriptRoot 'private_client_firewall.ps1'
    $firewallStatus = & $firewallHelper -Action Status -PrivateRoot $Root
    if (-not $firewallStatus.Enabled) {
        throw 'The private client firewall group is not enabled.'
    }

    $executable = Join-Path $Root 'X5\Binaries\Win64\ArcheAgeChronicles.exe'
    $rules = @(Get-NetFirewallRule -Group $firewallStatus.RuleGroup)
    $invalidRules = @(
        $rules | Where-Object {
            $_.Enabled -ne 'True' -or $_.Action -ne 'Block' -or
            $_.Direction -ne 'Outbound' -or $_.Profile -ne 'Any'
        }
    )
    if ($invalidRules.Count -gt 0) {
        throw 'The private firewall group must block outbound traffic on every profile.'
    }

    $addressFilters = @($rules | Get-NetFirewallAddressFilter)
    $limitedRules = @(
        $addressFilters | Where-Object { @($_.RemoteAddress) -notcontains 'Any' }
    )
    if ($addressFilters.Count -ne $rules.Count -or $limitedRules.Count -gt 0) {
        throw 'The private firewall group does not block every remote address.'
    }

    $portFilters = @($rules | Get-NetFirewallPortFilter)
    $limitedPorts = @(
        $portFilters | Where-Object {
            $_.Protocol -ne 'Any' -or $_.LocalPort -ne 'Any' -or
            $_.RemotePort -ne 'Any'
        }
    )
    if ($portFilters.Count -ne $rules.Count -or $limitedPorts.Count -gt 0) {
        throw 'The private firewall group does not block every protocol and port.'
    }

    $blockedPrograms = @(
        $rules | Get-NetFirewallApplicationFilter | Select-Object -ExpandProperty Program
    )
    $privatePrograms = @(
        Get-ChildItem -LiteralPath $Root -Recurse -File |
            Where-Object { $_.Extension -in @('.exe', '.xem') }
    )
    $uncoveredPrograms = @(
        $privatePrograms | Where-Object { $_.FullName -notin $blockedPrograms }
    )
    if ($uncoveredPrograms.Count -gt 0 -or $executable -notin $blockedPrograms) {
        throw 'The private firewall group does not cover every game executable and module.'
    }

    return $executable
}

$root = [IO.Path]::GetFullPath($PrivateRoot).TrimEnd('\')
$archive = [IO.Path]::GetFullPath($BootstrapArchive)
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    throw "Bootstrap archive is missing: $archive"
}
if ($SteamAppId -notmatch '^[0-9]+$') {
    throw 'SteamAppId must contain only digits.'
}

$executable = Assert-LocalClient -Root $root
if ($CheckOnly) {
    [pscustomobject]@{
        PrivateRoot = $root
        Executable = $executable
        Ready = $true
    }
    return
}

# Steam initializes its API from these values when the private executable is started directly.
$env:SteamAppId = $SteamAppId
$env:SteamGameId = $SteamAppId
$workingDirectory = Split-Path -Parent $executable
$arguments = @(
    '-textconfig'
    ('"-IniBootstrap=' + $archive + '"')
)
if ($SavedDirSuffix) {
    $arguments += "-saveddirsuffix=$SavedDirSuffix"
    $arguments += "-enginesaveddirsuffix=$SavedDirSuffix"
}

$client = Start-Process -FilePath $executable -ArgumentList $arguments `
    -WorkingDirectory $workingDirectory -PassThru

if ($client.HasExited) {
    throw 'The private game client exited immediately after launch.'
}

# The zone helper uses ordinary Windows user-mode access and inherits this launcher's
# privileges. Run it before world entry so inactive demo zones cannot latch a movement anchor.
$projectRoot = Split-Path -Parent $PSScriptRoot
$explorationProfile = Join-Path $projectRoot 'out\launcher\local-exploration.json'
if (Test-Path -LiteralPath $explorationProfile -PathType Leaf) {
    if ((Get-Item -LiteralPath $explorationProfile).Length -gt 4096) {
        throw 'The local exploration profile is oversized.'
    }
    $exploration = Get-Content -LiteralPath $explorationProfile -Raw | ConvertFrom-Json
    if ($exploration.unlock_all_zones -eq $true) {
        $zoneHelper = Join-Path $projectRoot `
            'out\build\windows\bin\Release\launcher\chronicles-zone-unlock.exe'
        if (-not (Test-Path -LiteralPath $zoneHelper -PathType Leaf)) {
            throw 'Build chronicles-zone-unlock before enabling all-zone exploration.'
        }
        $zoneArguments = @(
            '--pid', $client.Id,
            '--client-directory', ('"' + $root + '"'),
            '--wait-seconds', '20'
        )
        $zoneLog = Join-Path $projectRoot ("out\launcher\zone-unlock-{0}.log" -f $client.Id)
        $zoneProcess = Start-Process -FilePath $zoneHelper -ArgumentList $zoneArguments `
            -WorkingDirectory $projectRoot -WindowStyle Hidden `
            -RedirectStandardOutput $zoneLog -RedirectStandardError ($zoneLog + '.error') `
            -PassThru
        if (-not $zoneProcess.WaitForExit(25000)) {
            throw "All-zone exploration is still pending; inspect $zoneLog"
        }
        if ($zoneProcess.ExitCode -ne 0) {
            throw "All-zone exploration failed; inspect $zoneLog.error"
        }
    }
}

[pscustomobject]@{
    Pid = $client.Id
    PrivateRoot = $root
    Started = $true
}
