<#
.SYNOPSIS
Inspect Windows/WSL LAN readiness. System changes require explicit switches.
.EXAMPLE
.\security\scripts\setup_lan.ps1
.EXAMPLE
.\security\scripts\setup_lan.ps1 -EnableMirrored
.EXAMPLE
.\security\scripts\setup_lan.ps1 -ConfigureFirewall -LanSubnet 192.168.1.0/24
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [switch]$EnableMirrored,
    [switch]$ConfigureFirewall,
    [string]$LanSubnet
)

$ErrorActionPreference = 'Stop'

function Get-MirroredConfig([string]$Content) {
    # Change only networkingMode inside [wsl2], preserving other sections/comments.
    $newline = if ($Content.Contains("`r`n")) { "`r`n" } else { "`n" }
    $sections = [regex]::Matches($Content, '(?m)^\s*\[wsl2\][ \t]*\r?$')
    if ($sections.Count -gt 1) { throw 'Duplicate [wsl2] sections: repair .wslconfig first.' }
    if ($sections.Count -eq 0) {
        return $Content.TrimEnd("`r", "`n") + $newline + '[wsl2]' + $newline + 'networkingMode=mirrored' + $newline
    }
    $section = [regex]::Match($Content, '(?ms)^\s*\[wsl2\][ \t]*\r?\n(?:(?!^[ \t]*\[).)*')
    if (-not $section.Success) {
        return $Content.TrimEnd("`r", "`n") + $newline + 'networkingMode=mirrored' + $newline
    }
    $block = $section.Value
    $settings = [regex]::Matches($block, '(?mi)^[ \t]*networkingMode[ \t]*=.*$')
    if ($settings.Count -gt 1) { throw 'Duplicate networkingMode settings: repair .wslconfig first.' }
    if ($settings.Count) {
        $block = [regex]::Replace($block, '(?mi)^[ \t]*networkingMode[ \t]*=[^\r\n]*', 'networkingMode=mirrored')
    } else {
        $block = $block.TrimEnd("`r", "`n") + $newline + 'networkingMode=mirrored' + $newline
    }
    return $Content.Substring(0, $section.Index) + $block + $Content.Substring($section.Index + $section.Length)
}

# Dot-sourcing exposes the pure config editor for tests without inspecting/changing Windows.
if ($MyInvocation.InvocationName -eq '.') { return }

Write-Output 'Inspecting Windows/WSL. No changes occur without an explicit setup switch.'
Write-Output ('Windows build: ' + [Environment]::OSVersion.Version.Build)
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$modeOutput = & wsl.exe --exec wslinfo --networking-mode 2>&1
$ErrorActionPreference = $previousPreference
if ($LASTEXITCODE -eq 0) { Write-Output "WSL mode: $modeOutput" }
else { Write-Warning 'Cannot query WSL mode. Start/update WSL and run wslinfo --networking-mode in Ubuntu.' }
Get-NetConnectionProfile | Select-Object InterfaceAlias, NetworkCategory | Format-Table
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.InterfaceAlias -notlike '*Loopback*' } |
    Select-Object InterfaceAlias, IPAddress, PrefixLength | Format-Table
Write-Output 'Existing FUSE firewall rules:'
Get-NetFirewallRule -Name 'FUSE-LAN-UDP' -ErrorAction SilentlyContinue |
    Select-Object Name, Enabled, Profile, Direction, Action | Format-Table
if (Get-Command Get-NetFirewallHyperVRule -ErrorAction SilentlyContinue) {
    Get-NetFirewallHyperVRule -Name 'FUSE-LAN-UDP-WSL' -ErrorAction SilentlyContinue |
        Select-Object Name, Enabled, Profiles, LocalPorts, RemoteAddresses | Format-Table
}

if ($EnableMirrored) {
    if ([Environment]::OSVersion.Version.Build -lt 22621) {
        throw 'Mirrored networking requires Windows 11 22H2 or later. Use native Ubuntu on older PCs.'
    }
    $configPath = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.wslconfig'
    $old = if (Test-Path -LiteralPath $configPath) { [IO.File]::ReadAllText($configPath) } else { '' }
    $updated = Get-MirroredConfig $old
    if ($old -eq $updated) {
        Write-Output 'Mirrored networking is already configured.'
        if (($modeOutput | Out-String).Trim() -ne 'mirrored') {
            Write-Output 'It is not active yet: save WSL work, run wsl --shutdown, reopen Ubuntu and recheck the mode.'
        }
    } elseif ($PSCmdlet.ShouldProcess($configPath, 'Back up existing config and enable mirrored WSL networking')) {
        if (Test-Path -LiteralPath $configPath) {
            $backupPath = $configPath + '.fuse-lan-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
            Copy-Item -LiteralPath $configPath -Destination $backupPath
            Write-Output "Backup: $backupPath"
        }
        [IO.File]::WriteAllText($configPath, $updated, [Text.UTF8Encoding]::new($false))
        Write-Output 'Restart required: save work in all WSL terminals, then run wsl --shutdown and reopen Ubuntu.'
    }
}

if ($ConfigureFirewall) {
    $parts = $LanSubnet -split '/'
    $address = $null
    if ($parts.Count -ne 2 -or -not [Net.IPAddress]::TryParse($parts[0], [ref]$address) -or
        $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
        $parts[1] -notmatch '^([1-9]|[12][0-9]|3[0-2])$') {
        throw 'Specify the actual LAN IPv4 subnet, for example -LanSubnet 192.168.1.0/24 (a /0 rule is not allowed).'
    }
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $WhatIfPreference -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Firewall changes require an Administrator PowerShell window. Inspection and -EnableMirrored do not.'
    }
    if (-not (Get-Command New-NetFirewallHyperVRule -ErrorAction SilentlyContinue)) {
        throw 'Hyper-V firewall cmdlets are unavailable. Update Windows/WSL before configuring LAN rules.'
    }
    if ($PSCmdlet.ShouldProcess("Private LAN $LanSubnet", 'Allow FUSE UDP 7400-7401 and 7410-7429 in Windows and WSL')) {
        $hostRule = @{ Name='FUSE-LAN-UDP'; Direction='Inbound'; Action='Allow'; Enabled='True';
                       Profile='Private'; Protocol='UDP'; LocalPort=@('7400-7401','7410-7429'); RemoteAddress=$LanSubnet }
        if (Get-NetFirewallRule -Name $hostRule.Name -ErrorAction SilentlyContinue) {
            Set-NetFirewallRule @hostRule | Out-Null
        } else {
            New-NetFirewallRule @hostRule -DisplayName 'FUSE direct LAN multiplayer' | Out-Null
        }
        $wslRule = @{ Name='FUSE-LAN-UDP-WSL'; VMCreatorId='{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}';
                      Direction='Inbound'; Action='Allow'; Enabled='True'; Profiles='Private'; Protocol='UDP';
                      LocalPorts=@('7400-7401','7410-7429'); RemoteAddresses=$LanSubnet }
        if (Get-NetFirewallHyperVRule -Name $wslRule.Name -ErrorAction SilentlyContinue) {
            Set-NetFirewallHyperVRule @wslRule | Out-Null
        } else {
            New-NetFirewallHyperVRule @wslRule -DisplayName 'FUSE direct LAN multiplayer (WSL)' | Out-Null
        }
        Write-Output 'FUSE rules configured for this subnet on Private networks. Firewall defaults remain unchanged.'
    }
}
Write-Output 'Run the secure LAN check against the host before considering further changes.'
