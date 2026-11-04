<#
.SYNOPSIS
Install FUSE dependencies, configure networking, provision the host and build.
.EXAMPLE
.\run-setup.bat
.EXAMPLE
.\run-setup.ps1 -Role Player -LanSubnet 192.168.1.0/24
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidateSet('Host', 'Player')]
    [string]$Role,
    [string]$LanSubnet = '10.243.66.0/24',
    [switch]$NoRestart,
    [switch]$SkipDependencies,
    [Parameter(DontShow=$true)]
    [switch]$NetworkOnly,
    [string]$WslConfigPath = (Join-Path $env:USERPROFILE '.wslconfig')
)

$ErrorActionPreference = 'Stop'

function Get-FuseWslConfig([string]$Content) {
    $newline = if (-not $Content -or $Content.Contains("`r`n")) { "`r`n" } else { "`n" }
    $headers = [regex]::Matches($Content, '(?mi)^[ \t]*\[wsl2\][ \t]*(?:[;#][^\r\n]*)?\r?$')
    if ($headers.Count -gt 1) { throw 'Duplicate [wsl2] sections: repair .wslconfig first.' }
    if ($headers.Count -eq 0) {
        $prefix = if ($Content) { $Content.TrimEnd("`r", "`n") + $newline + $newline } else { '' }
        return $prefix + '[wsl2]' + $newline + 'networkingMode=mirrored' + $newline +
            'localhostForwarding=true' + $newline + 'firewall=false' + $newline
    }

    $start = $headers[0].Index
    $afterHeader = $start + $headers[0].Length
    $nextHeader = [regex]::Match($Content.Substring($afterHeader), '(?m)^[ \t]*\[')
    $end = if ($nextHeader.Success) { $afterHeader + $nextHeader.Index } else { $Content.Length }
    $block = $Content.Substring($start, $end - $start)
    foreach ($setting in @('networkingMode=mirrored', 'localhostForwarding=true', 'firewall=false')) {
        $key = $setting.Split('=')[0]
        $pattern = '(?mi)^[ \t]*' + $key + '[ \t]*=[^\r\n]*'
        $matches = [regex]::Matches($block, $pattern)
        if ($matches.Count -gt 1) { throw "Duplicate $key settings: repair .wslconfig first." }
        if ($matches.Count) {
            $block = [regex]::Replace($block, $pattern, $setting)
        } else {
            $block = $block.TrimEnd("`r", "`n") + $newline + $setting + $newline
        }
    }
    # A section originally ending at EOF may not have had a trailing newline.
    if ($end -lt $Content.Length -and -not $block.EndsWith("`n")) { $block += $newline }
    return $Content.Substring(0, $start) + $block + $Content.Substring($end)
}

function Test-FuseAdministrator {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-FuseNetworkSetup {
    [CmdletBinding(SupportsShouldProcess)]
    param([string]$Subnet, [string]$ConfigPath, [switch]$SkipRestart)

    # Preflight before changing either the firewall or the user's config.
    foreach ($command in @('wsl.exe', 'Get-NetFirewallRule', 'New-NetFirewallRule', 'Set-NetFirewallRule',
            'Get-NetFirewallHyperVRule', 'New-NetFirewallHyperVRule', 'Set-NetFirewallHyperVRule')) {
        if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
            throw "Required command '$command' is unavailable. Install/update Windows 11 and WSL first."
        }
    }
    $old = if (Test-Path -LiteralPath $ConfigPath) { [IO.File]::ReadAllText($ConfigPath) } else { '' }
    $updated = Get-FuseWslConfig $old
    # Host firewall rules also appear in this view as HostFirewallLocal entries.
    # They are managed through NetFirewallRule, not NetFirewallHyperVRule.
    $localHypervRules = @(Get-NetFirewallHyperVRule -PolicyStore PersistentStore -PolicyStoreSourceType Local -ErrorAction Stop)

    foreach ($rule in @(
            @{ Name='FUSEPairing'; DisplayName='FUSE Pairing'; Protocol='TCP'; Ports=@('7440') },
            @{ Name='FUSEDDS'; DisplayName='FUSE DDS'; Protocol='UDP'; Ports=@('7400-7401', '7410-7429') })) {
        if ($PSCmdlet.ShouldProcess("$($rule.DisplayName): $Subnet", 'Allow inbound traffic in Windows and WSL')) {
            $windows = @{ Name=$rule.Name; Direction='Inbound';
                          Action='Allow'; Enabled='True'; Profile='Any'; Protocol=$rule.Protocol;
                          LocalPort=$rule.Ports; RemoteAddress=$Subnet }
            if (Get-NetFirewallRule -Name $rule.Name -ErrorAction SilentlyContinue) {
                Set-NetFirewallRule @windows -NewDisplayName $rule.DisplayName -ErrorAction Stop | Out-Null
            } else {
                New-NetFirewallRule @windows -DisplayName $rule.DisplayName -ErrorAction Stop | Out-Null
            }
            # Separate names avoid colliding with the host rules mirrored above.
            $hyperv = @{ Name=($rule.Name + '-WSL'); PolicyStore='PersistentStore'; Direction='Inbound';
                         Action='Allow'; Enabled='True'; Profiles='Any'; Protocol=$rule.Protocol;
                         VMCreatorId='{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}';
                         LocalPorts=$rule.Ports; RemoteAddresses=$Subnet }
            if ($localHypervRules | Where-Object { $_.Name -eq $hyperv.Name }) {
                Set-NetFirewallHyperVRule @hyperv -NewDisplayName $rule.DisplayName -ErrorAction Stop | Out-Null
            } else {
                New-NetFirewallHyperVRule @hyperv -DisplayName $rule.DisplayName -ErrorAction Stop | Out-Null
            }
        }
    }

    if ($old -ne $updated -and $PSCmdlet.ShouldProcess($ConfigPath, 'Back up config and set mirrored networking, localhost forwarding and firewall=false')) {
        if (Test-Path -LiteralPath $ConfigPath) {
            $backup = $ConfigPath + '.fuse-setup-backup-' + [guid]::NewGuid().ToString('N')
            Copy-Item -LiteralPath $ConfigPath -Destination $backup
            Write-Host "Backup: $backup"
        }
        [IO.File]::WriteAllText($ConfigPath, $updated, [Text.UTF8Encoding]::new($false))
    }

    if (-not $SkipRestart -and $PSCmdlet.ShouldProcess('All WSL distributions', 'Shut down WSL to apply network settings')) {
        Write-Host 'Stopping all WSL distributions to apply the network settings...'
        & wsl.exe --shutdown
        if ($LASTEXITCODE -ne 0) { throw "wsl --shutdown failed (exit $LASTEXITCODE)." }
    }
    if ($WhatIfPreference) { return }
    if ($SkipRestart) { Write-Host 'Save WSL work, then run wsl --shutdown before starting FUSE.' }
    Write-Host 'Network setup complete for server and client on this PC.'
}

function Invoke-FuseElevatedNetworkSetup {
    param([string]$Subnet, [string]$ConfigPath)
    # Elevate only Windows configuration. Linux installation must run in the
    # original user's WSL distribution, even if UAC uses another admin account.
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -NetworkOnly -NoRestart -LanSubnet `"$Subnet`" -WslConfigPath `"$ConfigPath`""
    Write-Host 'Requesting Administrator access to configure the firewall...'
    $process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) { throw 'Administrator network setup failed; full setup stopped.' }
}

function Invoke-FuseSetup {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [Parameter(Mandatory=$true)][ValidateSet('Host', 'Player')][string]$SetupRole,
        [string]$Subnet,
        [string]$ConfigPath,
        [string]$Repository = $PSScriptRoot,
        [switch]$SkipRestart,
        [switch]$SkipDependencyInstall
    )

    if (-not (Test-Path -LiteralPath (Join-Path $Repository 'run-setup.sh') -PathType Leaf)) {
        throw 'run-setup.sh is missing. Keep the complete FUSE project together.'
    }
    # Do not start WSL at all during a preview. Check the Linux user before any
    # Windows changes so credentials cannot accidentally be installed as root.
    if (-not $WhatIfPreference) {
        $linuxUid = & wsl.exe --exec id -u
        if ($LASTEXITCODE -ne 0) { throw 'Open your Ubuntu WSL distribution and finish its first-time Linux user setup, then rerun run-setup.' }
        if (($linuxUid | Out-String).Trim() -notmatch '^[1-9][0-9]*$') {
            throw 'Use a regular Linux user as the default WSL user, then rerun run-setup.'
        }
    }

    if ($PSCmdlet.ShouldProcess('Windows/WSL networking', 'Configure firewall and mirrored networking')) {
        if (Test-FuseAdministrator) {
            Invoke-FuseNetworkSetup -Subnet $Subnet -ConfigPath $ConfigPath -SkipRestart
        } else {
            Invoke-FuseElevatedNetworkSetup -Subnet $Subnet -ConfigPath $ConfigPath
        }
    }
    if (-not $SkipRestart -and $PSCmdlet.ShouldProcess('All WSL distributions', 'Restart WSL before Linux installation')) {
        & wsl.exe --shutdown
        if ($LASTEXITCODE -ne 0) { throw "wsl --shutdown failed (exit $LASTEXITCODE)." }
    }
    if ($PSCmdlet.ShouldProcess("FUSE $SetupRole in the original user's WSL", 'Install dependencies and DDS, provision host credentials if needed, and build')) {
        $linuxArguments = @('--cd', $Repository, '--exec', 'bash', './run-setup.sh', '--role', $SetupRole.ToLowerInvariant())
        if ($SkipDependencyInstall) { $linuxArguments += '--skip-dependencies' }
        & wsl.exe @linuxArguments
        if ($LASTEXITCODE -ne 0) { throw "Linux setup failed (exit $LASTEXITCODE). Fix the error above and rerun run-setup; existing credentials are preserved." }
        if ($SkipRestart) { Write-Host 'Run wsl --shutdown before playing to apply the network settings.' }
        Write-Host "FUSE $SetupRole setup complete. Use .\run-server.bat on the host or .\run-client.bat on a player PC."
    }
}

# Expose functions for isolated tests without inspecting/changing this computer.
if ($MyInvocation.InvocationName -eq '.') { return }

try {
    $parts = $LanSubnet -split '/'
    $address = $null
    if ($parts.Count -ne 2 -or -not [Net.IPAddress]::TryParse($parts[0], [ref]$address) -or
            $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
            $parts[1] -notmatch '^([1-9]|[12][0-9]|3[0-2])$') {
        throw 'LanSubnet must be an IPv4 CIDR subnet, for example 10.243.66.0/24.'
    }
    if ([Environment]::OSVersion.Version.Build -lt 22621) {
        throw 'Mirrored networking requires Windows 11 22H2 or later and a current WSL installation.'
    }
    $WslConfigPath = [IO.Path]::GetFullPath($WslConfigPath)
    if ($NetworkOnly) {
        if (-not $WhatIfPreference -and -not (Test-FuseAdministrator)) { throw 'Internal network setup requires Administrator access.' }
        Invoke-FuseNetworkSetup -Subnet $LanSubnet -ConfigPath $WslConfigPath -SkipRestart:$NoRestart -WhatIf:$WhatIfPreference
        exit 0
    }
    if (-not $Role) {
        if ($WhatIfPreference) {
            $Role = 'Host'
            Write-Host 'Previewing Host setup. Use -Role Player to preview a player PC.'
        } else {
            do {
                $choice = Read-Host 'Set up this PC as [H]ost server or [P]layer'
                if ([string]::IsNullOrWhiteSpace($choice)) { throw 'Choose a role, or rerun with -Role Host or -Role Player.' }
            } until ($choice -match '^(?i:h|host|p|player)$')
            $Role = if ($choice -match '^(?i:h|host)$') { 'Host' } else { 'Player' }
        }
    }
    if (-not $NoRestart -and -not $WhatIfPreference) {
        Write-Warning 'Setup shuts down ALL WSL distributions. Save WSL work before running it; use -NoRestart to defer shutdown.'
    }
    Invoke-FuseSetup -SetupRole $Role -Subnet $LanSubnet -ConfigPath $WslConfigPath -SkipRestart:$NoRestart -SkipDependencyInstall:$SkipDependencies -WhatIf:$WhatIfPreference
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
