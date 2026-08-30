# mp_menu.ps1 -- the multiplayer "main menu": a companion window.
#
# TF2 game scripts only run once a map is loaded, so the engine's own main menu
# cannot host mod UI; this window is the session entry point instead:
#   * lists the host's saves (newest first)
#   * HOST SESSION: syncs the selected save to the peer and brings up the pair
#     (tools/mp_launch.ps1 -Save <name>) -- output streams into the window
#   * RELAUNCH: brings the pair up on whatever save is newest
#   * STOP: kills the harness and both game instances
#   * live status: the same lockstep_status_* files the in-game panel reads
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$R = Split-Path -Parent $PSScriptRoot
$Out = "C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out"
$Ovl = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C" + $Out.Substring(2)
$SaveA = "$((Get-ChildItem "C:\Program Files (x86)\Steam\userdata" -Directory | Where-Object { Test-Path (Join-Path $_.FullName "1066780") } | Select-Object -First 1).FullName)\1066780\local\save"
$LogFile = Join-Path $env:TEMP "mp_menu_launch.log"

$form = New-Object System.Windows.Forms.Form
$form.Text = "Transport Fever 2 -- Multiplayer"
$form.Size = New-Object System.Drawing.Size(560, 520)
$form.StartPosition = "CenterScreen"
$form.TopMost = $false

$lblSaves = New-Object System.Windows.Forms.Label
$lblSaves.Text = "Save to host:"
$lblSaves.Location = New-Object System.Drawing.Point(12, 10)
$lblSaves.AutoSize = $true
$form.Controls.Add($lblSaves)

$list = New-Object System.Windows.Forms.ListBox
$list.Location = New-Object System.Drawing.Point(12, 30)
$list.Size = New-Object System.Drawing.Size(520, 150)
$form.Controls.Add($list)

function Refresh-Saves {
    $list.Items.Clear()
    Get-ChildItem (Join-Path $SaveA "*.sav") -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "autosave*" } |
        Sort-Object LastWriteTime -Descending | ForEach-Object {
            $mb = [int]($_.Length / 1MB)
            [void]$list.Items.Add(("{0}  ({1} MB, {2:yyyy-MM-dd HH:mm})" -f $_.BaseName, $mb, $_.LastWriteTime))
        }
    if ($list.Items.Count -gt 0) { $list.SelectedIndex = 0 }
}
Refresh-Saves

$btnHost = New-Object System.Windows.Forms.Button
$btnHost.Text = "HOST SESSION (sync save + launch both)"
$btnHost.Location = New-Object System.Drawing.Point(12, 190)
$btnHost.Size = New-Object System.Drawing.Size(340, 32)
$form.Controls.Add($btnHost)

$btnRelaunch = New-Object System.Windows.Forms.Button
$btnRelaunch.Text = "RELAUNCH"
$btnRelaunch.Location = New-Object System.Drawing.Point(360, 190)
$btnRelaunch.Size = New-Object System.Drawing.Size(80, 32)
$form.Controls.Add($btnRelaunch)

$btnStop = New-Object System.Windows.Forms.Button
$btnStop.Text = "STOP"
$btnStop.Location = New-Object System.Drawing.Point(448, 190)
$btnStop.Size = New-Object System.Drawing.Size(84, 32)
$form.Controls.Add($btnStop)

$status = New-Object System.Windows.Forms.TextBox
$status.Location = New-Object System.Drawing.Point(12, 232)
$status.Size = New-Object System.Drawing.Size(520, 240)
$status.Multiline = $true
$status.ReadOnly = $true
$status.ScrollBars = "Vertical"
$status.Font = New-Object System.Drawing.Font("Consolas", 9)
$form.Controls.Add($status)

$script:launchProc = $null

function Start-Launch([string]$saveArg) {
    if ($script:launchProc -and -not $script:launchProc.HasExited) {
        $status.AppendText("a launch is already running`r`n"); return
    }
    Remove-Item $LogFile -ErrorAction SilentlyContinue
    $args = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "$R\tools\mp_launch.ps1")
    if ($saveArg) { $args += @("-Save", $saveArg) }
    $script:launchProc = Start-Process powershell -ArgumentList $args -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $LogFile
    $status.AppendText(("starting session{0}...`r`n" -f $(if ($saveArg) { " on save '$saveArg'" } else { "" })))
}

$btnHost.Add_Click({
    if ($list.SelectedItem) {
        $name = ($list.SelectedItem -split "  \(")[0]
        Start-Launch $name
    }
})
$btnRelaunch.Add_Click({ Start-Launch "" })
$btnStop.Add_Click({
    Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" |
        Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'autotest|mp_launch|slice_two_way' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Get-Process TransportFever2 -ErrorAction SilentlyContinue | Stop-Process -Force
    $status.AppendText("session stopped`r`n")
})

$script:lastLogLen = 0
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1500
$timer.Add_Tick({
    # stream new launcher output
    if (Test-Path $LogFile) {
        try {
            $txt = [IO.File]::ReadAllText($LogFile)
            if ($txt.Length -gt $script:lastLogLen) {
                $status.AppendText($txt.Substring($script:lastLogLen).Replace("`n", "`r`n"))
                $script:lastLogLen = $txt.Length
            }
        } catch {}
    }
    # live session rows in the title bar area
    $rows = @()
    foreach ($i in @("a", "b")) {
        foreach ($base in @($Out, $Ovl)) {
            $f = Join-Path $base "lockstep_status_$i.txt"
            if (Test-Path $f) {
                try { $rows += ("{0}  {1}" -f $i.ToUpper(), (Get-Content $f -TotalCount 1)) } catch {}
                break
            }
        }
    }
    $n = (Get-Process TransportFever2 -ErrorAction SilentlyContinue | Measure-Object).Count
    $form.Text = "TPF2 Multiplayer -- $n instance(s)" + $(if ($rows.Count -gt 0) { "   " + ($rows -join "   |   ") } else { "" })
})
$timer.Start()

[void]$form.ShowDialog()
