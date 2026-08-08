[CmdletBinding()]
param(
    [ValidateSet('Package', 'ProvisionRemote', 'Preflight', 'Deploy', 'Smoke', 'Collect', 'All')]
    [string]$Action = 'All',
    [string]$LocalGameRoot = 'E:\HTA_EFA',
    [string]$RemoteSsh = 'etozh@192.168.2.80',
    [string]$RemoteGameRoot = 'E:\HTA_EFA',
    [string]$EfaRoot = 'E:\code\Escape-from-Apocalypse',
    [string]$KrakenBuildRoot = 'E:\KrakenWorkspace\Kraken\build-ninja',
    [int]$SmokePort = 27830
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Root = Split-Path -Parent $PSScriptRoot
$script:ArtifactRoot = Join-Path $script:Root 'artifacts\efa-multiplayer-harness'
$script:RunId = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:RunRoot = Join-Path $script:ArtifactRoot $script:RunId
$script:OverlayRoot = Join-Path $script:RunRoot 'overlay'
$script:RemoteStage = "C:\Users\etozh\AppData\Local\Temp\efa-mp-$script:RunId.zip"
$script:RemoteScpStage = "C:/Users/etozh/AppData/Local/Temp/efa-mp-$script:RunId.zip"
$script:RemoteScriptStage = "C:\Users\etozh\AppData\Local\Temp\efa-mp-$script:RunId.ps1"
$script:RemoteScpScriptStage = "C:/Users/etozh/AppData/Local/Temp/efa-mp-$script:RunId.ps1"
$script:OverlayFiles = @(
    [PSCustomObject]@{ Source = 'kraken.dll'; Target = 'kraken.dll' },
    [PSCustomObject]@{ Source = 'kraken_net_peer_test.exe'; Target = 'kraken_net_peer_test.exe' },
    [PSCustomObject]@{ Source = 'data\scripts\efa_multiplayer.lua'; Target = 'data\scripts\efa_multiplayer.lua' }
)

function Assert-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
}

function ConvertTo-EncodedPowerShell([string]$ScriptText) {
    return [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($ScriptText))
}

function Invoke-RemotePowerShell([string]$ScriptText) {
    $encoded = ConvertTo-EncodedPowerShell $ScriptText
    $remoteExitCode = 0
    if ($encoded.Length -le 7000) {
        $result = & ssh -o BatchMode=yes -o ConnectTimeout=10 $RemoteSsh "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded"
        $remoteExitCode = $LASTEXITCODE
    }
    else {
        $localScript = Join-Path $env:TEMP "efa-mp-$script:RunId.ps1"
        [IO.File]::WriteAllText($localScript, $ScriptText, [Text.Encoding]::Unicode)
        try {
            Copy-ToRemote $localScript $script:RemoteScpScriptStage
            $result = & ssh -o BatchMode=yes -o ConnectTimeout=10 $RemoteSsh "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$script:RemoteScriptStage`""
            $remoteExitCode = $LASTEXITCODE
        }
        finally {
            Remove-Item -LiteralPath $localScript -Force -ErrorAction SilentlyContinue
            & ssh -o BatchMode=yes -o ConnectTimeout=10 $RemoteSsh "del /q `"$script:RemoteScriptStage`"" 2>$null
        }
    }
    if ($remoteExitCode -ne 0) {
        throw "Remote command failed ($remoteExitCode): $result"
    }
    return $result
}

function Copy-ToRemote([string]$Source, [string]$Destination) {
    & scp -q $Source "$RemoteSsh`:$Destination"
    if ($LASTEXITCODE -ne 0) {
        throw "SCP upload failed: $Source -> $RemoteSsh`:$Destination"
    }
}

function Copy-RelativeFile([string]$SourceRoot, [string]$DestinationRoot,
                           [string]$RelativePath) {
    $source = Join-Path $SourceRoot $RelativePath
    Assert-File $source
    $destination = Join-Path $DestinationRoot $RelativePath
    $destinationDirectory = Split-Path -Parent $destination
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

function New-Overlay {
    Remove-Item -LiteralPath $script:OverlayRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $script:OverlayRoot -Force | Out-Null
    foreach ($file in $script:OverlayFiles) {
        $sourceRoot = if ($file.Source -like 'data\*') { $EfaRoot } else { $KrakenBuildRoot }
        $source = Join-Path $sourceRoot $file.Source
        Assert-File $source
        $path = Join-Path $script:OverlayRoot $file.Target
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $path -Force
    }
    $manifest = foreach ($file in $script:OverlayFiles) {
        $path = Join-Path $script:OverlayRoot $file.Target
        [PSCustomObject]@{
            path = $file.Target.Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            bytes = (Get-Item -LiteralPath $path).Length
        }
    }
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $script:OverlayRoot 'overlay-manifest.json') -Encoding UTF8
    Compress-Archive -Path (Join-Path $script:OverlayRoot '*') -DestinationPath (Join-Path $script:RunRoot 'overlay.zip') -Force
}

function New-ComModPackage {
    New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
    $archive = Join-Path $script:RunRoot 'escape-from-apocalypse-multiplayer.zip'
    $entries = Get-ChildItem -LiteralPath $EfaRoot -Force | Where-Object { $_.Name -ne '.git' }
    if (-not $entries) { throw "EFA root is empty: $EfaRoot" }
    Compress-Archive -Path $entries.FullName -DestinationPath $archive -Force
    Write-Host "ComMod package created: $archive"
}

function New-RemoteRuntimePackage {
    Test-LocalTarget
    $archive = Join-Path $script:RunRoot 'hta-efa-runtime.zip'
    # Compress-Archive in Windows PowerShell 5 may fail while reading a hidden
    # Explorer metadata file; it is not part of the game runtime.
    $excludedNames = @('backups', 'exceptions', '.git', 'desktop.ini', 'exmachina.log', 'kraken.log', 'memory.log', 'memlog.txt')
    $entries = Get-ChildItem -LiteralPath $LocalGameRoot -Force | Where-Object {
        $_.Name -notin $excludedNames -and $_.Name -notlike 'screen*.png' -and $_.Name -notlike 'screen*.tga'
    }
    if (-not $entries) { throw "Local EFA target is empty: $LocalGameRoot" }
    Compress-Archive -Path $entries.FullName -DestinationPath $archive -Force
    return $archive
}

function Provision-RemoteTarget {
    $archive = New-RemoteRuntimePackage
    Copy-ToRemote $archive $script:RemoteScpStage
    $scriptText = @"
`$root = '$($RemoteGameRoot.Replace("'", "''"))'
`$archive = '$($script:RemoteStage.Replace("'", "''"))'
`$ProgressPreference = 'SilentlyContinue'
if (Test-Path -LiteralPath `$root) {
    Write-Error "Refusing to overwrite existing remote target: `$root"
    exit 4
}
`$parent = Split-Path -Parent `$root
if (-not (Test-Path -LiteralPath `$parent)) { throw "Remote parent does not exist: `$parent" }
New-Item -ItemType Directory -Path `$root -Force | Out-Null
try {
    Expand-Archive -LiteralPath `$archive -DestinationPath `$root -Force
    `$required = @('hta.exe','data\\scripts\\efa.lua','data\\maps\\r0m0\\cinematriggers.xml')
    `$missing = `$required | Where-Object { -not (Test-Path -LiteralPath (Join-Path `$root `$_)) }
    `$raidTriggerCandidates = @('data\\maps\\r1m1\\triggers.xml','data\\maps\\r1m1\\winter\\spring\\summer\\autumn\\main\\triggers.xml')
    if (-not (`$raidTriggerCandidates | Where-Object { Test-Path -LiteralPath (Join-Path `$root `$_) })) { `$missing += 'r1m1 triggers.xml (flat or Community Remaster layout)' }
    if (`$missing) { throw ('Provisioned target is incomplete. Missing: ' + (`$missing -join ', ')) }
    Write-Output ('provisioned_target=' + `$root)
}
catch {
    Remove-Item -LiteralPath `$root -Recurse -Force -ErrorAction SilentlyContinue
    throw
}
finally {
    Remove-Item -LiteralPath `$archive -Force -ErrorAction SilentlyContinue
}
"@
    Invoke-RemotePowerShell $scriptText | Write-Host
}

function Test-LocalTarget {
    $required = @(
        'hta.exe',
        'data\scripts\efa.lua',
        'data\maps\r0m0\cinematriggers.xml'
    )
    $missing = $required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $LocalGameRoot $_)) }
    $raidTriggerCandidates = @(
        'data\maps\r1m1\triggers.xml',
        'data\maps\r1m1\winter\spring\summer\autumn\main\triggers.xml'
    )
    if (-not ($raidTriggerCandidates | Where-Object { Test-Path -LiteralPath (Join-Path $LocalGameRoot $_) })) {
        $missing += 'r1m1 triggers.xml (flat or Community Remaster layout)'
    }
    if ($missing) {
        throw "Local target is not a ComMod-installed EFA copy ($LocalGameRoot). Missing: $($missing -join ', ')"
    }
}

function Test-RemoteTarget {
    $scriptText = @"
`$root = '$($RemoteGameRoot.Replace("'", "''"))'
`$required = @('hta.exe','data\\scripts\\efa.lua','data\\maps\\r0m0\\cinematriggers.xml')
`$missing = `$required | Where-Object { -not (Test-Path -LiteralPath (Join-Path `$root `$_)) }
`$raidTriggerCandidates = @('data\\maps\\r1m1\\triggers.xml','data\\maps\\r1m1\\winter\\spring\\summer\\autumn\\main\\triggers.xml')
if (-not (`$raidTriggerCandidates | Where-Object { Test-Path -LiteralPath (Join-Path `$root `$_) })) { `$missing += 'r1m1 triggers.xml (flat or Community Remaster layout)' }
if (`$missing) { Write-Error ('Remote target is not a ComMod-installed EFA copy. Missing: ' + (`$missing -join ', ')); exit 3 }
Write-Output ('remote_target=' + `$root)
"@
    Invoke-RemotePowerShell $scriptText | Write-Host
}

function Stop-LocalGame {
    $processes = @(Get-Process -Name 'hta' -ErrorAction SilentlyContinue)
    if ($processes.Count -eq 0) { return }
    $processes | Stop-Process -Force
    $deadline = (Get-Date).AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 200
        $processes = @(Get-Process -Name 'hta' -ErrorAction SilentlyContinue)
    } while ($processes.Count -ne 0 -and (Get-Date) -lt $deadline)
    if ($processes.Count -ne 0) { throw 'Local hta.exe did not exit before deployment' }
    Write-Host 'Stopped local hta.exe before deployment'
}

function Stop-RemoteGame {
    $scriptText = @"
`$processes = @(Get-Process -Name 'hta' -ErrorAction SilentlyContinue)
if (`$processes.Count -ne 0) {
    `$processes | Stop-Process -Force
    `$deadline = (Get-Date).AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 200
        `$processes = @(Get-Process -Name 'hta' -ErrorAction SilentlyContinue)
    } while (`$processes.Count -ne 0 -and (Get-Date) -lt `$deadline)
    if (`$processes.Count -ne 0) { Write-Error 'Remote hta.exe did not exit before deployment'; exit 5 }
    Write-Output 'stopped_remote_hta=1'
}
"@
    Invoke-RemotePowerShell $scriptText | Write-Host
}

function Deploy-LocalOverlay {
    Test-LocalTarget
    $backupRoot = Join-Path $LocalGameRoot "backups\efa-mp-harness\$script:RunId"
    foreach ($file in $script:OverlayFiles) {
        $source = Join-Path $script:OverlayRoot $file.Target
        $target = Join-Path $LocalGameRoot $file.Target
        $backup = Join-Path $backupRoot $file.Target
        if (Test-Path -LiteralPath $target) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $backup) -Force | Out-Null
            Copy-Item -LiteralPath $target -Destination $backup -Force
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
    Install-LocalRaidStartHook $LocalGameRoot $backupRoot
    Install-LocalServerAdapter $LocalGameRoot $backupRoot
}

function Install-LocalRaidStartHook([string]$GameRoot, [string]$BackupRoot) {
    $relative = 'data\maps\r0m0\cinematriggers.xml'
    $target = Join-Path $GameRoot $relative
    Assert-File $target
    $encoding = [Text.Encoding]::GetEncoding(1251)
    $content = [IO.File]::ReadAllText($target, $encoding)
    $marker = '-- Kraken multiplayer raid start'
    $diagnostic = 'LOG("EFA MP: StartMatchmaking trigger entered")'
    if ($content.Contains($marker) -and $content.Contains($diagnostic)) { return }
    $pattern = '(<trigger Name="StartMatchmaking" active="0">\s*<event\s+timeout="0"\s+eventid="GE_TIME_PERIOD"\s*/>\s*<script>)'
    $regex = [regex]::new($pattern)
    if (-not $regex.IsMatch($content)) { throw "Cannot find StartMatchmaking hook in $target" }
    $insertion = "`r`n`t`t`t$marker`r`n`t`t`t$diagnostic`r`n`t`t`tEFA_MP.BeginRaid()"
    $backup = Join-Path $BackupRoot $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $backup) -Force | Out-Null
    Copy-Item -LiteralPath $target -Destination $backup -Force
    if ($content.Contains($marker)) {
        [IO.File]::WriteAllText($target, $content.Replace($marker, "$marker`r`n`t`t`t$diagnostic"), $encoding)
    }
    else {
        [IO.File]::WriteAllText($target, $regex.Replace($content, '$1' + $insertion, 1), $encoding)
    }
}

function Install-LocalServerAdapter([string]$GameRoot, [string]$BackupRoot) {
    $server = Join-Path $GameRoot 'data\scripts\server.lua'
    Assert-File $server
    $efa = Join-Path $GameRoot 'data\scripts\efa.lua'
    $serverBytes = [IO.File]::ReadAllBytes($server)
    $efaBytes = [IO.File]::ReadAllBytes($efa)
    if ([Text.Encoding]::ASCII.GetString($serverBytes).Contains('IfLoadSave()') -and
        -not [Text.Encoding]::ASCII.GetString($efaBytes).Contains('function IfLoadSave')) {
        $knownGood = Get-ChildItem -LiteralPath (Join-Path $GameRoot 'backups\efa-mp-harness') -Recurse -Filter server.lua -ErrorAction SilentlyContinue |
            Where-Object { -not [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($_.FullName)).Contains('IfLoadSave()') } |
            Sort-Object LastWriteTime | Select-Object -First 1
        if ($null -eq $knownGood) { throw "Cannot restore compatible server.lua for $GameRoot" }
        Copy-Item -LiteralPath $knownGood.FullName -Destination $server -Force
        $serverBytes = [IO.File]::ReadAllBytes($server)
    }
    $marker = 'EXECUTE_SCRIPT "data\\scripts\\efa_multiplayer.lua"'
    if ([Text.Encoding]::ASCII.GetString($serverBytes).Contains($marker)) { return }
    $backup = Join-Path $BackupRoot 'data\scripts\server.lua'
    New-Item -ItemType Directory -Path (Split-Path -Parent $backup) -Force | Out-Null
    Copy-Item -LiteralPath $server -Destination $backup -Force
    $suffix = [Text.Encoding]::ASCII.GetBytes("`r`n-- Kraken multiplayer lifecycle adapter`r`n$marker`r`n")
    $stream = [IO.File]::Open($server, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($suffix, 0, $suffix.Length) }
    finally { $stream.Dispose() }
}

function Deploy-RemoteOverlay {
    $archive = Join-Path $script:RunRoot 'overlay.zip'
    Copy-ToRemote $archive $script:RemoteScpStage
    $remoteFiles = @($script:OverlayFiles | ForEach-Object { $_.Target }) | ConvertTo-Json -Compress
    $scriptText = @"
`$root = '$($RemoteGameRoot.Replace("'", "''"))'
`$archive = '$($script:RemoteStage.Replace("'", "''"))'
`$runId = '$script:RunId'
`$ProgressPreference = 'SilentlyContinue'
`$files = '$($remoteFiles.Replace("'", "''"))' | ConvertFrom-Json
`$required = @('hta.exe','data\\scripts\\efa.lua','data\\maps\\r0m0\\cinematriggers.xml')
`$missing = `$required | Where-Object { -not (Test-Path -LiteralPath (Join-Path `$root `$_)) }
`$raidTriggerCandidates = @('data\\maps\\r1m1\\triggers.xml','data\\maps\\r1m1\\winter\\spring\\summer\\autumn\\main\\triggers.xml')
if (-not (`$raidTriggerCandidates | Where-Object { Test-Path -LiteralPath (Join-Path `$root `$_) })) { `$missing += 'r1m1 triggers.xml (flat or Community Remaster layout)' }
if (`$missing) { Write-Error ('Remote target is not a ComMod-installed EFA copy. Missing: ' + (`$missing -join ', ')); exit 3 }
`$stage = Join-Path `$env:TEMP ('efa-mp-unpack-' + `$runId)
`$backupRoot = Join-Path `$root ('backups\\efa-mp-harness\\' + `$runId)
Remove-Item -LiteralPath `$stage -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -LiteralPath `$archive -DestinationPath `$stage -Force
foreach (`$relative in `$files) {
    `$source = Join-Path `$stage `$relative
    `$target = Join-Path `$root `$relative
    `$backup = Join-Path `$backupRoot `$relative
    if (Test-Path -LiteralPath `$target) {
        New-Item -ItemType Directory -Path (Split-Path -Parent `$backup) -Force | Out-Null
        Copy-Item -LiteralPath `$target -Destination `$backup -Force
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent `$target) -Force | Out-Null
    Copy-Item -LiteralPath `$source -Destination `$target -Force
    `$hash = (Get-FileHash -LiteralPath `$target -Algorithm SHA256).Hash
    Write-Output (`$relative.Replace('\','/') + '=' + `$hash)
}
`$raidHook = Join-Path `$root 'data\maps\r0m0\cinematriggers.xml'
`$encoding = [Text.Encoding]::GetEncoding(1251)
`$content = [IO.File]::ReadAllText(`$raidHook, `$encoding)
`$marker = '-- Kraken multiplayer raid start'
`$diagnostic = 'LOG("EFA MP: StartMatchmaking trigger entered")'
if (-not (`$content.Contains(`$marker) -and `$content.Contains(`$diagnostic))) {
    `$pattern = '(<trigger Name="StartMatchmaking" active="0">\s*<event\s+timeout="0"\s+eventid="GE_TIME_PERIOD"\s*/>\s*<script>)'
    `$regex = [regex]::new(`$pattern)
    if (-not `$regex.IsMatch(`$content)) { throw "Cannot find StartMatchmaking hook in `$raidHook" }
    `$hookBackup = Join-Path `$backupRoot 'data\maps\r0m0\cinematriggers.xml'
    New-Item -ItemType Directory -Path (Split-Path -Parent `$hookBackup) -Force | Out-Null
    Copy-Item -LiteralPath `$raidHook -Destination `$hookBackup -Force
    `$insertion = "`r`n`t`t`t`$marker`r`n`t`t`t`$diagnostic`r`n`t`t`tEFA_MP.BeginRaid()"
    if (`$content.Contains(`$marker)) {
        [IO.File]::WriteAllText(`$raidHook, `$content.Replace(`$marker, "`$marker`r`n`t`t`t`$diagnostic"), `$encoding)
    }
    else {
        [IO.File]::WriteAllText(`$raidHook, `$regex.Replace(`$content, '`$1' + `$insertion, 1), `$encoding)
    }
}
`$server = Join-Path `$root 'data\scripts\server.lua'
`$efa = Join-Path `$root 'data\scripts\efa.lua'
`$serverBytes = [IO.File]::ReadAllBytes(`$server)
`$efaBytes = [IO.File]::ReadAllBytes(`$efa)
if ([Text.Encoding]::ASCII.GetString(`$serverBytes).Contains('IfLoadSave()') -and -not [Text.Encoding]::ASCII.GetString(`$efaBytes).Contains('function IfLoadSave')) {
    `$knownGood = Get-ChildItem -LiteralPath (Join-Path `$root 'backups\efa-mp-harness') -Recurse -Filter server.lua -ErrorAction SilentlyContinue |
        Where-Object { -not [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes(`$_.FullName)).Contains('IfLoadSave()') } |
        Sort-Object LastWriteTime | Select-Object -First 1
    if (`$null -eq `$knownGood) { throw "Cannot restore compatible server.lua for `$root" }
    Copy-Item -LiteralPath `$knownGood.FullName -Destination `$server -Force
}
`$marker = 'EXECUTE_SCRIPT "data\\scripts\\efa_multiplayer.lua"'
`$serverBytes = [IO.File]::ReadAllBytes(`$server)
if (-not [Text.Encoding]::ASCII.GetString(`$serverBytes).Contains(`$marker)) {
    `$serverBackup = Join-Path `$backupRoot 'data\scripts\server.lua'
    New-Item -ItemType Directory -Path (Split-Path -Parent `$serverBackup) -Force | Out-Null
    Copy-Item -LiteralPath `$server -Destination `$serverBackup -Force
    `$suffix = [Text.Encoding]::ASCII.GetBytes("`r`n-- Kraken multiplayer lifecycle adapter`r`n`$marker`r`n")
    `$stream = [IO.File]::Open(`$server, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { `$stream.Write(`$suffix, 0, `$suffix.Length) }
    finally { `$stream.Dispose() }
}
Remove-Item -LiteralPath `$stage -Recurse -Force
Remove-Item -LiteralPath `$archive -Force
"@
    $remoteHashes = Invoke-RemotePowerShell $scriptText
    $expected = Get-Content -LiteralPath (Join-Path $script:OverlayRoot 'overlay-manifest.json') -Raw | ConvertFrom-Json
    foreach ($entry in $expected) {
        $line = $remoteHashes | Where-Object { $_ -like "$($entry.path)=*" } | Select-Object -First 1
        if ($null -eq $line -or $line.Split('=')[1] -ne $entry.sha256) {
            throw "Remote hash verification failed for $($entry.path)"
        }
    }
}

function Get-LocalLanAddress {
    $remoteIp = ($RemoteSsh -split '@')[-1]
    $udp = [Net.Sockets.UdpClient]::new()
    try {
        $udp.Connect($remoteIp, 9)
        return ([Net.IPEndPoint]$udp.Client.LocalEndPoint).Address.IPAddressToString
    }
    finally { $udp.Dispose() }
}

function Invoke-Smoke {
    $localPeer = Join-Path $LocalGameRoot 'kraken_net_peer_test.exe'
    Assert-File $localPeer
    $localIp = Get-LocalLanAddress
    $hostOut = Join-Path $script:RunRoot 'host.out.log'
    $hostErr = Join-Path $script:RunRoot 'host.err.log'
    $hostProcess = Start-Process -FilePath $localPeer -ArgumentList @('host', "$SmokePort", $localIp, '--scripted-snapshots', '--scripted-input', '--scripted-weapon', '--scripted-despawn') -WindowStyle Hidden -RedirectStandardOutput $hostOut -RedirectStandardError $hostErr -PassThru
    Start-Sleep -Milliseconds 300
    $scriptText = @"
& '$($RemoteGameRoot.Replace("'", "''"))\kraken_net_peer_test.exe' client $SmokePort $localIp --scripted-snapshots --scripted-input --scripted-weapon --scripted-despawn
exit `$LASTEXITCODE
"@
    try { Invoke-RemotePowerShell $scriptText | Set-Content -LiteralPath (Join-Path $script:RunRoot 'client.out.log') -Encoding UTF8 }
    finally { $hostProcess.WaitForExit(25000) | Out-Null }
    $errors = [string](Get-Content -LiteralPath $hostErr -Raw)
    if (-not [string]::IsNullOrEmpty([string]$errors)) { throw "Local smoke host failed: $errors" }
    $clientLog = [string](Get-Content -LiteralPath (Join-Path $script:RunRoot 'client.out.log') -Raw)
    foreach ($marker in @('entity_assign=42', 'weapon entity=42', 'entity_despawn=42')) {
        if (-not $clientLog.Contains($marker)) { throw "Remote smoke log lacks: $marker" }
    }
    Write-Host "LAN peer smoke passed; local host endpoint was $localIp`:$SmokePort"
}

function Collect-Logs {
    $localLog = Join-Path $LocalGameRoot 'kraken.log'
    if (Test-Path -LiteralPath $localLog) {
        Get-Content -LiteralPath $localLog -Tail 2000 | Set-Content -LiteralPath (Join-Path $script:RunRoot 'local-kraken.log') -Encoding UTF8
    }
    $scriptText = @"
`$log = Join-Path '$($RemoteGameRoot.Replace("'", "''"))' 'kraken.log'
if (Test-Path -LiteralPath `$log) { Get-Content -LiteralPath `$log -Tail 2000 }
else { Write-Output 'kraken.log: missing (game has not been started on this target)' }
"@
    Invoke-RemotePowerShell $scriptText | Set-Content -LiteralPath (Join-Path $script:RunRoot 'remote-kraken.log') -Encoding UTF8
}

New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
switch ($Action) {
    'Package' { New-ComModPackage }
    'ProvisionRemote' { Provision-RemoteTarget }
    'Preflight' { Test-LocalTarget; Test-RemoteTarget }
    'Deploy' { Stop-LocalGame; Stop-RemoteGame; New-Overlay; Test-LocalTarget; Test-RemoteTarget; Deploy-LocalOverlay; Deploy-RemoteOverlay }
    'Smoke' { Invoke-Smoke }
    'Collect' { Collect-Logs }
    'All' { Stop-LocalGame; Stop-RemoteGame; New-ComModPackage; New-Overlay; Test-LocalTarget; Test-RemoteTarget; Deploy-LocalOverlay; Deploy-RemoteOverlay; Invoke-Smoke; Collect-Logs }
}

Write-Host "Artifacts: $script:RunRoot"
