# Isolated regression checks: no real firewall commands or WSL shutdowns.
# Run: powershell.exe -NoProfile -File .\tests\test_run_setup.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\run-setup.ps1')

function Assert-True($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Assert-Throws([scriptblock]$Action, [string]$Pattern) {
    try { & $Action } catch {
        if ($_.Exception.Message -like $Pattern) { return }
        throw
    }
    throw "Expected an error matching $Pattern"
}

foreach ($inputConfig in @('', '[wsl2]', "[wsl2]`nnetworkingMode=nat", "[experimental]`nautoMemoryReclaim=gradual`n",
        "# keep`r`n[WSL2] # settings`r`nmemory=8GB`r`nfirewall=true`r`n[experimental]`r`nnetworkingMode=nat`r`n")) {
    $result = Get-FuseWslConfig $inputConfig
    Assert-True ($result -eq (Get-FuseWslConfig $result)) 'Config editing must be idempotent.'
    foreach ($setting in @('networkingMode=mirrored', 'localhostForwarding=true', 'firewall=false')) {
        Assert-True ($result.Contains($setting)) "Missing $setting"
    }
    if ($inputConfig.Contains('memory=8GB')) {
        Assert-True ($result.Contains("# keep`r`n")) 'Top-level comment changed.'
        Assert-True ($result.Contains("memory=8GB`r`n")) 'Unrelated WSL setting changed.'
        Assert-True ($result.Contains("[experimental]`r`nnetworkingMode=nat`r`n")) 'Other section changed.'
    }
}
Assert-Throws { Get-FuseWslConfig "[wsl2]`n[wsl2]" } '*Duplicate * sections*'
foreach ($key in @('networkingMode', 'localhostForwarding', 'firewall')) {
    Assert-Throws { Get-FuseWslConfig "[wsl2]`n$key=a`n$key=b" } "*Duplicate $key settings*"
}

# Mock the complete mutation boundary and retain parameters for assertions.
$script:windowsRules = @{}
$script:hypervRules = @{}
$script:creates = 0
$script:updates = 0
$script:shutdowns = 0
$script:shutdownExit = 0
function Get-NetFirewallRule {
    [CmdletBinding()] param($Name)
    if ($script:windowsRules.ContainsKey($Name)) { return $script:windowsRules[$Name] }
}
function Get-NetFirewallHyperVRule {
    [CmdletBinding()] param($Name)
    if ($script:hypervRules.ContainsKey($Name)) { return $script:hypervRules[$Name] }
}
function New-NetFirewallRule {
    [CmdletBinding()] param($Name, $DisplayName, $Direction, $Action, $Enabled, $Profile, $Protocol, $LocalPort, $RemoteAddress)
    Assert-True (-not $script:windowsRules.ContainsKey($Name)) 'Duplicate Windows rule.'
    $script:windowsRules[$Name] = @{} + $PSBoundParameters
    $script:creates++
}
function Set-NetFirewallRule {
    [CmdletBinding()] param($Name, $NewDisplayName, $Direction, $Action, $Enabled, $Profile, $Protocol, $LocalPort, $RemoteAddress)
    Assert-True ($script:windowsRules.ContainsKey($Name)) 'Updating nonexistent Windows rule.'
    $script:windowsRules[$Name] = @{} + $PSBoundParameters
    $script:updates++
}
function New-NetFirewallHyperVRule {
    [CmdletBinding()] param($Name, $DisplayName, $Direction, $Action, $Enabled, $Profiles, $Protocol, $LocalPorts, $RemoteAddresses, $VMCreatorId)
    Assert-True (-not $script:hypervRules.ContainsKey($Name)) 'Duplicate Hyper-V rule.'
    $script:hypervRules[$Name] = @{} + $PSBoundParameters
    $script:creates++
}
function Set-NetFirewallHyperVRule {
    [CmdletBinding()] param($Name, $NewDisplayName, $Direction, $Action, $Enabled, $Profiles, $Protocol, $LocalPorts, $RemoteAddresses, $VMCreatorId)
    Assert-True ($script:hypervRules.ContainsKey($Name)) 'Updating nonexistent Hyper-V rule.'
    $script:hypervRules[$Name] = @{} + $PSBoundParameters
    $script:updates++
}
function wsl.exe {
    param([Parameter(ValueFromRemainingArguments)] [string[]]$WslArguments)
    Assert-True (($WslArguments -join ' ') -eq '--shutdown') 'Unexpected WSL operation.'
    $script:shutdowns++
    $global:LASTEXITCODE = $script:shutdownExit
}

$directory = Join-Path $PSScriptRoot ('setup-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $directory | Out-Null
$configPath = Join-Path $directory '.wslconfig'
$original = "[wsl2]`nmemory=8GB`nnetworkingMode=nat`n"
[IO.File]::WriteAllText($configPath, $original)
try {
    Invoke-FuseNetworkSetup -Subnet '10.243.66.0/24' -ConfigPath $configPath -WhatIf
    Assert-True ($script:creates -eq 0 -and $script:shutdowns -eq 0) 'Preview performed a mutation.'
    Assert-True ([IO.File]::ReadAllText($configPath) -eq $original) 'Preview changed the config.'
    Assert-True (@(Get-ChildItem -LiteralPath $directory -Force).Count -eq 1) 'Preview created a backup.'

    Invoke-FuseNetworkSetup -Subnet '10.243.66.0/24' -ConfigPath $configPath
    Assert-True ($script:creates -eq 4 -and $script:shutdowns -eq 1) 'First setup did not create four rules and restart WSL.'
    $backups = @(Get-ChildItem -LiteralPath $directory -Force -Filter '*.fuse-setup-backup-*')
    Assert-True ($backups.Count -eq 1) 'Expected one config backup.'
    Assert-True ([IO.File]::ReadAllText($backups[0].FullName) -eq $original) 'Backup does not match original.'
    foreach ($name in @('FUSEPairing', 'FUSEDDS')) {
        $windows = $script:windowsRules[$name]
        $hyperv = $script:hypervRules[$name]
        Assert-True ($windows.RemoteAddress -eq '10.243.66.0/24' -and $hyperv.RemoteAddresses -eq '10.243.66.0/24') 'Incorrect subnet scope.'
        Assert-True ($windows.Profile -eq 'Any' -and $hyperv.Profiles -eq 'Any') 'Incorrect profile scope.'
        Assert-True ($windows.Direction -eq 'Inbound' -and $hyperv.Direction -eq 'Inbound') 'Incorrect direction.'
        Assert-True ($windows.Action -eq 'Allow' -and $hyperv.Action -eq 'Allow') 'Incorrect action.'
        Assert-True ($hyperv.VMCreatorId -eq '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}') 'Incorrect WSL VM creator.'
        $protocol = if ($name -eq 'FUSEPairing') { 'TCP' } else { 'UDP' }
        $ports = if ($name -eq 'FUSEPairing') { '7440' } else { '7400-7401,7410-7429' }
        Assert-True ($windows.Protocol -eq $protocol -and $hyperv.Protocol -eq $protocol) 'Incorrect protocol.'
        Assert-True (($windows.LocalPort -join ',') -eq $ports -and ($hyperv.LocalPorts -join ',') -eq $ports) 'Incorrect ports.'
    }

    Invoke-FuseNetworkSetup -Subnet '192.168.1.0/24' -ConfigPath $configPath -SkipRestart
    Assert-True ($script:creates -eq 4 -and $script:updates -eq 4) 'Rerun did not update existing rules.'
    Assert-True ($script:windowsRules.FUSEDDS.RemoteAddress -eq '192.168.1.0/24') 'Rerun did not update subnet.'
    Assert-True ($script:shutdowns -eq 1) 'SkipRestart shut down WSL.'
    Assert-True (@(Get-ChildItem -LiteralPath $directory -Force -Filter '*.fuse-setup-backup-*').Count -eq 1) 'Unchanged config was backed up again.'

    [IO.File]::WriteAllText($configPath, "[wsl2]`nfirewall=true`nfirewall=false")
    Assert-Throws { Invoke-FuseNetworkSetup -Subnet '10.243.66.0/24' -ConfigPath $configPath } '*Duplicate firewall settings*'
    Assert-True ($script:updates -eq 4) 'Invalid config changed firewall rules before failing.'
    [IO.File]::WriteAllText($configPath, (Get-FuseWslConfig ''))
    $script:shutdownExit = 5
    Assert-Throws { Invoke-FuseNetworkSetup -Subnet '10.243.66.0/24' -ConfigPath $configPath } '*shutdown failed*'
} finally {
    # Only flat files in this explicitly created test directory are removed.
    Get-ChildItem -LiteralPath $directory -File -Force | Remove-Item -Force
    Remove-Item -LiteralPath $directory
}
Write-Host 'PASS: config preservation, backups, firewall creation/update, preview, deferred restart and shutdown errors.'
