# End-to-end: a foreign process with a text box; keys typed into it; the QuickPrompts dev tool expands the trigger.
# Usage: powershell -STA -File e2e.ps1 <outFile> <typed>
param([string]$Out, [string]$Typed = "Hello doitforme ")
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
"@
$form = New-Object System.Windows.Forms.Form
$form.Text = "QuickPrompts e2e target"
$form.Width = 900; $form.Height = 400; $form.TopMost = $true
$form.StartPosition = "CenterScreen"
$box = New-Object System.Windows.Forms.TextBox
$box.Multiline = $true; $box.AcceptsReturn = $true; $box.Dock = "Fill"
$box.Font = New-Object System.Drawing.Font("Consolas", 12)
$form.Controls.Add($box)
$enters = 0
$box.Add_KeyDown({ if ($_.KeyCode -eq [System.Windows.Forms.Keys]::Enter) { $script:enters++ } })
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1500
$step = 0
$timer.Add_Tick({
    $script:step++
    if ($script:step -eq 1) {
        [W]::SetForegroundWindow($form.Handle) | Out-Null
        $box.Focus() | Out-Null
        # Type key by key, like a person (SendKeys escapes: none needed for letters and space).
        foreach ($ch in $Typed.ToCharArray()) { [System.Windows.Forms.SendKeys]::SendWait([string]$ch); Start-Sleep -Milliseconds 40 }
    } elseif ($script:step -eq 3) {
        $text = $box.Text
        $result = "ENTERS=" + $script:enters + "`r`n" + $text
        [System.IO.File]::WriteAllText($Out, $result, [System.Text.Encoding]::UTF8)
        $form.Close()
    }
})
$form.Add_Shown({ $form.Activate(); $timer.Start() })
[System.Windows.Forms.Application]::Run($form)
