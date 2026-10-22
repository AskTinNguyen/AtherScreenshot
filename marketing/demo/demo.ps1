# MARKETING DEMO ONLY (branch intent/marketing-0.0.2.5, never merged). Runs on GitHub's Windows runner: starts the real
# Ather Screenshot build, a self-playing game in Edge, and records the desktop with ffmpeg while it uses the 0.0.2.5
# features with real mouse and keyboard input (SendInput), at a human pace:
#   1. voice bug report while recording the game   2. Shift+drag + Ctrl+C a clip in the video editor, paste in Explorer
#   3. the bug folder and bug.png                  4. capture tray: three captures, annotate, Save selected
# The only stand-in: the runner has no microphone, so the app hears a text-to-speech WAV (ATHER_DEMO_VOICE_WAV hook).
param([string]$Exe, [string]$Out)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -Path "$PSScriptRoot\Demo.cs" -ReferencedAssemblies System.Drawing, System.Core
$log = "$Out\demo.log"
$sw = [Diagnostics.Stopwatch]::StartNew()
function Log($m) { $line = '{0,8:N2}  {1}' -f $sw.Elapsed.TotalSeconds, $m; $line | Add-Content $log; Write-Host $line }
function Wait($s) { Start-Sleep -Milliseconds ([int]($s * 1000)) }
function Ctrl($k) { [Demo]::Chord(0x11, [uint16][char]$k) }
function CtrlAlt($k) { [Demo]::Chord(0x11, 0x12, [uint16][char]$k) }
function Win1($cls, $part) { $w = [Demo]::Find($cls, $part); if ($w.Count) { $w[0] } else { [IntPtr]::Zero } }
function WaitWin($cls, $part, $secs) {
    for ($i = 0; $i -lt $secs * 10; $i++) { $h = Win1 $cls $part; if ($h -ne [IntPtr]::Zero) { return $h }; Wait 0.1 }
    Log "no window $cls '$part'"; Log ([Demo]::List()); return [IntPtr]::Zero
}
function ExplorerHwnd($urlPart, $secs) {  # Explorer windows by their folder
    for ($i = 0; $i -lt $secs * 5; $i++) {
        foreach ($w in (New-Object -ComObject Shell.Application).Windows()) { if ($w.LocationURL -like "*$urlPart*") { return [IntPtr]$w.HWND } }
        Wait 0.2
    }
    Log "no Explorer window $urlPart"; return [IntPtr]::Zero
}
function Center($h) { $c = [Demo]::Window($h); @([int]($c[0] + $c[2] / 2), [int]($c[1] + $c[3] / 2)) }

Log ("display: " + [Demo]::Resolution(1920, 1080))
$demo = 'C:\demo'
$captures = "$env:USERPROFILE\Pictures\Ather Screenshot"
New-Item -ItemType Directory -Force "$demo\game\logs", $captures, "$env:APPDATA\AtherScreenshot", "$env:USERPROFILE\Desktop\Shared clips" | Out-Null
Copy-Item "$PSScriptRoot\game.html" "$demo\game\game.html"

# The settings a playtester would pick (Settings window): voice commands on, the game's logs, no update checks.
@"
[Hotkeys]
RecordVideo=Ctrl+Alt+R
[Capture]
SaveFolder=$captures
[Recording]
RecordSystemAudio=0
CountdownSeconds=3
ShowClicks=1
[Editor]
Tray=1
[Updates]
CheckAutomatically=0
[Bugs]
VoiceCommands=1
LogPaths=$demo\game\logs\*.log
PreRollSeconds=10
"@ | Set-Content -Encoding Unicode "$env:APPDATA\AtherScreenshot\settings.ini"

# The narration, made with Windows text-to-speech (SAPI) at 16 kHz mono: the runner has no microphone.
$wav = "$demo\narration.wav"
$voice = New-Object -ComObject SAPI.SpVoice
$fmt = New-Object -ComObject SAPI.SpAudioFormat; $fmt.Type = 18  # SAFT16kHz16BitMono
$stream = New-Object -ComObject SAPI.SpFileStream; $stream.Format = $fmt; $stream.Open($wav, 3)
$voice.AudioOutputStream = $stream; $voice.Rate = -1
[void]$voice.Speak('<silence msec="500"/>Bug start.<silence msec="1000"/>The player falls through the floor.<silence msec="1000"/>Bug stop.<silence msec="500"/>', 8)
$stream.Close()
Log "narration: $((Get-Item $wav).Length) bytes"
$env:ATHER_DEMO_VOICE_WAV = $wav
$env:ATHER_DEMO_OPAQUE_TOASTS = '1'

# The game's log, as a game would write it.
$logJob = Start-Job -ArgumentList "$demo\game\logs\SkyLedge.log" -ScriptBlock {
    param($f)
    $n = 0
    while ($true) {
        $n++
        $t = Get-Date -Format 'HH:mm:ss.fff'
        $lines = @("[$t] INFO  frame $($n*30) dt=16.6ms entities=42", "[$t] DEBUG player x=$($n*165) grounded=true")
        if ($n % 40 -eq 20) { $lines += "[$t] WARN  collision: tile (x=$($n*3), y=3) has no collider in chunk 7" }
        Add-Content $f $lines
        Start-Sleep -Milliseconds 500
    }
}

# Desktop recording: our own BitBlt grabber (Demo.cs), piped to ffmpeg at 30 fps.
$ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
[Demo]::StartCapture($ff, "$Out\desktop.mkv")
[Demo]::WatchToasts($Out)
Wait 1
Log 'recording desktop'

# ---- start: the app (tray), the game ----
Start-Process $Exe
Wait 2
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
Start-Process $edge -ArgumentList '--user-data-dir=C:\demo\edge', '--no-first-run', '--no-default-browser-check', '--start-maximized', '--disable-features=msEdgeWelcomePage', '--app=file:///C:/demo/game/game.html'
$gameStart = $sw.Elapsed.TotalSeconds + 1.2  # the page starts its clock once loaded
$game = WaitWin $null 'Sky Ledge' 25
Log "game window $game"
Wait 3
Log ([Demo]::List())

# ---- 1. record the game, say "bug start … bug stop" ----
Log 'scene: record'
[Demo]::Move(1500, 700, 600)
CtrlAlt 'R'
Wait 1.2
[Demo]::Drag(160, 120, 1760, 1020, 1300)   # the region to record
Wait 3.5                                   # countdown
Log 'recording the game'
# The game drops the runner through the floor at 10 s, 30 s, 50 s… after it loads: speak right as it happens.
$next = $gameStart + 10; while ($next -lt $sw.Elapsed.TotalSeconds + 12) { $next += 20 }
Wait ($next - $sw.Elapsed.TotalSeconds + 0.3)
New-Item -ItemType File "$wav.go" | Out-Null
Log 'voice: narration plays (bug start … bug stop)'
Wait 13
CtrlAlt 'R'                                # stop recording
Log 'recording stopped'
Wait 2.5

# ---- 2. the recording in the video editor: Shift+drag, Ctrl+C, paste ----
Log 'scene: copy clip'
$toast = WaitWin 'AtherScreenshotToast' $null 5
$tc = Center $toast
[Demo]::Click($tc[0], $tc[1], 700)          # "Recording saved": click to edit
$ve = WaitWin 'AtherScreenshotVideoEditor' $null 15
Wait 1
[Demo]::ShowWindow($ve, 3) | Out-Null
Wait 3
$c = [Demo]::Client($ve)
Log "video editor client $($c -join ',')"
$sy = $c[1] + $c[3] - 132
$sx0 = $c[0] + 100; $sx1 = $c[0] + $c[2] - 14
$a = [int]($sx0 + ($sx1 - $sx0) * 0.50); $b = [int]($sx0 + ($sx1 - $sx0) * 0.74)
[Demo]::Move($a, $sy, 700); Wait 0.4
[Demo]::KeyDown(0x10); Wait 0.25
[Demo]::Down(); Wait 0.15; [Demo]::Move($b, $sy, 1400); Wait 0.2; [Demo]::Up()
Wait 0.25; [Demo]::KeyUp(0x10)
Log 'range marked'
Wait 1.5
Ctrl 'C'
Log 'Ctrl+C'
Wait 3.5
Start-Process explorer.exe "`"$env:USERPROFILE\Desktop\Shared clips`""
$ex = ExplorerHwnd 'Shared%20clips' 10
Wait 1
[Demo]::MoveWindow($ex, 940, 330, 900, 560, $true) | Out-Null
Wait 1
[Demo]::Click(1390, 700, 800)
Wait 0.6
Ctrl 'V'
Log 'Ctrl+V in Explorer'
Wait 3.5
$vw = [Demo]::Window($ve)
[Demo]::Click(600, $vw[1] + 14, 700)       # back to the video editor
Wait 0.5
[Demo]::Chord(0x11, 0x10, 0x43)            # Ctrl+Shift+C: the frame
Log 'Ctrl+Shift+C'
Wait 3

# ---- 3. the bug folder ----
Log 'scene: bug folder'
$bug = Get-ChildItem "$captures\Bugs" -Directory | Sort-Object CreationTime | Select-Object -Last 1
Log "bug folder: $($bug.FullName)"
Get-ChildItem -Recurse $bug.FullName | ForEach-Object { Log "  $($_.FullName.Substring($bug.FullName.Length)) $($_.Length)" }
[Demo]::ShowWindow($ex, 6) | Out-Null
Start-Process explorer.exe "`"$($bug.FullName)`""
$bx = ExplorerHwnd ([Uri]::EscapeUriString($bug.Name)) 10
Wait 1
[Demo]::MoveWindow($bx, 200, 140, 1520, 800, $true) | Out-Null
Wait 1
[Demo]::Click(1200, 820, 800)
[Demo]::Chord(0x11, 0x10, 0x32)            # Ctrl+Shift+2: large icons
Wait 4
$png = 'file:///' + ($bug.FullName.Replace('\','/') + '/bug.png').Replace(' ', '%20')
Start-Process $edge -ArgumentList '--user-data-dir=C:\demo\edge2', '--no-first-run', '--no-default-browser-check', '--start-maximized', "--app=$png"
Wait 6
Start-Process notepad.exe "`"$($bug.FullName)\bug.md`""
Wait 2
$np = WaitWin $null 'bug.md' 10
if ($np -ne [IntPtr]::Zero) { [Demo]::MoveWindow($np, 360, 120, 1200, 820, $true) | Out-Null }
Wait 5
Log ([Demo]::List())

# ---- 4. capture tray ----
Log 'scene: tray'
Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force
Get-CimInstance Win32_Process -Filter "name='msedge.exe'" | Where-Object { $_.CommandLine -like '*edge2*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
foreach ($w in (New-Object -ComObject Shell.Application).Windows()) { $w.Quit() }
[Demo]::ShowWindow($ve, 6) | Out-Null
Wait 2
Log ([Demo]::List())
[Demo]::Click(1840, 560, 700)               # back to the game
Wait 1.5
CtrlAlt 'E'
Wait 1.2
[Demo]::Drag(300, 560, 1100, 1010, 1100)    # picture 1: the runner
$ed = WaitWin 'AtherScreenshotEditor' $null 10
Wait 0.8
[Demo]::MoveWindow($ed, 260, 70, 1400, 900, $true) | Out-Null
Wait 1
$c = [Demo]::Client($ed); $cx = $c[0] + [int]($c[2] / 2); $cy = $c[1] + 52 + [int](($c[3] - 52 - 26) / 2)
Log "editor client $($c -join ',')"
[Demo]::Drag($cx + 230, $cy - 170, $cx - 90, $cy + 20, 900)   # an arrow at the runner
Wait 1.2
[Demo]::Click(1840, 560, 700)
Wait 1
CtrlAlt 'E'
Wait 1.2
[Demo]::Drag(1600, 40, 1912, 216, 900)      # picture 2: the score
Wait 2
[Demo]::Click(1840, 560, 700)
Wait 1
CtrlAlt 'E'
Wait 1.2
[Demo]::Drag(860, 300, 1660, 750, 1000)     # picture 3: the level ahead
Wait 2
$c = [Demo]::Client($ed); Log "editor client $($c -join ',')"
$st = $c[1] + $c[3] - 26 - 96
function Thumb($i) { @(($c[0] + 14 + $i * 132 + 60), ($st + 14 + 34)) }
$cx = $c[0] + [int]($c[2] / 2); $cy = $c[1] + 52 + [int](($c[3] - 52 - 26 - 96) / 2)
[Demo]::KeyDown(0x52); [Demo]::KeyUp(0x52)  # R: rectangle
[Demo]::Drag($cx - 260, $cy - 120, $cx + 120, $cy + 160, 900)
Wait 1.2
$t = Thumb 0; [Demo]::Click($t[0], $t[1], 800)   # back to picture 1: its arrow is still there
Wait 2
$t = Thumb 1; [Demo]::Click($t[0] - 46, $t[1] - 20, 800)   # untick picture 2
Wait 1.5
[Demo]::Click($c[0] + $c[2] - 98, $c[1] + $c[3] - 59, 900)  # Save selected
Log 'Save selected'
Wait 4
Get-ChildItem $captures -File | ForEach-Object { Log "  capture: $($_.Name) $($_.Length)" }
Log ([Demo]::List())

# ---- done ----
$code = [Demo]::StopCapture()
Log "ffmpeg exit $code, frames grabbed $([Demo]::CapturedFrames)"
Stop-Job $logJob
New-Item -ItemType Directory -Force "$Out\captures" | Out-Null
Copy-Item -Recurse "$captures\*" "$Out\captures" -ErrorAction SilentlyContinue
Copy-Item $wav $Out
