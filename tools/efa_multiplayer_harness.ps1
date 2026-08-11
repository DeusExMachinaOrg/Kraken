[CmdletBinding()]
param(
    [ValidateSet('Package', 'ProvisionRemote', 'Preflight', 'Deploy', 'Smoke', 'RaidCrashSmoke', 'CombatHostKillsClient', 'CombatClientKillsHost', 'Collect', 'All')]
    [string]$Action = 'All',
    [string]$LocalGameRoot = 'E:\HTA_EFA',
    [string]$RemoteSsh = 'etozh@192.168.2.80',
    [string]$RemoteGameRoot = 'E:\HTA_EFA',
    [string]$EfaRoot = 'E:\code\Escape-from-Apocalypse',
    [string]$KrakenBuildRoot = 'E:\KrakenWorkspace\Kraken\build-ninja',
    [int]$SmokePort = 27830,
    [ValidateRange(30, 600)]
    [int]$RaidTestTimeoutSeconds = 180,
    [ValidateRange(5, 120)]
    [int]$RaidTestStableSeconds = 20,
    [ValidateRange(1, 30)]
    [int]$RaidTestHostWarmupSeconds = 5,
    [ValidateRange(5, 300)]
    [int]$CombatKillTimeoutSeconds = 30,
    [ValidateRange(5, 60)]
    [int]$RemoteCommandTimeoutSeconds = 10
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
$script:RemoteRaidTaskName = $null
$script:OverlayFiles = @(
    [PSCustomObject]@{ Source = 'kraken.dll'; Target = 'kraken.dll' },
    [PSCustomObject]@{ Source = 'kraken_net_peer_test.exe'; Target = 'kraken_net_peer_test.exe' },
    [PSCustomObject]@{ Source = 'data\scripts\efa_multiplayer.lua'; Target = 'data\scripts\efa_multiplayer.lua' },
    [PSCustomObject]@{ Source = 'data\scripts\efa.lua'; Target = 'data\scripts\efa.lua' },
    [PSCustomObject]@{ Source = 'data\scripts\server.lua'; Target = 'data\scripts\server.lua' },
    [PSCustomObject]@{
        Source = 'data\maps\r1m1\winter\spring\summer\autumn\main\triggers.xml'
        Target = 'data\maps\r1m1\winter\spring\summer\autumn\main\triggers.xml'
    }
)

$script:OverlayFiles += @(
    [PSCustomObject]@{ Source = 'data\\multiplayer\\r1m1_player_slots.xml'; Target = 'data\\multiplayer\\r1m1_player_slots.xml' },
    [PSCustomObject]@{ Source = 'data\\maps\\r1m1\\winter\\dynamicscene.xml'; Target = 'data\\maps\\r1m1\\winter\\dynamicscene.xml' },
    [PSCustomObject]@{ Source = 'data\\maps\\r1m1\\winter\\spring\\dynamicscene.xml'; Target = 'data\\maps\\r1m1\\winter\\spring\\dynamicscene.xml' },
    [PSCustomObject]@{ Source = 'data\\maps\\r1m1\\winter\\spring\\summer\\dynamicscene.xml'; Target = 'data\\maps\\r1m1\\winter\\spring\\summer\\dynamicscene.xml' }
)

function Assert-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
}

function ConvertTo-EncodedPowerShell([string]$ScriptText) {
    return [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($ScriptText))
}

function Invoke-SshBounded([string]$RemoteCommand) {
    $stdout = Join-Path $env:TEMP "efa-mp-ssh-$script:RunId-$([guid]::NewGuid().ToString('N')).out"
    $stderr = "$stdout.err"
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = 'ssh.exe'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    # Windows PowerShell 5.1's ProcessStartInfo has no ArgumentList. ssh
    # deliberately treats every argument after HOST as the remote command, so
    # the encoded command can safely remain the tail of Arguments.
    $info.Arguments = "-o BatchMode=yes -o ConnectTimeout=10 $RemoteSsh $RemoteCommand"
    $process = [Diagnostics.Process]::Start($info)
    if ($null -eq $process) { throw 'Could not start ssh.exe' }
    try {
        $outTask = $process.StandardOutput.ReadToEndAsync()
        $errTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($RemoteCommandTimeoutSeconds * 1000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Remote command exceeded $RemoteCommandTimeoutSeconds seconds"
        }
        $stdoutText = $outTask.GetAwaiter().GetResult()
        $stderrText = $errTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            throw "Remote command failed ($($process.ExitCode)): $stdoutText$stderrText"
        }
        return @($stdoutText -split "`r?`n" | Where-Object { $_ -ne '' })
    }
    finally {
        $process.Dispose()
    }
}

function Invoke-RemotePowerShell([string]$ScriptText) {
    $encoded = ConvertTo-EncodedPowerShell $ScriptText
    if ($encoded.Length -le 7000) {
        return Invoke-SshBounded "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded"
    }
    else {
        $localScript = Join-Path $env:TEMP "efa-mp-$script:RunId.ps1"
        [IO.File]::WriteAllText($localScript, $ScriptText, [Text.Encoding]::Unicode)
        try {
            Copy-ToRemote $localScript $script:RemoteScpScriptStage
            return Invoke-SshBounded "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$script:RemoteScriptStage`""
        }
        finally {
            Remove-Item -LiteralPath $localScript -Force -ErrorAction SilentlyContinue
            try { Invoke-SshBounded "del /q `"$script:RemoteScriptStage`"" | Out-Null } catch { }
        }
    }
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
    # Escape from Apocalypse replaces (rather than merges) r1m1's base
    # DynamicScene with the selected seasonal scene.  Every selectable scene
    # therefore needs the same map-owned slots.  Inject one block per *file*;
    # only one of those files is active in a given raid.
    $baseRelative = 'data\\maps\\r1m1\\dynamicscene.xml'
    $baseSource = Join-Path $LocalGameRoot $baseRelative
    Assert-File $baseSource
    $baseTarget = Join-Path $script:OverlayRoot $baseRelative
    New-Item -ItemType Directory -Path (Split-Path -Parent $baseTarget) -Force | Out-Null
    Copy-Item -LiteralPath $baseSource -Destination $baseTarget -Force
    $encoding = [Text.Encoding]::GetEncoding(1251)
    $slotMarker = '<!-- Kraken multiplayer player slots -->'
    $slotSpec = Join-Path $script:OverlayRoot 'data\\multiplayer\\r1m1_player_slots.xml'
    $slotContent = [IO.File]::ReadAllText($slotSpec)
    $closeTag = '</DynamicScene>'
    $script:OverlayFiles += [PSCustomObject]@{ Source = $baseRelative; Target = $baseRelative }
    $sceneTargets = @(
        $baseRelative,
        'data\\maps\\r1m1\\winter\\dynamicscene.xml',
        'data\\maps\\r1m1\\winter\\spring\\dynamicscene.xml',
        'data\\maps\\r1m1\\winter\\spring\\summer\\dynamicscene.xml'
    )
    foreach ($sceneTarget in $sceneTargets) {
        $scenePath = Join-Path $script:OverlayRoot $sceneTarget
        Assert-File $scenePath
        $sceneContent = [IO.File]::ReadAllText($scenePath, $encoding)
        if (-not $sceneContent.Contains($closeTag)) {
            throw "r1m1 DynamicScene has no $closeTag closing tag: $sceneTarget"
        }
        if ($sceneContent.Contains($slotMarker)) {
            # Match only the final standalone DynamicScene element: the
            # fragment's own comment mentions the closing-tag text.
            $slotPattern = '(?s)' + [regex]::Escape($slotMarker) + '.*?(?=\r?\n\s*' + [regex]::Escape($closeTag) + '\s*$)'
            $sceneContent = [regex]::Replace($sceneContent, $slotPattern, $slotMarker + [Environment]::NewLine + $slotContent + [Environment]::NewLine, 1)
        } else {
            $sceneContent = $sceneContent.Replace($closeTag, $slotMarker + [Environment]::NewLine + $slotContent + [Environment]::NewLine + $closeTag)
        }
        [IO.File]::WriteAllText($scenePath, $sceneContent, $encoding)
    }
    foreach ($file in ($script:OverlayFiles | Where-Object { $_.Target -in $sceneTargets })) {
        $content = [IO.File]::ReadAllText((Join-Path $script:OverlayRoot $file.Target))
        foreach ($name in @('MP_SPAWN_1','MP_SPAWN_2','MP_SPAWN_3','MP_SPAWN_4','MP_PROXY_1','MP_PROXY_2','MP_PROXY_3','MP_PROXY_4')) {
            $count = [regex]::Matches($content, [regex]::Escape('Name="' + $name + '"')).Count
            if ($count -ne 1) {
                throw "Player-slot marker count in overlay $($file.Target) for $name must be 1; got $count"
            }
        }
        foreach ($name in @('MP_PROXY_1','MP_PROXY_2','MP_PROXY_3','MP_PROXY_4')) {
            $prototypePattern = '<Object\s+Name="' + [regex]::Escape($name) +
                '"[^>]*\sPrototype="Bug01ForStart"'
            if ([regex]::Matches($content, $prototypePattern).Count -ne 1) {
                throw "Player proxy in overlay $($file.Target) must use the validated Bug01ForStart prototype: $name"
            }
        }
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

function Get-ExceptionInventory([string]$GameRoot) {
    $exceptionRoot = Join-Path $GameRoot 'exceptions'
    if (-not (Test-Path -LiteralPath $exceptionRoot)) { return @() }
    return @(
        Get-ChildItem -LiteralPath $exceptionRoot -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^hta\.exe\d+\.(dmp|log|game\.log)$' } |
            ForEach-Object { "$($_.Name)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
    )
}

function Get-CombatWeaponPart([string]$GameRoot) {
    # CurrentWeaponGroups is persisted by the game's native WeaponGroupManager.
    # Do not choose a weapon prototype or alter the player's loadout: return the
    # first actually selected part from the current save.
    $map = @(Get-ChildItem -LiteralPath (Join-Path $GameRoot 'data\profiles') -Filter currentmap.xml -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1)
    if ($map.Count -eq 0) { throw "Combat autotest cannot find currentmap.xml under $GameRoot\data\profiles" }
    $text = [IO.File]::ReadAllText($map[0].FullName)
    $match = [regex]::Match($text, 'weaponParts\s*=\s*"(?<parts>[^"]+)"')
    if (-not $match.Success) { throw "Combat autotest has no selected weapon group in $($map[0].FullName)" }
    $part = ($match.Groups['parts'].Value -split '[,;\s]+' | Where-Object { $_ -ne '' } | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($part)) { throw "Combat autotest selected weapon group is empty in $($map[0].FullName)" }
    return $part
}

function Get-RemoteCombatWeaponPart {
    $scriptText = @"
`$root = '$($RemoteGameRoot.Replace("'", "''"))'
`$profiles = Join-Path `$root 'data\profiles'
`$map = @(Get-ChildItem -LiteralPath `$profiles -Filter currentmap.xml -File -Recurse -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1)
if (`$map.Count -eq 0) { throw "Combat autotest cannot find currentmap.xml under `$profiles" }
`$text = [IO.File]::ReadAllText(`$map[0].FullName)
`$match = [regex]::Match(`$text, 'weaponParts\s*=\s*"(?<parts>[^"]+)"')
if (-not `$match.Success) { throw "Combat autotest has no selected weapon group in `$(`$map[0].FullName)" }
`$part = (`$match.Groups['parts'].Value -split '[,;\s]+' | Where-Object { `$_ -ne '' } | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace(`$part)) { throw "Combat autotest selected weapon group is empty in `$(`$map[0].FullName)" }
Write-Output `$part
"@
    return (Invoke-RemotePowerShell $scriptText | Select-Object -First 1)
}

function Start-LocalRaidTestGame([hashtable]$Environment) {
    $exe = Join-Path $LocalGameRoot 'hta.exe'
    Assert-File $exe
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $exe
    $info.WorkingDirectory = $LocalGameRoot
    $info.UseShellExecute = $false
    foreach ($entry in $Environment.GetEnumerator()) {
        $info.EnvironmentVariables[$entry.Key] = [string]$entry.Value
    }
    $process = [Diagnostics.Process]::Start($info)
    if ($null -eq $process) { throw 'Failed to start local hta.exe' }
    return $process.Id
}

function Start-RemoteRaidTestGame([hashtable]$Environment) {
    # The SSH service session cannot initialize the game's D3D device.  Start
    # hta.exe through a short-lived scheduled task with the active user's
    # InteractiveToken, then discover the created process by its start time.
    $environmentJson = ($Environment | ConvertTo-Json -Compress).Replace("'", "''")
    $scriptText = @"
`$root = '$($RemoteGameRoot.Replace("'", "''"))'
`$exe = Join-Path `$root 'hta.exe'
if (-not (Test-Path -LiteralPath `$exe)) { throw "Missing remote hta.exe: `$exe" }
`$environment = '$environmentJson' | ConvertFrom-Json
`$active = @(query user 2>`$null | ForEach-Object {
    if (`$_ -match '^\s*>?\s*(\S+)\s+(?:console|rdp-tcp#\d+)\s+(\d+)\s+Active\b') {
        [PSCustomObject]@{ User = `$matches[1]; SessionId = [int]`$matches[2] }
    }
}) | Select-Object -First 1
if (`$null -eq `$active) { throw 'No active interactive Windows session is available on the remote PC' }
`$taskName = '\Kraken-EFA-RaidSmoke-$script:RunId'
`$stage = Join-Path `$env:TEMP 'kraken-efa-raid-smoke-$script:RunId.cmd'
`$wrapperLines = [Collections.Generic.List[string]]::new()
`$wrapperLines.Add('@echo off')
`$wrapperLines.Add('setlocal')
foreach (`$entry in `$environment.PSObject.Properties) {
    `$wrapperLines.Add(('set "' + `$entry.Name + '=' + [string]`$entry.Value + '"'))
}
`$wrapperLines.Add(('cd /d "' + `$root + '"'))
`# Do not detach hta.exe from the scheduled task. Task Scheduler otherwise
`# tears down the job shortly after cmd.exe exits, which looks like a client
`# crash before the engine has loaded its first level.
`$wrapperLines.Add(('"' + `$exe + '"'))
[IO.File]::WriteAllLines(`$stage, `$wrapperLines, [Text.Encoding]::ASCII)
`$taskCommand = 'cmd.exe /d /c "' + `$stage + '"'
`$scheduleTime = (Get-Date).AddMinutes(1).ToString('HH:mm')
`$runAfter = [DateTime]::UtcNow.AddSeconds(-1)
& schtasks.exe /Create /TN `$taskName /SC ONCE /ST `$scheduleTime /TR `$taskCommand /RU `$active.User /IT /F | Out-Null
if (`$LASTEXITCODE -ne 0) { Remove-Item -LiteralPath `$stage -Force -ErrorAction SilentlyContinue; throw 'Could not create interactive remote launch task' }
& schtasks.exe /Run /TN `$taskName | Out-Null
if (`$LASTEXITCODE -ne 0) { & schtasks.exe /Delete /TN `$taskName /F | Out-Null; Remove-Item -LiteralPath `$stage -Force -ErrorAction SilentlyContinue; throw 'Could not run interactive remote launch task' }
`$process = `$null
`$deadline = (Get-Date).AddSeconds(20)
do {
    `$process = @(Get-Process -Name 'hta' -ErrorAction SilentlyContinue |
        Where-Object { `$_.StartTime.ToUniversalTime() -ge `$runAfter } |
        Sort-Object StartTime -Descending | Select-Object -First 1)
    if (`$process.Count -ne 0) { break }
    Start-Sleep -Milliseconds 250
} while ((Get-Date) -lt `$deadline)
Remove-Item -LiteralPath `$stage -Force -ErrorAction SilentlyContinue
if (`$process.Count -eq 0) { & schtasks.exe /Delete /TN `$taskName /F | Out-Null; throw 'Interactive remote launch task did not create hta.exe within 20 seconds' }
[PSCustomObject]@{ processId = `$process[0].Id; taskName = `$taskName; user = `$active.User; sessionId = `$active.SessionId } | ConvertTo-Json -Compress
"@
    $json = Invoke-RemotePowerShell $scriptText | Select-Object -Last 1
    $launch = $json | ConvertFrom-Json
    $remoteProcessId = [int]$launch.processId
    if ($remoteProcessId -le 0 -or [string]::IsNullOrWhiteSpace([string]$launch.taskName)) {
        throw "Interactive remote hta.exe launch did not return a valid process: $json"
    }
    $script:RemoteRaidTaskName = [string]$launch.taskName
    return $launch
}

function Remove-RemoteRaidTestTask {
    if ([string]::IsNullOrWhiteSpace($script:RemoteRaidTaskName)) { return }
    $taskName = $script:RemoteRaidTaskName.Replace("'", "''")
    $script:RemoteRaidTaskName = $null
    $scriptText = @"
& schtasks.exe /Delete /TN '$taskName' /F 2>`$null | Out-Null
"@
    Invoke-RemotePowerShell $scriptText | Out-Null
}

function Get-LocalLogLength {
    $log = Join-Path $LocalGameRoot 'kraken.log'
    if (-not (Test-Path -LiteralPath $log)) { return [int64]0 }
    return [int64](Get-Item -LiteralPath $log).Length
}

function Get-RemoteLogLength {
    $scriptText = @"
`$log = Join-Path '$($RemoteGameRoot.Replace("'", "''"))' 'kraken.log'
if (Test-Path -LiteralPath `$log) { Write-Output ([string](Get-Item -LiteralPath `$log).Length) } else { Write-Output '0' }
"@
    return [int64](Invoke-RemotePowerShell $scriptText | Select-Object -Last 1)
}

function Read-LogDelta([string]$Path, [int64]$Offset) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return [PSCustomObject]@{ text = ''; nextOffset = [int64]0 }
    }
    $length = [int64](Get-Item -LiteralPath $Path).Length
    # A log rotation/truncation starts a new generation, so its full contents
    # belong to this run even when the old byte offset is beyond EOF.
    $start = if ($length -lt $Offset) { [int64]0 } else { $Offset }
    if ($length -eq $start) {
        return [PSCustomObject]@{ text = ''; nextOffset = $length }
    }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $stream.Seek($start, [IO.SeekOrigin]::Begin) | Out-Null
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
        try { $text = $reader.ReadToEnd() }
        finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
    return [PSCustomObject]@{ text = $text; nextOffset = $length }
}

function Get-LocalRaidTestProbe([int]$ProcessId, [string[]]$BaselineExceptions, [int64]$LogOffset) {
    $alive = $null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
    $inventory = Get-ExceptionInventory $LocalGameRoot
    $newExceptions = @($inventory | Where-Object { $_ -notin $BaselineExceptions })
    $log = Join-Path $LocalGameRoot 'kraken.log'
    $delta = Read-LogDelta $log $LogOffset
    return [PSCustomObject]@{ alive = $alive; newExceptions = $newExceptions; logDelta = $delta.text; logNextOffset = $delta.nextOffset }
}

function Get-RemoteRaidTestProbe([int]$ProcessId, [string[]]$BaselineExceptions, [int64]$LogOffset) {
    $baselineJson = $BaselineExceptions | ConvertTo-Json -Compress
    $scriptText = @"
`$root = '$($RemoteGameRoot.Replace("'", "''"))'
`$baseline = '$($baselineJson.Replace("'", "''"))' | ConvertFrom-Json
`$exceptionRoot = Join-Path `$root 'exceptions'
`$inventory = @()
if (Test-Path -LiteralPath `$exceptionRoot) {
    `$inventory = @(Get-ChildItem -LiteralPath `$exceptionRoot -File -ErrorAction SilentlyContinue |
        Where-Object { `$_.Name -match '^hta\.exe\d+\.(dmp|log|game\.log)`$' } |
        ForEach-Object { "`$(`$_.Name)|`$(`$_.Length)|`$(`$_.LastWriteTimeUtc.Ticks)" })
}
`$log = Join-Path `$root 'kraken.log'
`$offset = [int64]$LogOffset
`$deltaText = ''
`$nextOffset = [int64]0
if (Test-Path -LiteralPath `$log) {
    `$length = [int64](Get-Item -LiteralPath `$log).Length
    `$start = if (`$length -lt `$offset) { [int64]0 } else { `$offset }
    if (`$length -gt `$start) {
        `$stream = [IO.File]::Open(`$log, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            `$stream.Seek(`$start, [IO.SeekOrigin]::Begin) | Out-Null
            `$reader = [IO.StreamReader]::new(`$stream, [Text.Encoding]::UTF8, `$true)
            try { `$deltaText = `$reader.ReadToEnd() } finally { `$reader.Dispose() }
        } finally { `$stream.Dispose() }
    }
    `$nextOffset = `$length
}
[PSCustomObject]@{
    alive = (`$null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue))
    newExceptions = @(`$inventory | Where-Object { `$_ -notin `$baseline })
    logDelta = `$deltaText
    logNextOffset = `$nextOffset
} | ConvertTo-Json -Compress
"@
    $json = Invoke-RemotePowerShell $scriptText | Select-Object -Last 1
    return $json | ConvertFrom-Json
}

function Write-RaidTestDiagnostics {
    Collect-Logs
    $localExceptions = Join-Path $LocalGameRoot 'exceptions'
    if (Test-Path -LiteralPath $localExceptions) {
        Copy-Item -LiteralPath $localExceptions -Destination (Join-Path $script:RunRoot 'local-exceptions') -Recurse -Force -ErrorAction SilentlyContinue
    }
    $remoteExceptions = Join-Path $script:RunRoot 'remote-exceptions'
    New-Item -ItemType Directory -Path $remoteExceptions -Force | Out-Null
    $remoteScript = @"
`$root = '$($RemoteGameRoot.Replace("'", "''"))'
`$source = Join-Path `$root 'exceptions'
if (Test-Path -LiteralPath `$source) {
    Get-ChildItem -LiteralPath `$source -File -ErrorAction SilentlyContinue |
        Where-Object { `$_.Name -match '^hta\.exe\d+\.(dmp|log|game\.log)`$' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 4 |
        ForEach-Object { Write-Output (`$_.FullName) }
}
"@
    $files = @(Invoke-RemotePowerShell $remoteScript | Where-Object { $_ -like '*.dmp' -or $_ -like '*.log' })
    foreach ($file in $files) {
        $leaf = Split-Path -Leaf $file
        & scp -q "$RemoteSsh`:$($file.Replace('\', '/'))" (Join-Path $remoteExceptions $leaf)
    }
}

function Invoke-RaidCrashSmoke([string]$CombatScenario = '') {
    Stop-LocalGame
    Stop-RemoteGame
    $localIp = Get-LocalLanAddress
    $baselineLocal = Get-ExceptionInventory $LocalGameRoot
    $localLogOffset = Get-LocalLogLength
    $baselineRemoteScript = @"
`$root = '$($RemoteGameRoot.Replace("'", "''"))'
`$exceptionRoot = Join-Path `$root 'exceptions'
if (Test-Path -LiteralPath `$exceptionRoot) {
    Get-ChildItem -LiteralPath `$exceptionRoot -File -ErrorAction SilentlyContinue |
        Where-Object { `$_.Name -match '^hta\.exe\d+\.(dmp|log|game\.log)`$' } |
        ForEach-Object { "`$(`$_.Name)|`$(`$_.Length)|`$(`$_.LastWriteTimeUtc.Ticks)" }
}
"@
    $baselineRemote = @(Invoke-RemotePowerShell $baselineRemoteScript)
    $remoteLogOffset = Get-RemoteLogLength
    $commonEnvironment = @{
        KRAKEN_MP_ENABLED = '1'; KRAKEN_MP_AUTOSTART = '0'; KRAKEN_MP_AUTO_LAN = '0'
        KRAKEN_MP_PORT = [string]$SmokePort; KRAKEN_MP_MAX_PEERS = '2'
        KRAKEN_MP_SPAWN_TOGETHER = '1'; KRAKEN_EFA_RAID_AUTOTEST = '1'
    }
    if ($CombatScenario -ne '') {
        $commonEnvironment['KRAKEN_EFA_COMBAT_AUTOTEST'] = $CombatScenario
    }
    $hostEnvironment = @{} + $commonEnvironment
    $hostEnvironment['KRAKEN_MP_HOST'] = '1'
    $hostEnvironment['KRAKEN_MP_ADDRESS'] = '0.0.0.0'
    $clientEnvironment = @{} + $commonEnvironment
    $clientEnvironment['KRAKEN_MP_HOST'] = '0'
    $clientEnvironment['KRAKEN_MP_ADDRESS'] = $localIp
    if ($CombatScenario -ne '') {
        $hostEnvironment['KRAKEN_EFA_COMBAT_WEAPON_PART'] = Get-CombatWeaponPart $LocalGameRoot
        $clientEnvironment['KRAKEN_EFA_COMBAT_WEAPON_PART'] = Get-RemoteCombatWeaponPart
        Write-Host "Combat autotest: host weapon=$($hostEnvironment['KRAKEN_EFA_COMBAT_WEAPON_PART']); client weapon=$($clientEnvironment['KRAKEN_EFA_COMBAT_WEAPON_PART'])"
    }

    $hostPid = 0
    $clientPid = 0
    $failure = $null
    $hostPeerObserved = $false
    $clientPeerObserved = $false
    $replicaObserved = $false
    $replicaObservedAt = $null
    $combatStarted = $false
    $deathObserved = $false
    $deathObservedAt = $null
    $combatDeadline = $null
    try {
        $hostPid = Start-LocalRaidTestGame $hostEnvironment
        Write-Host "Raid crash smoke: local host PID=$hostPid; warming up $RaidTestHostWarmupSeconds s"
        Start-Sleep -Seconds $RaidTestHostWarmupSeconds
        $clientLaunch = Start-RemoteRaidTestGame $clientEnvironment
        $clientPid = [int]$clientLaunch.processId
        Write-Host "Raid crash smoke: remote client PID=$clientPid in $($clientLaunch.user) session $($clientLaunch.sessionId); waiting for replicated NPC"
        $deadline = (Get-Date).AddSeconds($RaidTestTimeoutSeconds)
        while ((Get-Date) -lt $deadline) {
            $local = Get-LocalRaidTestProbe $hostPid $baselineLocal $localLogOffset
            $remote = Get-RemoteRaidTestProbe $clientPid $baselineRemote $remoteLogOffset
            $localLogOffset = [int64]$local.logNextOffset
            $remoteLogOffset = [int64]$remote.logNextOffset
            if (-not $local.alive) { throw 'Host hta.exe exited unexpectedly' }
            if (-not $remote.alive) { throw 'Client hta.exe exited unexpectedly' }
            if ($local.newExceptions.Count -ne 0) { throw "Host wrote exception record: $($local.newExceptions -join '; ')" }
            if ($remote.newExceptions.Count -ne 0) { throw "Client wrote exception record: $($remote.newExceptions -join '; ')" }
            if (-not $hostPeerObserved -and $local.logDelta -match 'peer=1 rtt=') {
                $hostPeerObserved = $true
                Write-Host 'Raid crash smoke: host observed its connected peer'
            }
            if (-not $clientPeerObserved -and $remote.logDelta -match 'peer=1 rtt=') {
                $clientPeerObserved = $true
                Write-Host 'Raid crash smoke: client observed its connected peer'
            }
            if ($CombatScenario -ne '' -and $null -eq $combatDeadline -and
                $hostPeerObserved -and $clientPeerObserved) {
                $combatDeadline = (Get-Date).AddSeconds($CombatKillTimeoutSeconds)
                Write-Host "Combat autotest: both players joined; kill deadline is $CombatKillTimeoutSeconds s"
            }
            if ($CombatScenario -ne '' -and $null -ne $combatDeadline -and
                (Get-Date) -gt $combatDeadline -and -not $deathObserved) {
                throw "Combat autotest did not kill its target within $CombatKillTimeoutSeconds seconds after both players joined ($CombatScenario)"
            }
            if (-not $replicaObserved -and $remote.logDelta -match 'created remote NPC replica') {
                $replicaObserved = $true
                $replicaObservedAt = Get-Date
                Write-Host 'Raid crash smoke: remote NPC replica observed; starting stability window'
            }
            if ($CombatScenario -ne '' -and -not $combatStarted -and
                $local.logDelta -match "KRAKEN_COMBAT_AUTOTEST start scenario=$([regex]::Escape($CombatScenario))") {
                $combatStarted = $true
                Write-Host "Combat autotest: native weapon path started ($CombatScenario)"
            }
            if ($CombatScenario -ne '' -and -not $deathObserved -and
                $local.logDelta -match "KRAKEN_COMBAT_AUTOTEST death scenario=$([regex]::Escape($CombatScenario))") {
                $deathObserved = $true
                $deathObservedAt = Get-Date
                Write-Host "Combat autotest: authoritative death observed ($CombatScenario)"
            }
            if ($CombatScenario -ne '' -and $hostPeerObserved -and $clientPeerObserved -and
                $combatStarted -and $deathObserved -and
                ((Get-Date) - $deathObservedAt).TotalSeconds -ge $RaidTestStableSeconds) {
                [PSCustomObject]@{ status = 'passed'; scenario = $CombatScenario; hostPid = $hostPid; clientPid = $clientPid; hostPeerObserved = $hostPeerObserved; clientPeerObserved = $clientPeerObserved; replicaObservedAt = $replicaObservedAt; deathObservedAt = $deathObservedAt } |
                    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $script:RunRoot 'raid-combat-autotest.json') -Encoding UTF8
                Write-Host "Combat autotest passed; death transition remained stable for $RaidTestStableSeconds s"
                return
            }
            if ($CombatScenario -ne '') { Start-Sleep -Seconds 1; continue }
            if ($hostPeerObserved -and $clientPeerObserved -and $replicaObserved -and ((Get-Date) - $replicaObservedAt).TotalSeconds -ge $RaidTestStableSeconds) {
                [PSCustomObject]@{ status = 'passed'; hostPid = $hostPid; clientPid = $clientPid; hostPeerObserved = $hostPeerObserved; clientPeerObserved = $clientPeerObserved; replicaObservedAt = $replicaObservedAt } |
                    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $script:RunRoot 'raid-crash-smoke.json') -Encoding UTF8
                Write-Host "Raid crash smoke passed; replica remained stable for $RaidTestStableSeconds s"
                return
            }
            Start-Sleep -Seconds 1
        }
        if (-not $hostPeerObserved) { throw 'Timed out before host observed the connected client peer' }
        if (-not $clientPeerObserved) { throw 'Timed out before client observed the connected host peer' }
        if ($CombatScenario -eq '' -and -not $replicaObserved) { throw 'Timed out before remote NPC replica was observed' }
        if ($CombatScenario -ne '' -and -not $combatStarted) { throw "Timed out before combat autotest started ($CombatScenario)" }
        if ($CombatScenario -ne '' -and -not $deathObserved) { throw "Timed out before authoritative death ($CombatScenario)" }
        throw 'Timed out while waiting for the NPC stability window'
    }
    catch {
        $failure = $_
        [PSCustomObject]@{ status = 'failed'; scenario = $CombatScenario; error = $_.Exception.Message; hostPid = $hostPid; clientPid = $clientPid } |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $script:RunRoot $(if ($CombatScenario -eq '') { 'raid-crash-smoke.json' } else { 'raid-combat-autotest.json' })) -Encoding UTF8
        throw
    }
    finally {
        Stop-LocalGame
        Stop-RemoteGame
        Remove-RemoteRaidTestTask
        Write-RaidTestDiagnostics
        if ($failure) { Write-Host "Raid crash smoke diagnostics: $script:RunRoot" }
    }
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
    'RaidCrashSmoke' { Stop-LocalGame; Stop-RemoteGame; New-Overlay; Test-LocalTarget; Test-RemoteTarget; Deploy-LocalOverlay; Deploy-RemoteOverlay; Invoke-RaidCrashSmoke }
    'CombatHostKillsClient' { Stop-LocalGame; Stop-RemoteGame; New-Overlay; Test-LocalTarget; Test-RemoteTarget; Deploy-LocalOverlay; Deploy-RemoteOverlay; Invoke-RaidCrashSmoke 'host-kills-client' }
    'CombatClientKillsHost' { Stop-LocalGame; Stop-RemoteGame; New-Overlay; Test-LocalTarget; Test-RemoteTarget; Deploy-LocalOverlay; Deploy-RemoteOverlay; Invoke-RaidCrashSmoke 'client-kills-host' }
    'Collect' { Collect-Logs }
    'All' { Stop-LocalGame; Stop-RemoteGame; New-ComModPackage; New-Overlay; Test-LocalTarget; Test-RemoteTarget; Deploy-LocalOverlay; Deploy-RemoteOverlay; Invoke-Smoke; Collect-Logs }
}

Write-Host "Artifacts: $script:RunRoot"
