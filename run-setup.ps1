<#
.SYNOPSIS
Configure Windows/WSL networking for both FUSE launchers on this PC.
.EXAMPLE
.\run-setup.bat
.EXAMPLE
.\run-setup.ps1 -LanSubnet 192.168.1.0/24 -NoRestart
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$LanSubnet = '10.243.66.0/24',
    [switch]$NoRestart,
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
    Write-Host 'After the Build prerequisites in README.md, use .\run-server.bat or .\run-client.bat.'
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
    if (-not $NoRestart -and -not $WhatIfPreference) {
        Write-Warning 'Setup shuts down ALL WSL distributions. Save WSL work before running it; use -NoRestart to defer shutdown.'
    }
    if (-not $WhatIfPreference -and -not (Test-FuseAdministrator)) {
        # UAC may use another admin account. Keep the original config path and
        # shut down WSL in this user's session after the elevated changes finish.
        $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -LanSubnet `"$LanSubnet`" -WslConfigPath `"$WslConfigPath`" -NoRestart"
        Write-Host 'Requesting Administrator access to configure the firewall...'
        $process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
        if ($process.ExitCode -ne 0) {
            throw 'Administrator setup failed. Run .\run-setup.bat in an Administrator PowerShell window to see the error.'
        }
        if (-not $NoRestart) {
            Write-Host 'Stopping all WSL distributions to apply the network settings...'
            & wsl.exe --shutdown
            if ($LASTEXITCODE -ne 0) { throw "wsl --shutdown failed (exit $LASTEXITCODE)." }
        }
        Write-Host 'FUSE networking setup complete. Use .\run-server.bat or .\run-client.bat after completing the Build prerequisites.'
        if ($NoRestart) { Write-Host 'Run wsl --shutdown before starting FUSE to apply the network settings.' }
        exit $process.ExitCode
    }
    Invoke-FuseNetworkSetup -Subnet $LanSubnet -ConfigPath $WslConfigPath -SkipRestart:$NoRestart -WhatIf:$WhatIfPreference
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
