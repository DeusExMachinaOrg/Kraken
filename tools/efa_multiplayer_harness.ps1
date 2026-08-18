[CmdletBinding()]
param(
    [ValidateSet('Package', 'Preflight', 'Deploy', 'Headless', 'Unit', 'Smoke',
                 'RaidCrashSmoke', 'JipSmoke', 'CombatHostKillsClient',
                 'CombatClientKillsHost', 'Collect', 'SelfCheck', 'All')]
    [string]$Action = 'SelfCheck',
    [string]$LocalGameRoot = 'E:\HTA_EFA',
    [string]$RemoteSsh = 'etozh@192.168.2.80',
    [string]$RemoteGameRoot = 'E:\HTA_EFA',
    [string]$KrakenBuildRoot = $env:KRAKEN_MP_BUILD_ROOT,
    [int]$SmokePort = 27830,
    [ValidateRange(5, 120)][int]$SmokeTimeoutSeconds = 25,
    [ValidateRange(30, 600)][int]$RaidTestTimeoutSeconds = 180,
    [ValidateRange(5, 120)][int]$RaidTestStableSeconds = 20,
    [ValidateRange(5, 300)][int]$JipMutationTimeoutSeconds = 90,
    [ValidateRange(5, 60)][int]$RemoteCommandTimeoutSeconds = 10,
    [string]$CTestPath = 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe',
    [string]$RaidTargetMap = 'r1m1',
    [string]$RaidExitMap = '',
    [string]$HostCombatWeaponPart = $env:KRAKEN_EFA_HOST_COMBAT_WEAPON_PART,
    [string]$ClientCombatWeaponPart = $env:KRAKEN_EFA_CLIENT_COMBAT_WEAPON_PART
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Root = Split-Path -Parent $PSScriptRoot
$script:RunId = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$script:RunRoot = Join-Path $script:Root "artifacts\efa-multiplayer-harness\$script:RunId"
$script:OverlayRoot = Join-Path $script:RunRoot 'overlay'
$script:RemoteStage = "C:\Users\Public\kraken-mp-$script:RunId.zip"
$script:RemoteScpStage = "C:/Users/Public/kraken-mp-$script:RunId.zip"
$script:RemoteRaidTaskName = $null
$script:CombatKillTimeoutSeconds = 30
$script:PollMilliseconds = 100
$script:OverlayFiles = @(
    [PSCustomObject]@{ Source = 'kraken.dll'; Target = 'kraken.dll' },
    [PSCustomObject]@{ Source = 'kraken_net_peer_test.exe'; Target = 'kraken_net_peer_test.exe' }
)
$script:RuntimeMarkers = [ordered]@{
    NativeSaved = 'KRAKEN_MP_ACCEPT native_saved_game'
    Matchmaking = 'KRAKEN_MP_ACCEPT matchmaking'
    Snapshot = 'KRAKEN_MP_ACCEPT snapshot_committed'
    Quest = 'KRAKEN_MP_ACCEPT quest_committed'
    WorldReady = 'KRAKEN_MP_ACCEPT world_ready'
    GameplayOpen = 'KRAKEN_MP_ACCEPT gameplay_open'
    FirstInput = 'KRAKEN_MP_ACCEPT first_input'
    HostControlReady = 'KRAKEN_MP_ACCEPT host_control_ready'
    CombatArmed = 'KRAKEN_MP_ACCEPT combat_armed'
    SessionExit = 'KRAKEN_MP_ACCEPT session_exit'
    NativeEntity = 'KRAKEN_MP_ACCEPT native_entity_registered'
    Replica = 'KRAKEN_MP_ACCEPT replica_materialized'
    CombatDeath = 'KRAKEN_COMBAT_AUTOTEST death scenario='
    HostSurvival = 'KRAKEN_MP_ACCEPT combat_host_surviving'
}

function Assert-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
}

function Assert-BuildRoot {
    if ([string]::IsNullOrWhiteSpace($KrakenBuildRoot)) {
        throw 'Pass -KrakenBuildRoot from the unique MCP CMake build (or set KRAKEN_MP_BUILD_ROOT)'
    }
    $resolved = (Resolve-Path -LiteralPath $KrakenBuildRoot).Path
    if ((Split-Path -Leaf $resolved) -in @('build', 'build-ninja')) {
        throw "Shared build roots are rejected; use a unique MCP build directory: $resolved"
    }
    foreach ($file in $script:OverlayFiles) { Assert-File (Join-Path $resolved $file.Source) }
    $script:ResolvedBuildRoot = $resolved
}

function ConvertTo-EncodedPowerShell([string]$Text) {
    [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Text))
}

function Invoke-SshBounded([string]$RemoteCommand) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = 'ssh.exe'; $info.UseShellExecute = $false
    $info.CreateNoWindow = $true; $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.Arguments = "-o BatchMode=yes -o ConnectTimeout=10 $RemoteSsh $RemoteCommand"
    $process = [Diagnostics.Process]::Start($info)
    if ($null -eq $process) { throw 'Could not start ssh.exe' }
    try {
        $outTask = $process.StandardOutput.ReadToEndAsync()
        $errTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($RemoteCommandTimeoutSeconds * 1000)) {
            $process.Kill(); $process.WaitForExit()
            throw "Remote command exceeded $RemoteCommandTimeoutSeconds seconds"
        }
        $stdout = $outTask.GetAwaiter().GetResult()
        $stderr = $errTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "Remote command failed ($($process.ExitCode)): $stdout$stderr" }
        @($stdout -split "`r?`n" | Where-Object { $_ -ne '' })
    } finally { $process.Dispose() }
}

function Invoke-RemotePowerShell([string]$Text) {
    $encoded = ConvertTo-EncodedPowerShell $Text
    if ($encoded.Length -gt 24000) { throw 'Remote command exceeds bounded encoded-command size' }
    Invoke-SshBounded "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded"
}

function Start-RemotePowerShellStream([string]$Text) {
    $encoded = ConvertTo-EncodedPowerShell $Text
    if ($encoded.Length -gt 24000) { throw 'Remote stream command exceeds bounded encoded-command size' }
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = 'ssh.exe'; $info.UseShellExecute = $false
    $info.CreateNoWindow = $true; $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.Arguments = "-o BatchMode=yes -o ConnectTimeout=10 $RemoteSsh powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded"
    $out = [Collections.Concurrent.ConcurrentQueue[string]]::new()
    $err = [Collections.Concurrent.ConcurrentQueue[string]]::new()
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $info
    $onOut = { param($s,$e) if ($null -ne $e.Data) { $out.Enqueue([string]$e.Data) } }.GetNewClosure()
    $onErr = { param($s,$e) if ($null -ne $e.Data) { $err.Enqueue([string]$e.Data) } }.GetNewClosure()
    $process.add_OutputDataReceived($onOut); $process.add_ErrorDataReceived($onErr)
    if (-not $process.Start()) { throw 'Could not start persistent remote stream' }
    $process.BeginOutputReadLine(); $process.BeginErrorReadLine()
    [PSCustomObject]@{ Process=$process; Output=$out; Error=$err }
}

function Copy-ToRemote([string]$Source, [string]$Destination) {
    & scp -q $Source "$RemoteSsh`:$Destination"
    if ($LASTEXITCODE -ne 0) { throw "SCP failed: $Source -> $Destination" }
}

function Get-EfaContractFiles {
    @('hta.exe','data\scripts\efa.lua','data\scripts\efa_multiplayer.lua',
      'data\scripts\server.lua','data\maps\r0m0\cinematriggers.xml')
}

function Get-LocalEfaContract {
    $result = [ordered]@{}
    foreach ($relative in Get-EfaContractFiles) {
        $path = Join-Path $LocalGameRoot $relative; Assert-File $path
        $result[$relative] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    $result
}

function Test-Targets {
    $local = Get-LocalEfaContract
    $filesJson = @($local.Keys) | ConvertTo-Json -Compress
    $remoteScript = @"
`$root='$($RemoteGameRoot.Replace("'","''"))'; `$files='$($filesJson.Replace("'","''"))' | ConvertFrom-Json
`$result=[ordered]@{}; foreach (`$relative in `$files) { `$path=Join-Path `$root `$relative; if (-not (Test-Path -LiteralPath `$path -PathType Leaf)) { throw "Missing remote contract file: `$relative" }; `$result[`$relative]=(Get-FileHash -LiteralPath `$path -Algorithm SHA256).Hash }; `$result | ConvertTo-Json -Compress
"@
    $remote = (Invoke-RemotePowerShell $remoteScript | Select-Object -Last 1) | ConvertFrom-Json
    foreach ($relative in $local.Keys) {
        if ($local[$relative] -ne [string]$remote.$relative) {
            throw "EFA contract differs between peers: $relative"
        }
    }
    Write-Host 'EFA/mod contract hashes match; no EFA resource will be written'
}

function Test-ArtifactHashesMatch([string]$Expected,[string]$Local,[string]$Remote) {
    -not[string]::IsNullOrWhiteSpace($Expected) -and
        $Expected.Equals($Local,[StringComparison]::OrdinalIgnoreCase) -and
        $Expected.Equals($Remote,[StringComparison]::OrdinalIgnoreCase)
}

function Assert-DeployedKrakenArtifact {
    Assert-BuildRoot
    $source=Join-Path $script:ResolvedBuildRoot 'kraken.dll';$local=Join-Path $LocalGameRoot 'kraken.dll'
    Assert-File $source;Assert-File $local
    $expected=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $localHash=(Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash
    $remoteHash=[string](Invoke-RemotePowerShell "`$p=Join-Path '$($RemoteGameRoot.Replace("'","''"))' 'kraken.dll';if(-not(Test-Path -LiteralPath `$p -PathType Leaf)){throw 'Remote kraken.dll is missing'};(Get-FileHash -LiteralPath `$p -Algorithm SHA256).Hash"|Select-Object -Last 1)
    if(-not(Test-ArtifactHashesMatch $expected $localHash $remoteHash.Trim())){throw "Stale Kraken deployment: build=$expected local=$localHash remote=$($remoteHash.Trim()). Run -Action Deploy first."}
    Write-Host "Deployed kraken.dll matches build artifact: $expected"
}

function Get-FirstEquippedWeaponPart([string]$XmlText) {
    try{$document=[xml]$XmlText}catch{throw "Malformed currentmap.xml: $($_.Exception.Message)"}
    $groups=@($document.SelectNodes('//WeaponGroupManager/CurrentWeaponGroups/WeaponGroup'))
    if(-not$groups.Count){throw 'currentmap.xml has no WeaponGroupManager/CurrentWeaponGroups entries'}
    foreach($group in $groups){
        $parts=[string]$group.weaponParts
        foreach($token in @($parts-split'[\s,;]+'|Where-Object{$_-ne''})){return [string]$token}
    }
    throw 'currentmap.xml has no equipped weaponParts token'
}

function Find-NewestSaveCurrentMap([string]$GameRoot) {
    $profiles=Join-Path $GameRoot 'data\profiles';if(-not(Test-Path -LiteralPath $profiles -PathType Container)){throw "Profiles directory is missing: $profiles"}
    $selected=$null
    foreach($profile in @(Get-ChildItem -LiteralPath $profiles -Directory -ErrorAction Stop)){
        foreach($save in @(Get-ChildItem -LiteralPath (Join-Path $profile.FullName 'saves') -Directory -ErrorAction SilentlyContinue)){
            $xml=Join-Path $save.FullName 'maps\currentmap.xml'
            if((Test-Path -LiteralPath $xml -PathType Leaf)-and($null-eq$selected-or$save.LastWriteTimeUtc-gt$selected.LastWriteTimeUtc)){$selected=[PSCustomObject]@{XmlPath=$xml;LastWriteTimeUtc=$save.LastWriteTimeUtc}}
        }
    }
    if($null-eq$selected){throw "No data/profiles/*/saves/*/maps/currentmap.xml under $GameRoot"}
    [string]$selected.XmlPath
}

function Get-LocalCombatWeaponPart {
    $xmlPath=Find-NewestSaveCurrentMap $LocalGameRoot
    Get-FirstEquippedWeaponPart ([IO.File]::ReadAllText($xmlPath))
}

function Get-RemoteCombatWeaponPart {
    $remote=@"
`$root='$($RemoteGameRoot.Replace("'","''"))';`$profiles=Join-Path `$root 'data\profiles';if(-not(Test-Path -LiteralPath `$profiles -PathType Container)){throw "Profiles directory is missing: `$profiles"}
`$selected=`$null;foreach(`$profile in @(Get-ChildItem -LiteralPath `$profiles -Directory -ErrorAction Stop)){foreach(`$save in @(Get-ChildItem -LiteralPath (Join-Path `$profile.FullName 'saves') -Directory -ErrorAction SilentlyContinue)){`$xml=Join-Path `$save.FullName 'maps\currentmap.xml';if((Test-Path -LiteralPath `$xml -PathType Leaf)-and(`$null-eq`$selected-or`$save.LastWriteTimeUtc-gt`$selected.LastWriteTimeUtc)){`$selected=[PSCustomObject]@{XmlPath=`$xml;LastWriteTimeUtc=`$save.LastWriteTimeUtc}}}};if(`$null-eq`$selected){throw "No data/profiles/*/saves/*/maps/currentmap.xml under `$root"}
`$xmlPath=[string]`$selected.XmlPath;try{`$document=[xml][IO.File]::ReadAllText(`$xmlPath)}catch{throw "Malformed currentmap.xml: `$(`$_.Exception.Message)"};`$groups=@(`$document.SelectNodes('//WeaponGroupManager/CurrentWeaponGroups/WeaponGroup'));if(-not`$groups.Count){throw 'currentmap.xml has no WeaponGroupManager/CurrentWeaponGroups entries'};foreach(`$group in `$groups){foreach(`$token in @(([string]`$group.weaponParts)-split'[\s,;]+'|Where-Object{`$_-ne''})){Write-Output ([string]`$token);exit 0}};throw 'currentmap.xml has no equipped weaponParts token'
"@
    $part=[string](Invoke-RemotePowerShell $remote|Select-Object -Last 1);if([string]::IsNullOrWhiteSpace($part)){throw 'Remote newest save returned no equipped weapon part'};$part.Trim()
}

function Resolve-CombatWeaponPart([ValidateSet('host-kills-client','client-kills-host')][string]$Combat) {
    $override=if($Combat-eq'host-kills-client'){$HostCombatWeaponPart}else{$ClientCombatWeaponPart}
    if(-not[string]::IsNullOrWhiteSpace($override)){return $override.Trim()}
    if($Combat-eq'host-kills-client'){Get-LocalCombatWeaponPart}else{Get-RemoteCombatWeaponPart}
}

function Stop-LocalGame {
    Get-Process -Name hta -ErrorAction SilentlyContinue | Stop-Process -Force
    $deadline=(Get-Date).AddSeconds(10)
    while (Get-Process -Name hta -ErrorAction SilentlyContinue) {
        if ((Get-Date) -ge $deadline) { throw 'Local hta.exe did not stop' }
        Start-Sleep -Milliseconds $script:PollMilliseconds
    }
}

function Stop-RemoteGame {
    $remote = @"
Get-Process -Name hta -ErrorAction SilentlyContinue | Stop-Process -Force
`$deadline=(Get-Date).AddSeconds(10); while (Get-Process -Name hta -ErrorAction SilentlyContinue) { if ((Get-Date) -ge `$deadline) { throw 'Remote hta.exe did not stop' }; Start-Sleep -Milliseconds 100 }
"@
    Invoke-RemotePowerShell $remote | Out-Null
}

function Remove-RemoteTask {
    if([string]::IsNullOrWhiteSpace($script:RemoteRaidTaskName)){return}
    $task=$script:RemoteRaidTaskName.Replace("'","''");$script:RemoteRaidTaskName=$null
    try{Invoke-RemotePowerShell "schtasks.exe /Delete /TN '$task' /F 2>`$null|Out-Null"|Out-Null}catch{}
}

function New-Overlay {
    Assert-BuildRoot
    if (Test-Path -LiteralPath $script:OverlayRoot) { Remove-Item -LiteralPath $script:OverlayRoot -Recurse -Force }
    New-Item -ItemType Directory -Path $script:OverlayRoot -Force | Out-Null
    $manifest = foreach ($file in $script:OverlayFiles) {
        $source=Join-Path $script:ResolvedBuildRoot $file.Source
        $target=Join-Path $script:OverlayRoot $file.Target
        Copy-Item -LiteralPath $source -Destination $target -Force
        [PSCustomObject]@{ path=$file.Target; sha256=(Get-FileHash $target -Algorithm SHA256).Hash; bytes=(Get-Item $target).Length }
    }
    $manifest | ConvertTo-Json | Set-Content (Join-Path $script:OverlayRoot 'kraken-manifest.json') -Encoding UTF8
    $archive=Join-Path $script:RunRoot 'kraken-multiplayer.zip'
    Compress-Archive -Path (Join-Path $script:OverlayRoot '*') -DestinationPath $archive -Force
    $archive
}

function Deploy-Overlay {
    Stop-LocalGame; Stop-RemoteGame; Test-Targets
    $archive=New-Overlay
    foreach ($file in $script:OverlayFiles) {
        Copy-Item -LiteralPath (Join-Path $script:OverlayRoot $file.Source) -Destination (Join-Path $LocalGameRoot $file.Target) -Force
    }
    Copy-ToRemote $archive $script:RemoteScpStage
    $remote = @"
`$root='$($RemoteGameRoot.Replace("'","''"))'; `$archive='$($script:RemoteStage.Replace("'","''"))'
Get-Process -Name hta -ErrorAction SilentlyContinue | Stop-Process -Force
Expand-Archive -LiteralPath `$archive -DestinationPath `$root -Force; Remove-Item -LiteralPath `$archive -Force
"@
    Invoke-RemotePowerShell $remote | Out-Null
    Write-Host 'Deployed Kraken-owned binaries only'
}

function Invoke-Headless {
    Assert-BuildRoot
    $exe=Join-Path $script:ResolvedBuildRoot 'kraken_net_64_participant_test.exe'; Assert-File $exe
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "64-participant test failed: $LASTEXITCODE" }
}

function Invoke-Unit {
    Assert-BuildRoot
    Assert-File $CTestPath
    & $CTestPath --test-dir $script:ResolvedBuildRoot -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed: $LASTEXITCODE" }
}

function Get-LocalLanAddress {
    $route=Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction Stop | Sort-Object RouteMetric | Select-Object -First 1
    (Get-NetIPAddress -InterfaceIndex $route.InterfaceIndex -AddressFamily IPv4 -ErrorAction Stop | Where-Object { $_.IPAddress -notlike '169.254.*' } | Select-Object -First 1).IPAddress
}

function Invoke-Smoke {
    Assert-BuildRoot
    $peer=Join-Path $script:ResolvedBuildRoot 'kraken_net_peer_test.exe'; Assert-File $peer
    $localIp=Get-LocalLanAddress; $hostOut=Join-Path $script:RunRoot 'peer-host.out'; $hostErr="$hostOut.err"
    $remote=@"
& '$($RemoteGameRoot.Replace("'","''"))\kraken_net_peer_test.exe' client $SmokePort $localIp --scripted-snapshots --scripted-input --scripted-weapon --scripted-despawn
exit `$LASTEXITCODE
"@
    $hostProcess=Start-Process -FilePath $peer -ArgumentList @('host',"$SmokePort",$localIp,'--scripted-snapshots','--scripted-input','--scripted-weapon','--scripted-despawn') -WindowStyle Hidden -RedirectStandardOutput $hostOut -RedirectStandardError $hostErr -PassThru
    $client=Start-RemotePowerShellStream $remote
    try {
        $deadline=(Get-Date).AddSeconds($SmokeTimeoutSeconds)
        while ((Get-Date) -lt $deadline -and -not $client.Process.HasExited) {
            if ($hostProcess.HasExited -and $hostProcess.ExitCode -ne 0) { throw "Peer host failed: $($hostProcess.ExitCode)" }
            Start-Sleep -Milliseconds $script:PollMilliseconds
        }
        if (-not $client.Process.HasExited) { throw 'Peer smoke timed out' }
        if ($client.Process.ExitCode -ne 0) { throw "Peer client failed: $($client.Process.ExitCode)" }
        Write-Host 'LAN peer smoke passed'
    } finally {
        if (-not $hostProcess.HasExited) { $hostProcess.Kill() }; if (-not $client.Process.HasExited) { $client.Process.Kill() }
        $hostProcess.Dispose(); $client.Process.Dispose()
    }
}

function Get-ExceptionInventory([string]$Root) {
    $dir=Join-Path $Root 'exceptions'; if (-not (Test-Path $dir)) { return @() }
    @(Get-ChildItem $dir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^hta\.exe\d+\.(dmp|log|game\.log)$' } | ForEach-Object { "$($_.Name)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" })
}

function Read-Delta([string]$Path,[int64]$Offset) {
    if (-not (Test-Path -LiteralPath $Path)) { return [PSCustomObject]@{Text='';Offset=[int64]0} }
    $length=[int64](Get-Item $Path).Length; $start=if($length -lt $Offset){[int64]0}else{$Offset}
    if($length -eq $start){return [PSCustomObject]@{Text='';Offset=$length}}
    $stream=[IO.File]::Open($Path,'Open','Read','ReadWrite')
    try{$stream.Seek($start,'Begin')|Out-Null;$reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8,$true);try{$text=$reader.ReadToEnd()}finally{$reader.Dispose()}}finally{$stream.Dispose()}
    [PSCustomObject]@{Text=$text;Offset=$length}
}

function New-FatalParser([string]$Name) { @{Name=$Name;Tail=''} }

function Update-FatalParser([hashtable]$Parser,[string]$Chunk) {
    if ([string]::IsNullOrEmpty($Chunk)) { return @() }
    $combined=[string]$Parser.Tail+$Chunk; $parts=@($combined -split "`n",-1)
    $Parser.Tail=if($combined.EndsWith("`n")){''}else{[string]$parts[-1]}
    $complete=if($combined.EndsWith("`n")){$parts}else{$parts[0..([Math]::Max(0,$parts.Count-2))]}
    $pattern='(?i)\bPANIC\b|\bassert(?:ion)?(?:\s+(?:failed|failure|error))?\b|\bfatal(?:\s+error)?\b|\bunhandled\s+exception\b|EXCEPTION_(?:ACCESS_VIOLATION|STACK_OVERFLOW)|pure virtual function'
    $hits=@($complete | Where-Object { $_ -match $pattern })
    if ([string]$Parser.Tail -match $pattern) { $hits += [string]$Parser.Tail }
    @($hits | ForEach-Object { "$($Parser.Name): $($_.Trim())" } | Select-Object -Unique)
}

function Start-LocalGame([hashtable]$Environment) {
    $exe=Join-Path $LocalGameRoot 'hta.exe'; Assert-File $exe
    $out=Join-Path $script:RunRoot 'local-hta.stdout.log'; $err=Join-Path $script:RunRoot 'local-hta.stderr.log'
    New-Item -ItemType File -Path $out -Force|Out-Null;New-Item -ItemType File -Path $err -Force|Out-Null
    $launcher=Join-Path $script:RunRoot 'local-hta-launch.cmd';$lines=[Collections.Generic.List[string]]::new();$lines.Add('@echo off');$lines.Add('setlocal')
    foreach($entry in $Environment.GetEnumerator()){
        $name=[string]$entry.Key;$value=[string]$entry.Value
        if($name-notmatch'^[A-Za-z_][A-Za-z0-9_]*$' -or $value.Contains("`r") -or $value.Contains("`n")){throw "Invalid local launch environment entry: $name"}
        $lines.Add(('set "'+$name+'='+$value.Replace('%','%%')+'"'))
    }
    $lines.Add(('cd /d "'+$LocalGameRoot+'"'));$lines.Add(('"'+$exe+'" 1>>"'+$out+'" 2>>"'+$err+'"'))
    [IO.File]::WriteAllLines($launcher,$lines,[Text.Encoding]::ASCII)
    $commandProcessor=[Environment]::GetEnvironmentVariable('ComSpec');Assert-File $commandProcessor
    $process=Start-Process -FilePath $commandProcessor -ArgumentList @('/d','/c',('"'+$launcher+'"')) -WorkingDirectory $LocalGameRoot -WindowStyle Hidden -PassThru
    [PSCustomObject]@{Process=$process;OutPath=$out;ErrPath=$err;LauncherPath=$launcher}
}

function Start-RemoteGame([hashtable]$Environment,[hashtable]$Supervisor,[scriptblock]$OnSupervisorChunk) {
    $json=($Environment|ConvertTo-Json -Compress).Replace("'","''")
    $remote=@"
`$root='$($RemoteGameRoot.Replace("'","''"))';`$exe=Join-Path `$root 'hta.exe';`$envs='$json'|ConvertFrom-Json
`$active=@(query user 2>`$null|ForEach-Object{if(`$_ -match '^\s*>?\s*(\S+)\s+(?:console|rdp-tcp#\d+)\s+(\d+)\s+Active\b'){[PSCustomObject]@{User=`$matches[1];Id=[int]`$matches[2]}}})|Select-Object -First 1;if(`$null -eq `$active){throw 'No active remote desktop session'}
`$task='\Kraken-MP-$script:RunId';`$cmd=Join-Path `$env:TEMP 'kraken-mp-$script:RunId.cmd';`$out=Join-Path `$root 'kraken-harness-$script:RunId.stdout.log';`$err=Join-Path `$root 'kraken-harness-$script:RunId.stderr.log'
`$lines=[Collections.Generic.List[string]]::new();`$lines.Add('@echo off');foreach(`$entry in `$envs.PSObject.Properties){`$lines.Add(('set "'+`$entry.Name+'='+[string]`$entry.Value+'"'))};`$lines.Add(('cd /d "'+`$root+'"'));`$lines.Add(('"'+`$exe+'" 1>>"'+`$out+'" 2>>"'+`$err+'"'));[IO.File]::WriteAllLines(`$cmd,`$lines,[Text.Encoding]::ASCII)
`$after=[DateTime]::UtcNow.AddSeconds(-1);`$at=(Get-Date).AddMinutes(1).ToString('HH:mm');schtasks /Create /TN `$task /SC ONCE /ST `$at /TR ('cmd.exe /d /c "'+`$cmd+'"') /RU `$active.User /IT /F|Out-Null;schtasks /Run /TN `$task|Out-Null
`$deadline=(Get-Date).AddSeconds(20);do{`$p=@(Get-Process hta -ErrorAction SilentlyContinue|Where-Object{`$_.StartTime.ToUniversalTime()-ge `$after}|Sort-Object StartTime -Descending|Select-Object -First 1);if(`$p){break};Start-Sleep -Milliseconds 100}while((Get-Date)-lt `$deadline);if(-not `$p){throw 'Remote hta.exe launch timed out'}
[PSCustomObject]@{processId=`$p[0].Id;taskName=`$task;stdout=`$out;stderr=`$err}|ConvertTo-Json -Compress
"@
    $launcher=Start-RemotePowerShellStream $remote;$launch=$null;$deadline=(Get-Date).AddSeconds(25)
    try{
        while((Get-Date)-lt$deadline -and $null-eq$launch){
            if($Supervisor){$supervisorChunk=Update-Supervisor $Supervisor;if($OnSupervisorChunk){& $OnSupervisorChunk $supervisorChunk}}
            $line=$null;while($launcher.Output.TryDequeue([ref]$line)){if(-not[string]::IsNullOrWhiteSpace($line)){$launch=$line|ConvertFrom-Json}}
            $launcherError=$null;while($launcher.Error.TryDequeue([ref]$launcherError)){if(-not[string]::IsNullOrWhiteSpace($launcherError)){throw "Remote launcher stderr: $launcherError"}}
            if($launcher.Process.HasExited -and $null-eq$launch){throw 'Remote launcher exited without process metadata'}
            Start-Sleep -Milliseconds $script:PollMilliseconds
        }
        if($null-eq$launch){throw 'Remote graphical launcher timed out'}
    }finally{if(-not$launcher.Process.HasExited){$launcher.Process.Kill()};$launcher.Process.Dispose()}
    $script:RemoteRaidTaskName=[string]$launch.taskName; $launch
}

function Start-RemoteWatchdog([object]$Launch,[string[]]$Baseline,[int64]$LogOffset) {
    $baselineJson=($Baseline|ConvertTo-Json -Compress).Replace("'","''")
    $remote=@"
`$pidValue=$([int]$Launch.processId);`$root='$($RemoteGameRoot.Replace("'","''"))';`$log=Join-Path `$root 'kraken.log';`$stderr='$(([string]$Launch.stderr).Replace("'","''"))';`$logOffset=[int64]$LogOffset;`$errOffset=[int64]0;`$baseline='$baselineJson'|ConvertFrom-Json
function ReadChunk(`$path,[ref]`$offset){if(-not(Test-Path `$path)){return ''};`$length=[int64](Get-Item `$path).Length;`$start=if(`$length-lt `$offset.Value){0}else{`$offset.Value};if(`$length-eq `$start){return ''};`$s=[IO.File]::Open(`$path,'Open','Read','ReadWrite');try{`$s.Seek(`$start,'Begin')|Out-Null;`$r=[IO.StreamReader]::new(`$s,[Text.Encoding]::UTF8,`$true);try{`$t=`$r.ReadToEnd()}finally{`$r.Dispose()}}finally{`$s.Dispose()};`$offset.Value=`$length;return `$t}
while(`$true){`$inventory=@();`$dir=Join-Path `$root 'exceptions';if(Test-Path `$dir){`$inventory=@(Get-ChildItem `$dir -File -ErrorAction SilentlyContinue|Where-Object{`$_.Name-match'^hta\.exe\d+\.(dmp|log|game\.log)$'}|ForEach-Object{"`$(`$_.Name)|`$(`$_.Length)|`$(`$_.LastWriteTimeUtc.Ticks)"})};`$new=@(`$inventory|Where-Object{`$_ -notin `$baseline});`$lc=ReadChunk `$log ([ref]`$logOffset);`$ec=ReadChunk `$stderr ([ref]`$errOffset);`$alive=`$null-ne(Get-Process -Id `$pidValue -ErrorAction SilentlyContinue);[PSCustomObject]@{alive=`$alive;log=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(`$lc));stderr=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(`$ec));exceptions=`$new}|ConvertTo-Json -Compress;if(-not `$alive){break};Start-Sleep -Milliseconds 100}
"@
    Start-RemotePowerShellStream $remote
}

function New-Supervisor([object]$Local,[string[]]$LocalBaseline,[int64]$LocalLogOffset) {
    @{Local=$Local;LocalBaseline=$LocalBaseline;LocalLogOffset=$LocalLogOffset;LocalErrOffset=[int64]0;LocalLogParser=New-FatalParser 'local kraken.log';LocalErrParser=New-FatalParser 'local stderr';Remote=$null;RemoteWatch=$null;RemoteLogParser=New-FatalParser 'remote kraken.log';RemoteErrParser=New-FatalParser 'remote stderr'}
}

function Update-Supervisor([hashtable]$State) {
    $local=$State.Local.Process;$local.Refresh();if($local.HasExited){throw 'Local hta.exe exited while supervised'}
    $new=@(Get-ExceptionInventory $LocalGameRoot|Where-Object{$_ -notin $State.LocalBaseline});if($new){throw "Local exception inventory changed: $($new -join '; ')"}
    $log=Read-Delta (Join-Path $LocalGameRoot 'kraken.log') $State.LocalLogOffset;$State.LocalLogOffset=[int64]$log.Offset
    $fatal=@(Update-FatalParser $State.LocalLogParser $log.Text)
    $localErr=Read-Delta $State.Local.ErrPath $State.LocalErrOffset;$State.LocalErrOffset=[int64]$localErr.Offset
    $fatal+=@(Update-FatalParser $State.LocalErrParser $localErr.Text)
    if($fatal.Count){throw "Fatal supervisor: $($fatal -join '; ')"}
    $remoteLog='';$remoteErr=''
    if($State.RemoteWatch){
        $line=$null;while($State.RemoteWatch.Output.TryDequeue([ref]$line)){if([string]::IsNullOrWhiteSpace($line)){continue};$record=$line|ConvertFrom-Json;$remoteLog+=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$record.log));$remoteErr+=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$record.stderr));if(@($record.exceptions).Count){throw "Remote exception inventory changed: $(@($record.exceptions)-join '; ')"};if(-not [bool]$record.alive){throw 'Remote hta.exe exited while supervised'}}
        $streamError=$null;while($State.RemoteWatch.Error.TryDequeue([ref]$streamError)){if(-not[string]::IsNullOrWhiteSpace($streamError)){throw "Remote watchdog stderr: $streamError"}}
        if($State.RemoteWatch.Process.HasExited -and -not $State.RemoteWatch.Output.TryPeek([ref]$line)){throw 'Remote watchdog exited unexpectedly'}
        $fatal=@(Update-FatalParser $State.RemoteLogParser $remoteLog)+@(Update-FatalParser $State.RemoteErrParser $remoteErr);if($fatal.Count){throw "Fatal supervisor: $($fatal -join '; ')"}
    }
    [PSCustomObject]@{LocalLog=[string]$log.Text;RemoteLog=$remoteLog}
}

function New-AcceptanceGate([ValidateSet('Host','Client')][string]$Role) {
    $native="KRAKEN_MP_ACCEPT native_saved_game role=$($Role.ToLowerInvariant())"
    $sequence=if($Role-eq'Host'){@($native,$script:RuntimeMarkers.WorldReady,$script:RuntimeMarkers.GameplayOpen,$script:RuntimeMarkers.HostControlReady)}else{@($native,$script:RuntimeMarkers.Snapshot,$script:RuntimeMarkers.Quest,$script:RuntimeMarkers.WorldReady,$script:RuntimeMarkers.GameplayOpen,$script:RuntimeMarkers.FirstInput)}
    @{Role=$Role;Sequence=$sequence;Stage=0;Buffer='';CompletedAt=$null}
}

function Update-AcceptanceGate([hashtable]$Gate,[string]$Chunk) {
    $Gate.Buffer=[string]$Gate.Buffer+$Chunk
    while($Gate.Stage-lt$Gate.Sequence.Count){$expected=$Gate.Sequence[$Gate.Stage];$index=$Gate.Buffer.IndexOf($expected,[StringComparison]::Ordinal);if($index-lt0){break};for($later=$Gate.Stage+1;$later-lt$Gate.Sequence.Count;$later++){if($Gate.Buffer.IndexOf($Gate.Sequence[$later],[StringComparison]::Ordinal)-ge0 -and $Gate.Buffer.IndexOf($Gate.Sequence[$later],[StringComparison]::Ordinal)-lt$index){throw "$($Gate.Role) acceptance order violation"}};$Gate.Buffer=$Gate.Buffer.Substring($index+$expected.Length);$Gate.Stage++}
    if($Gate.Stage-eq$Gate.Sequence.Count -and $null-eq$Gate.CompletedAt){$Gate.CompletedAt=Get-Date}
    if($Gate.Buffer.Length-gt65536){$Gate.Buffer=$Gate.Buffer.Substring($Gate.Buffer.Length-65536)}
}

function New-HostAcceptanceContext {
    @{Gate=New-AcceptanceGate Host;Buffer=''}
}

function Add-HostAcceptanceChunk([hashtable]$Context,[string]$Chunk) {
    $Context.Buffer=[string]$Context.Buffer+$Chunk
    Update-AcceptanceGate $Context.Gate $Chunk
}

function Test-OrdinaryRaidAcceptance([bool]$ReplicaReady,[bool]$RevisionReady,[bool]$HostGateComplete,[bool]$ClientGateComplete) {
    $ReplicaReady -and $RevisionReady -and $HostGateComplete -and $ClientGateComplete
}

function New-CombatAcceptanceState([ValidateSet('host-kills-client','client-kills-host')][string]$Scenario) {
    @{Scenario=$Scenario;HostArmed=$false;ClientArmed=$false;ArmedAt=$null;Deadline=$null;DeathAt=$null;HostExit=$false;ClientExit=$false;LastHostState='';LastHeartbeatAt=$null;StableAt=$null}
}

function Update-CombatAcceptanceState([hashtable]$State,[string]$HostChunk,[string]$ClientChunk,[datetime]$ObservedAt,[int]$KillSeconds,[int]$StableSeconds,[string]$RequestedRoute) {
    $hadBoth=$State.HostArmed-and$State.ClientArmed;$deathWasKnown=$null-ne$State.DeathAt
    if($HostChunk.Contains('KRAKEN_MP_ACCEPT combat_armed role=host')){$State.HostArmed=$true}
    if($ClientChunk.Contains('KRAKEN_MP_ACCEPT combat_armed role=client')){$State.ClientArmed=$true}
    if(-not$hadBoth-and$State.HostArmed-and$State.ClientArmed){$State.ArmedAt=$ObservedAt;$State.Deadline=$ObservedAt.AddSeconds($KillSeconds)}

    $deathPattern='KRAKEN_COMBAT_AUTOTEST death scenario=(?<scenario>\S+) shooter=(?<shooter>\d+) target=(?<target>\d+)'
    $deaths=@([regex]::Matches($HostChunk+"`n"+$ClientChunk,$deathPattern))
    if($deaths.Count){
        if(-not$hadBoth){return [PSCustomObject]@{Failed=$true;Reason='Death was observed before both roles were armed in a prior poll';Accepted=$false}}
        if($null-eq$State.Deadline-or$ObservedAt-gt$State.Deadline){return [PSCustomObject]@{Failed=$true;Reason='Authoritative death was observed after the 30-second deadline';Accepted=$false}}
        foreach($record in $deaths){
            $scenario=[string]$record.Groups['scenario'].Value;$shooter=[uint32]$record.Groups['shooter'].Value;$target=[uint32]$record.Groups['target'].Value
            $identityOk=$scenario-eq$State.Scenario-and(($State.Scenario-eq'host-kills-client'-and$shooter-eq1-and$target-ne0-and$target-ne1)-or($State.Scenario-eq'client-kills-host'-and$shooter-ne0-and$shooter-ne1-and$target-eq1))
            if(-not$identityOk){return [PSCustomObject]@{Failed=$true;Reason="Authoritative death identity mismatch scenario=$scenario shooter=$shooter target=$target";Accepted=$false}}
        }
        if($null-eq$State.DeathAt){$State.DeathAt=$ObservedAt}
    }elseif($State.Deadline-and$null-eq$State.DeathAt-and$ObservedAt-gt$State.Deadline){
        return [PSCustomObject]@{Failed=$true;Reason='Authoritative death missed the exact 30-second deadline';Accepted=$false}
    }

    $exitPattern='KRAKEN_MP_ACCEPT session_exit role=(?<role>\w+) reason=(?<reason>\w+) route=(?<route>\S+) result=(?<result>\w+)'
    foreach($source in @([PSCustomObject]@{Role='host';Text=$HostChunk},[PSCustomObject]@{Role='client';Text=$ClientChunk})){
        foreach($exitLine in @($source.Text-split"`r?`n"|Where-Object{$_.Contains($script:RuntimeMarkers.SessionExit)})){
            $record=[regex]::Match($exitLine,$exitPattern);if(-not$record.Success){return [PSCustomObject]@{Failed=$true;Reason="Malformed session_exit marker from $($source.Role)";Accepted=$false}}
            $role=[string]$record.Groups['role'].Value;$reason=[string]$record.Groups['reason'].Value;$route=[string]$record.Groups['route'].Value;$result=[string]$record.Groups['result'].Value
            if($result-ne'success'){return [PSCustomObject]@{Failed=$true;Reason="Session exit failed role=$role reason=$reason route=$route";Accepted=$false}}
            if($role-ne$source.Role-or$route-ne$RequestedRoute-or$reason-ne'death'){return [PSCustomObject]@{Failed=$true;Reason="Unexpected session exit source=$($source.Role) role=$role reason=$reason route=$route";Accepted=$false}}
            if($State.Scenario-eq'host-kills-client'-and$role-eq'host'){return [PSCustomObject]@{Failed=$true;Reason='Host exited during host-kills-client';Accepted=$false}}
            if($role-eq'host'){$State.HostExit=$true}else{$State.ClientExit=$true}
        }
    }

    $states=@([regex]::Matches($HostChunk,'KRAKEN_MP_ACCEPT match state=(?<state>\w+)'))
    if($states.Count){$State.LastHostState=[string]$states[-1].Groups['state'].Value}
    if($State.Scenario-eq'host-kills-client'-and$State.DeathAt-and$State.LastHostState-and$State.LastHostState-ne'Playing'){
        return [PSCustomObject]@{Failed=$true;Reason="Host match state regressed to $($State.LastHostState) after client death";Accepted=$false}
    }
    if($deathWasKnown-and$HostChunk.Contains($script:RuntimeMarkers.HostSurvival)){$State.LastHeartbeatAt=$ObservedAt}

    $candidate=if($State.Scenario-eq'host-kills-client'){
        $State.DeathAt-and$State.ClientExit-and-not$State.HostExit-and$State.LastHostState-eq'Playing'-and$State.LastHeartbeatAt-and(($ObservedAt-$State.LastHeartbeatAt).TotalSeconds-le2)
    }else{$State.DeathAt-and$State.HostExit-and$State.ClientExit}
    if($candidate){if($null-eq$State.StableAt){$State.StableAt=$ObservedAt}}else{$State.StableAt=$null}
    $accepted=$State.StableAt-and(($ObservedAt-$State.StableAt).TotalSeconds-ge$StableSeconds)
    [PSCustomObject]@{Failed=$false;Reason='';Accepted=[bool]$accepted}
}

function Find-NativeEntity([string]$Text) {
    $m=[regex]::Match($Text,'KRAKEN_MP_ACCEPT native_entity_registered entity=(?<e>\d+) generation=(?<g>\d+) kind=2 barrierRevision=(?<r>\d+)')
    if(-not$m.Success){return $null};[PSCustomObject]@{Entity=[uint32]$m.Groups['e'].Value;Generation=[uint16]$m.Groups['g'].Value;Revision=[uint64]$m.Groups['r'].Value}
}

function Start-ScenarioEnvironment([string]$Role,[string]$Scenario,[int]$Required,[string]$Combat='',[string]$WeaponPart='') {
    $envs=@{KRAKEN_MP_ENABLED='1';KRAKEN_MP_AUTOSTART='0';KRAKEN_MP_AUTO_LAN='1';KRAKEN_MP_PORT=[string]$SmokePort;KRAKEN_MP_MAX_PEERS='64';KRAKEN_MP_SPAWN_TOGETHER='1';KRAKEN_EFA_RAID_AUTOTEST='1';KRAKEN_MP_ACCEPT_ROLE=$Role;KRAKEN_MP_ACCEPT_SCENARIO=$Scenario;KRAKEN_MP_MATCH_TARGET=$RaidTargetMap;KRAKEN_MP_MATCH_EXIT=$RaidExitMap;KRAKEN_MP_MATCH_REQUIRED=[string]$Required}
    if($Combat){
        if([string]::IsNullOrWhiteSpace($WeaponPart)){throw "Combat scenario $Combat has no equipped shooter weapon part"}
        $envs.KRAKEN_EFA_COMBAT_AUTOTEST=$Combat;$envs.KRAKEN_EFA_COMBAT_WEAPON_PART=$WeaponPart
    };$envs
}

function Invoke-RaidScenario([string]$Scenario,[string]$Combat='') {
    Stop-LocalGame;Stop-RemoteGame;Test-Targets;Assert-DeployedKrakenArtifact
    $combatWeaponPart=if($Combat){Resolve-CombatWeaponPart $Combat}else{''}
    $localBase=Get-ExceptionInventory $LocalGameRoot;$localOffset=(if(Test-Path(Join-Path $LocalGameRoot 'kraken.log')){(Get-Item(Join-Path $LocalGameRoot 'kraken.log')).Length}else{0})
    $remoteBase=@(Invoke-RemotePowerShell "`$d=Join-Path '$($RemoteGameRoot.Replace("'","''"))' 'exceptions';if(Test-Path `$d){Get-ChildItem `$d -File|Where-Object{`$_.Name-match'^hta\.exe\d+\.(dmp|log|game\.log)`$'}|ForEach-Object{\"`$(`$_.Name)|`$(`$_.Length)|`$(`$_.LastWriteTimeUtc.Ticks)\"}}")
    $remoteOffset=[int64](Invoke-RemotePowerShell "`$p=Join-Path '$($RemoteGameRoot.Replace("'","''"))' 'kraken.log';if(Test-Path `$p){(Get-Item `$p).Length}else{0}"|Select-Object -Last 1)
    $required=if($Scenario-eq'jip'){1}else{2};$local=Start-LocalGame (Start-ScenarioEnvironment 'host' $Scenario $required $Combat $combatWeaponPart);$super=New-Supervisor $local $localBase $localOffset
    $hostAcceptance=New-HostAcceptanceContext;$hostGate=$hostAcceptance.Gate;$clientGate=New-AcceptanceGate Client;$clientBuffer='';$native=$null;$remoteLaunch=$null
    $requestedRoute=if([string]::IsNullOrWhiteSpace($RaidExitMap)){'main_menu'}else{$RaidExitMap};$combatState=if($Combat){New-CombatAcceptanceState $Combat}else{$null}
    $consumeHostChunk={param($chunk)Add-HostAcceptanceChunk $hostAcceptance $chunk.LocalLog;if($combatState){$combatUpdate=Update-CombatAcceptanceState $combatState $chunk.LocalLog '' (Get-Date) $script:CombatKillTimeoutSeconds $RaidTestStableSeconds $requestedRoute;if($combatUpdate.Failed){throw $combatUpdate.Reason}}}.GetNewClosure()
    try{
        $hostDeadline=(Get-Date).AddSeconds($RaidTestTimeoutSeconds)
        while((Get-Date)-lt$hostDeadline){$d=Update-Supervisor $super;&$consumeHostChunk $d;if($hostAcceptance.Buffer-match'KRAKEN_MP_ACCEPT native_saved_game role=host .*result=rejected' -or $hostAcceptance.Buffer-match'KRAKEN_MP_ACCEPT matchmaking role=host .*result=rejected'){throw 'Host native save or generic matchmaking was rejected'};if($hostAcceptance.Buffer-match'KRAKEN_MP_ACCEPT native_saved_game role=host .*result=loaded' -and $hostAcceptance.Buffer-match'KRAKEN_MP_ACCEPT matchmaking role=host .*result=accepted'){break};Start-Sleep -Milliseconds $script:PollMilliseconds}
        if($Scenario-eq'jip'){
            while((Get-Date)-lt$hostDeadline -and -not$hostAcceptance.Buffer.Contains($script:RuntimeMarkers.GameplayOpen)){$d=Update-Supervisor $super;&$consumeHostChunk $d;Start-Sleep -Milliseconds $script:PollMilliseconds}
            if(-not$hostAcceptance.Buffer.Contains($script:RuntimeMarkers.GameplayOpen)){throw 'JIP host did not reach gameplay_open before mutation capture'}
            $jipMutationBuffer=''
            $mutationDeadline=(Get-Date).AddSeconds($JipMutationTimeoutSeconds)
            while((Get-Date)-lt$mutationDeadline -and $null-eq$native){$d=Update-Supervisor $super;&$consumeHostChunk $d;$jipMutationBuffer+=$d.LocalLog;$native=Find-NativeEntity $jipMutationBuffer;Start-Sleep -Milliseconds $script:PollMilliseconds}
            if($null-eq$native){throw 'No natural typed NPC registration occurred after Playing within the JIP mutation timeout'}
        }
        $remoteLaunch=Start-RemoteGame (Start-ScenarioEnvironment 'client' $Scenario $required $Combat $combatWeaponPart) $super $consumeHostChunk;$super.Remote=$remoteLaunch;$super.RemoteWatch=Start-RemoteWatchdog $remoteLaunch $remoteBase $remoteOffset
        $deadline=(Get-Date).AddSeconds($RaidTestTimeoutSeconds);$stableAt=$null
        while((Get-Date)-lt$deadline){$d=Update-Supervisor $super;Add-HostAcceptanceChunk $hostAcceptance $d.LocalLog;$clientBuffer+=$d.RemoteLog;Update-AcceptanceGate $clientGate $d.RemoteLog
            if($clientBuffer-match'KRAKEN_MP_ACCEPT native_saved_game role=client .*result=rejected' -or $clientBuffer-match'KRAKEN_MP_ACCEPT matchmaking role=client .*result=rejected'){throw 'Client native save or generic matchmaking was rejected'}
            if($null-eq$native){$native=Find-NativeEntity $hostAcceptance.Buffer}
            if($native){$replicaPattern="KRAKEN_MP_ACCEPT replica_materialized entity=$($native.Entity) generation=$($native.Generation) kind=2";$replicaOk=$clientBuffer.Contains($replicaPattern);$snap=[regex]::Matches($clientBuffer,'KRAKEN_MP_ACCEPT snapshot_committed epoch=\d+ revision=(?<r>\d+)')|Select-Object -Last 1;$revisionOk=$snap-and([uint64]$snap.Groups['r'].Value-ge$native.Revision)}else{$replicaOk=$false;$revisionOk=$false}
            if($Combat){$combatUpdate=Update-CombatAcceptanceState $combatState $d.LocalLog $d.RemoteLog (Get-Date) $script:CombatKillTimeoutSeconds $RaidTestStableSeconds $requestedRoute;if($combatUpdate.Failed){throw $combatUpdate.Reason};if($combatUpdate.Accepted){return}}
            elseif(Test-OrdinaryRaidAcceptance $replicaOk $revisionOk ($null-ne$hostGate.CompletedAt) ($null-ne$clientGate.CompletedAt)){if($null-eq$stableAt){$stableAt=Get-Date}}
            if(-not$Combat-and$stableAt-and((Get-Date)-$stableAt).TotalSeconds-ge$RaidTestStableSeconds){return}
            Start-Sleep -Milliseconds $script:PollMilliseconds
        }
        throw "Scenario timed out: hostGate=$($hostGate.Stage)/$($hostGate.Sequence.Count) clientGate=$($clientGate.Stage)/$($clientGate.Sequence.Count)"
    }finally{if($super.RemoteWatch-and-not$super.RemoteWatch.Process.HasExited){$super.RemoteWatch.Process.Kill()};if($local.Process-and-not$local.Process.HasExited){$local.Process.Kill()};Stop-LocalGame;Stop-RemoteGame;Remove-RemoteTask;Collect-Logs}
}

function Collect-Logs {
    New-Item -ItemType Directory -Path $script:RunRoot -Force|Out-Null
    $local=Join-Path $LocalGameRoot 'kraken.log';if(Test-Path $local){Get-Content $local -Tail 4000|Set-Content(Join-Path $script:RunRoot 'local-kraken.log')}
    Invoke-RemotePowerShell "`$p=Join-Path '$($RemoteGameRoot.Replace("'","''"))' 'kraken.log';if(Test-Path `$p){Get-Content `$p -Tail 4000}"|Set-Content(Join-Path $script:RunRoot 'remote-kraken.log')
    foreach($stream in @('stdout','stderr')){$remotePath="$RemoteGameRoot/kraken-harness-$script:RunId.$stream.log";try{& scp -q "$RemoteSsh`:$remotePath" (Join-Path $script:RunRoot "remote-hta.$stream.log")}catch{}}
}

function Invoke-SelfCheck {
    $tokens=$null;$errors=$null;[Management.Automation.Language.Parser]::ParseFile($PSCommandPath,[ref]$tokens,[ref]$errors)|Out-Null;if($errors.Count){throw "PowerShell parse failed: $($errors-join'; ')"}
    $source=Get-Content $PSCommandPath -Raw
    foreach($fragment in @('EFA_MP.'+'BeginRaid','FireFromWeapon'+'Custom2','SetPosition'+'Self(','data\scripts\efa.lua''; Target','triggers.xml''; Target')){if($source.Contains($fragment)){throw "Forbidden harness behavior: $fragment"}}
    foreach($fragment in @('cli'+'ck','Send'+'Keys','UI'+'Automation','keybd'+'_event','user'+'32.dll')){if($source.IndexOf($fragment,[StringComparison]::OrdinalIgnoreCase)-ge0){throw "Forbidden automation behavior: $fragment"}}
    if(@($script:OverlayFiles|Where-Object{$_.Target-like'data\*'}).Count){throw 'Deployment contains non-Kraken resources'}
    $parser=New-FatalParser 'self';if(@(Update-FatalParser $parser 'PAN').Count){throw 'Fatal parser completed an incomplete token too early'};$hits=@(Update-FatalParser $parser "IC crash`r`n");if($hits.Count-ne1){throw 'Fatal parser did not preserve and detect a split line'}
    $tempErr=Join-Path ([IO.Path]::GetTempPath()) ("kraken-harness-stderr-$([Guid]::NewGuid().ToString('N')).log");$utf8NoBom=[Text.UTF8Encoding]::new($false)
    try{
        [IO.File]::WriteAllText($tempErr,'PAN',$utf8NoBom);$first=Read-Delta $tempErr 0;$fileParser=New-FatalParser 'file stderr'
        if($first.Text-ne'PAN' -or $first.Offset-ne3 -or @(Update-FatalParser $fileParser $first.Text).Count){throw 'Read-Delta did not preserve the first incomplete stderr fragment'}
        [IO.File]::AppendAllText($tempErr,'IC crash',$utf8NoBom);$second=Read-Delta $tempErr $first.Offset;$fileHits=@(Update-FatalParser $fileParser $second.Text)
        if($second.Text-ne'IC crash' -or $second.Offset-ne11 -or $fileHits.Count-ne1){throw 'Read-Delta/fatal parser did not detect split non-newline stderr across offsets'}
        $empty=Read-Delta $tempErr $second.Offset;if($empty.Text-ne'' -or $empty.Offset-ne$second.Offset){throw 'Read-Delta replayed already-consumed stderr'}
    }finally{if(Test-Path -LiteralPath $tempErr){Remove-Item -LiteralPath $tempErr -Force}}
    $client=New-AcceptanceGate Client;Update-AcceptanceGate $client (($client.Sequence-join"`r`n")+"`r`n");if($null-eq$client.CompletedAt){throw 'Client gate did not complete'}
    $hostSelfGate=New-AcceptanceGate Host;Update-AcceptanceGate $hostSelfGate (($hostSelfGate.Sequence-join"`r`n")+"`r`n");if($null-eq$hostSelfGate.CompletedAt){throw 'Host gate did not complete'}
    $hostContext=New-HostAcceptanceContext;Add-HostAcceptanceChunk $hostContext ($hostContext.Gate.Sequence[0]+"`r`n");if($hostContext.Gate.Stage-ne1){throw 'Pre-remote-launch host marker was not consumed'};Add-HostAcceptanceChunk $hostContext (($hostContext.Gate.Sequence[1..3]-join"`r`n")+"`r`n");if($null-eq$hostContext.Gate.CompletedAt-or-not$hostContext.Buffer.Contains($hostContext.Gate.Sequence[3])){throw 'Remote-launch-wait host marker did not advance the same gate'}
    if(Test-OrdinaryRaidAcceptance $true $true $true $false){throw 'Ordinary raid acceptance allowed an incomplete client gate'}
    if(Test-OrdinaryRaidAcceptance $true $true $false $true){throw 'Ordinary raid acceptance allowed an incomplete host gate'}
    if(-not(Test-OrdinaryRaidAcceptance $true $true $true $true)){throw 'Ordinary raid acceptance rejected two completed gates'}
    if(-not(Test-ArtifactHashesMatch ('A'*64) ('a'*64) ('A'*64))-or(Test-ArtifactHashesMatch ('A'*64) ('B'*64) ('A'*64))){throw 'Artifact provenance hash predicate accepted stale deployment or rejected an exact hash'}
    $weaponXml="<Root><WeaponGroupManager><CurrentWeaponGroups><WeaponGroup groupId='0' weaponParts='' /><WeaponGroup groupId='1' weaponParts='GUN_FIRST GUN_SECOND' /><WeaponGroup groupId='2' weaponParts='GUN_THIRD' /></CurrentWeaponGroups></WeaponGroupManager></Root>"
    if((Get-FirstEquippedWeaponPart $weaponXml)-ne'GUN_FIRST'){throw 'Weapon discovery did not select the first token from the first equipped group'}
    $malformedFailed=$false;try{Get-FirstEquippedWeaponPart '<Root><WeaponGroupManager>'|Out-Null}catch{$malformedFailed=$true};if(-not$malformedFailed){throw 'Weapon discovery accepted malformed currentmap.xml'}
    $emptyFailed=$false;try{Get-FirstEquippedWeaponPart "<Root><WeaponGroupManager><CurrentWeaponGroups><WeaponGroup weaponParts='' /></CurrentWeaponGroups></WeaponGroupManager></Root>"|Out-Null}catch{$emptyFailed=$true};if(-not$emptyFailed){throw 'Weapon discovery accepted a save with no equipped weapon'}
    $tempSaveRoot=Join-Path ([IO.Path]::GetTempPath()) ("kraken-harness-save-$([Guid]::NewGuid().ToString('N'))")
    try{
        $oldSave=Join-Path $tempSaveRoot 'data\profiles\pilot\saves\old';$newSave=Join-Path $tempSaveRoot 'data\profiles\pilot\saves\new'
        $oldMaps=Join-Path $oldSave 'maps';$newMaps=Join-Path $newSave 'maps';New-Item -ItemType Directory -Path $oldMaps,$newMaps -Force|Out-Null
        [IO.File]::WriteAllText((Join-Path $oldMaps 'currentmap.xml'),$weaponXml,$utf8NoBom)
        $newXml=$weaponXml.Replace('GUN_FIRST GUN_SECOND','NEWEST_GUN BACKUP_GUN');[IO.File]::WriteAllText((Join-Path $newMaps 'currentmap.xml'),$newXml,$utf8NoBom)
        (Get-Item -LiteralPath $oldSave).LastWriteTimeUtc=[datetime]'2026-01-01T00:00:00Z';(Get-Item -LiteralPath $newSave).LastWriteTimeUtc=[datetime]'2026-01-02T00:00:00Z'
        $selectedSave=Find-NewestSaveCurrentMap $tempSaveRoot
        if((Split-Path (Split-Path $selectedSave -Parent) -Parent)-ne$newSave-or(Get-FirstEquippedWeaponPart ([IO.File]::ReadAllText($selectedSave)))-ne'NEWEST_GUN'){throw 'Newest-save discovery did not match runtime save-directory ordering'}
    }finally{
        $tempBase=[IO.Path]::GetFullPath([IO.Path]::GetTempPath());$resolvedTemp=[IO.Path]::GetFullPath($tempSaveRoot)
        if($resolvedTemp.StartsWith($tempBase,[StringComparison]::OrdinalIgnoreCase)-and(Test-Path -LiteralPath $resolvedTemp)){Remove-Item -LiteralPath $resolvedTemp -Recurse -Force}
    }

    $t0=[datetime]'2026-01-01T00:00:00Z';$route='main_menu'
    $before=New-CombatAcceptanceState 'host-kills-client';$result=Update-CombatAcceptanceState $before 'KRAKEN_COMBAT_AUTOTEST death scenario=host-kills-client shooter=1 target=2' '' $t0 30 1 $route;if(-not$result.Failed){throw 'Combat state allowed death before arming'}
    $same=New-CombatAcceptanceState 'host-kills-client';$result=Update-CombatAcceptanceState $same "KRAKEN_MP_ACCEPT combat_armed role=host`nKRAKEN_COMBAT_AUTOTEST death scenario=host-kills-client shooter=1 target=2" 'KRAKEN_MP_ACCEPT combat_armed role=client' $t0 30 1 $route;if(-not$result.Failed){throw 'Combat state allowed same-poll second arm and death'}
    $late=New-CombatAcceptanceState 'host-kills-client';Update-CombatAcceptanceState $late 'KRAKEN_MP_ACCEPT combat_armed role=host' '' $t0 30 1 $route|Out-Null;Update-CombatAcceptanceState $late '' 'KRAKEN_MP_ACCEPT combat_armed role=client' $t0.AddSeconds(1) 30 1 $route|Out-Null;$result=Update-CombatAcceptanceState $late 'KRAKEN_COMBAT_AUTOTEST death scenario=host-kills-client shooter=1 target=2' '' $t0.AddSeconds(31.001) 30 1 $route;if(-not$result.Failed){throw 'Combat state allowed death after deadline'}
    $wrong=New-CombatAcceptanceState 'host-kills-client';Update-CombatAcceptanceState $wrong 'KRAKEN_MP_ACCEPT combat_armed role=host' '' $t0 30 1 $route|Out-Null;Update-CombatAcceptanceState $wrong '' 'KRAKEN_MP_ACCEPT combat_armed role=client' $t0.AddSeconds(1) 30 1 $route|Out-Null;$result=Update-CombatAcceptanceState $wrong 'KRAKEN_COMBAT_AUTOTEST death scenario=host-kills-client shooter=2 target=1' '' $t0.AddSeconds(2) 30 1 $route;if(-not$result.Failed){throw 'Combat state allowed wrong shooter/target identity'}
    $valid=New-CombatAcceptanceState 'host-kills-client';Update-CombatAcceptanceState $valid "KRAKEN_MP_ACCEPT match state=Playing`nKRAKEN_MP_ACCEPT combat_armed role=host" '' $t0 30 1 $route|Out-Null;Update-CombatAcceptanceState $valid '' 'KRAKEN_MP_ACCEPT combat_armed role=client' $t0.AddSeconds(1) 30 1 $route|Out-Null;$result=Update-CombatAcceptanceState $valid 'KRAKEN_COMBAT_AUTOTEST death scenario=host-kills-client shooter=1 target=2' '' $t0.AddSeconds(2) 30 1 $route;if($result.Failed-or$null-eq$valid.DeathAt){throw 'Combat state rejected valid in-window death'};Update-CombatAcceptanceState $valid $script:RuntimeMarkers.HostSurvival 'KRAKEN_MP_ACCEPT session_exit role=client reason=death route=main_menu result=success' $t0.AddSeconds(3) 30 1 $route|Out-Null;$result=Update-CombatAcceptanceState $valid $script:RuntimeMarkers.HostSurvival '' $t0.AddSeconds(4) 30 1 $route;if($result.Failed-or-not$result.Accepted){throw 'Host-kills-client matrix rejected fresh survival evidence'}
    $result=Update-CombatAcceptanceState $valid 'KRAKEN_MP_ACCEPT session_exit role=host reason=death route=main_menu result=success' '' $t0.AddSeconds(4.1) 30 1 $route;if(-not$result.Failed){throw 'Host-kills-client matrix retained stale success after an unexpected host exit'}
    $stale=New-CombatAcceptanceState 'host-kills-client';Update-CombatAcceptanceState $stale "KRAKEN_MP_ACCEPT match state=Playing`nKRAKEN_MP_ACCEPT combat_armed role=host" '' $t0 30 10 $route|Out-Null;Update-CombatAcceptanceState $stale '' 'KRAKEN_MP_ACCEPT combat_armed role=client' $t0.AddSeconds(1) 30 10 $route|Out-Null;Update-CombatAcceptanceState $stale 'KRAKEN_COMBAT_AUTOTEST death scenario=host-kills-client shooter=1 target=2' '' $t0.AddSeconds(2) 30 10 $route|Out-Null;Update-CombatAcceptanceState $stale $script:RuntimeMarkers.HostSurvival 'KRAKEN_MP_ACCEPT session_exit role=client reason=death route=main_menu result=success' $t0.AddSeconds(3) 30 10 $route|Out-Null;Update-CombatAcceptanceState $stale '' '' $t0.AddSeconds(6) 30 10 $route|Out-Null;if($null-ne$stale.StableAt){throw 'Stale host survival evidence left a prior stability candidate active'}
    $clientKill=New-CombatAcceptanceState 'client-kills-host';Update-CombatAcceptanceState $clientKill 'KRAKEN_MP_ACCEPT combat_armed role=host' '' $t0 30 0 $route|Out-Null;Update-CombatAcceptanceState $clientKill '' 'KRAKEN_MP_ACCEPT combat_armed role=client' $t0.AddSeconds(1) 30 0 $route|Out-Null;$result=Update-CombatAcceptanceState $clientKill "KRAKEN_COMBAT_AUTOTEST death scenario=client-kills-host shooter=2 target=1`nKRAKEN_MP_ACCEPT session_exit role=host reason=death route=main_menu result=success" 'KRAKEN_MP_ACCEPT session_exit role=client reason=death route=main_menu result=success' $t0.AddSeconds(2) 30 0 $route;if($result.Failed-or-not$result.Accepted){throw 'Client-kills-host matrix did not require and accept two death exits'}
    $result=Update-CombatAcceptanceState $clientKill '' 'KRAKEN_MP_ACCEPT session_exit role=client reason=death route=main_menu result=failed' $t0.AddSeconds(2.1) 30 0 $route;if(-not$result.Failed){throw 'Combat matrix retained stale success after a failed exit result'}
    $terminated=New-CombatAcceptanceState 'client-kills-host';Update-CombatAcceptanceState $terminated 'KRAKEN_MP_ACCEPT combat_armed role=host' '' $t0 30 0 $route|Out-Null;Update-CombatAcceptanceState $terminated '' 'KRAKEN_MP_ACCEPT combat_armed role=client' $t0.AddSeconds(1) 30 0 $route|Out-Null;$result=Update-CombatAcceptanceState $terminated 'KRAKEN_COMBAT_AUTOTEST death scenario=client-kills-host shooter=2 target=1' 'KRAKEN_MP_ACCEPT session_exit role=client reason=host_terminated route=main_menu result=success' $t0.AddSeconds(2) 30 0 $route;if(-not$result.Failed){throw 'Client-kills-host matrix accepted host_terminated'}
    $bad=New-AcceptanceGate Client;$threw=$false;try{Update-AcceptanceGate $bad ($bad.Sequence[2]+$bad.Sequence[0])}catch{$threw=$true};if(-not$threw){throw 'Acceptance gate allowed reordered markers'}
    $runtime=Get-Content (Join-Path $script:Root 'source\net\runtime.cpp') -Raw
    if($runtime.Contains('EFA_MP.'+'BeginRaid') -or $runtime.Contains('FireFromWeapon'+'Custom2')){throw 'Runtime contains a removed EFA-specific acceptance seam'}
    $combatBody=[regex]::Match($runtime,'(?s)void run_combat_autotest_tick\(const float elapsed_time\)\s*\{.*?(?=void observe_authoritative_combat_autotest_death)').Value
    if($combatBody.Contains('SetPosition'+'Self') -or $combatBody.Contains('SetRotation'+'Self')){throw 'Combat acceptance path repositions a vehicle'}
    $readme=Get-Content (Join-Path $PSScriptRoot 'README-efa-multiplayer-harness.md') -Raw;foreach($required in @('automatic LAN','30 seconds','no UI/input emulation','never overwrites EFA Lua/XML','combat_armed')){if(-not$readme.Contains($required)){throw "README contract missing: $required"}}
    Write-Host 'PowerShell parser and deterministic supervisor/gate SelfCheck passed'
}

if($Action-ne'SelfCheck'){New-Item -ItemType Directory -Path $script:RunRoot -Force|Out-Null}
switch($Action){
    'Package'{New-Overlay|Write-Host}
    'Preflight'{Assert-BuildRoot;Test-Targets}
    'Deploy'{Deploy-Overlay}
    'Headless'{Invoke-Headless}
    'Unit'{Invoke-Unit}
    'Smoke'{Invoke-Smoke}
    'RaidCrashSmoke'{Invoke-RaidScenario 'forming'}
    'JipSmoke'{Invoke-RaidScenario 'jip'}
    'CombatHostKillsClient'{Invoke-RaidScenario 'forming' 'host-kills-client'}
    'CombatClientKillsHost'{Invoke-RaidScenario 'forming' 'client-kills-host'}
    'Collect'{Collect-Logs}
    'SelfCheck'{Invoke-SelfCheck}
    'All'{Invoke-SelfCheck;Deploy-Overlay;Invoke-Headless;Invoke-Unit;Invoke-Smoke;Invoke-RaidScenario 'forming';Invoke-RaidScenario 'jip';Invoke-RaidScenario 'forming' 'host-kills-client';Invoke-RaidScenario 'forming' 'client-kills-host';Collect-Logs}
}
if($Action-ne'SelfCheck'){Write-Host "Artifacts: $script:RunRoot"}
