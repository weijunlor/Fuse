# Isolated full-setup orchestration. No real UAC, WSL, network or installation.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\run-setup.ps1')
function Assert-True($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Assert-Throws([scriptblock]$Action, [string]$Pattern) {
    try { & $Action } catch { if ($_.Exception.Message -like $Pattern) { return }; throw }
    throw "Expected error: $Pattern"
}
$script:events = [Collections.Generic.List[string]]::new()
$script:admin = $true
$script:failure = ''
$script:uid = '1000'
$script:lastLinuxArguments = @()
$script:config = Join-Path $PSScriptRoot 'test config with spaces'
function Test-FuseAdministrator { return $script:admin }
function Invoke-FuseNetworkSetup {
    param($Subnet, $ConfigPath, [switch]$SkipRestart)
    Assert-True ($Subnet -eq '10.243.66.0/24' -and $ConfigPath -eq $script:config) 'Network parameters changed.'
    Assert-True $SkipRestart 'Only the original user may restart WSL.'
    $script:events.Add('network')
    if ($script:failure -eq 'network') { throw 'network failed' }
}
function Start-Process {
    param($FilePath, $Verb, $ArgumentList, [switch]$Wait, [switch]$PassThru, $WindowStyle)
    Assert-True ($FilePath -eq 'powershell.exe' -and $Verb -eq 'RunAs' -and $WindowStyle -eq 'Hidden') 'Unexpected elevation.'
    Assert-True ($Wait -and $PassThru) 'Elevation must finish before Linux setup.'
    Assert-True ($ArgumentList.Contains('-NetworkOnly -NoRestart')) 'Elevated child could enter Linux setup.'
    Assert-True ($ArgumentList.Contains('"' + $script:config + '"')) 'Original user config path was not preserved.'
    $script:events.Add('elevate')
    return @{ExitCode=$(if ($script:failure -eq 'elevate') {1} else {0})}
}
function wsl.exe {
    param([Parameter(ValueFromRemainingArguments)] [string[]]$WslArguments)
    $global:LASTEXITCODE = 0
    switch ($WslArguments -join ' ') {
        '--exec id -u' {
            $script:events.Add('preflight')
            if ($script:failure -eq 'preflight') { $global:LASTEXITCODE = 1 } else { $script:uid }
        }
        '--shutdown' {
            $script:events.Add('restart')
            if ($script:failure -eq 'restart') { $global:LASTEXITCODE = 2 }
        }
        default {
            Assert-True ($WslArguments[0] -eq '--cd' -and $WslArguments[2] -eq '--exec' -and
                $WslArguments[3] -eq 'bash' -and $WslArguments[4] -eq './run-setup.sh') 'Unexpected Linux setup invocation.'
            $script:lastLinuxArguments = $WslArguments
            $script:events.Add('linux')
            if ($script:failure -eq 'linux') { $global:LASTEXITCODE = 3 }
        }
    }
}
$options = @{SetupRole='Host';Subnet='10.243.66.0/24';ConfigPath=$script:config}
Invoke-FuseSetup @options -WhatIf
Assert-True ($script:events.Count -eq 0) 'Preview started WSL or changed the machine.'
Invoke-FuseSetup @options
Assert-True (($script:events -join ',') -eq 'preflight,network,restart,linux') 'Wrong setup order.'
Assert-True (($script:lastLinuxArguments[-2..-1] -join ' ') -eq '--role host') 'Host role not forwarded.'
$script:events.Clear()
$options.SetupRole='Player'
Invoke-FuseSetup @options -SkipRestart -SkipDependencyInstall
Assert-True (($script:events -join ',') -eq 'preflight,network,linux') 'Deferred restart did not work.'
Assert-True (($script:lastLinuxArguments[-3..-1] -join ' ') -eq '--role player --skip-dependencies') 'Player/options not forwarded.'
$script:events.Clear()
$script:admin = $false
Invoke-FuseSetup @options
Assert-True (($script:events -join ',') -eq 'preflight,elevate,restart,linux') 'UAC must leave WSL operations in the original process.'
foreach ($stage in @('preflight','elevate','restart','linux')) {
    $script:events.Clear()
    $script:failure=$stage
    $messages=[Collections.Generic.List[object]]::new()
    Assert-Throws { Invoke-FuseSetup @options 6>&1 | ForEach-Object {$messages.Add($_)} } '*'
    Assert-True (-not (($messages | Out-String).Contains('setup complete'))) 'Failure reported success.'
    if ($stage -in @('preflight','elevate','restart')) { Assert-True (-not $script:events.Contains('linux')) 'Setup continued after failure.' }
}
$script:admin=$true
$script:failure='network'
$script:events.Clear()
Assert-Throws { Invoke-FuseSetup @options } '*network failed*'
Assert-True (($script:events -join ',') -eq 'preflight,network') 'Network failure restarted or installed.'
$script:failure=''
$script:uid='0'
$script:events.Clear()
Assert-Throws { Invoke-FuseSetup @options } '*regular Linux user*'
Assert-True (($script:events -join ',') -eq 'preflight') 'Root-user preflight changed the machine.'
Write-Host 'PASS: full setup roles, preview, original-user WSL, elevation, ordering, deferred restart and failure handling.'
