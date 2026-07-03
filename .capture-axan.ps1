# Capture the axan main window to a PNG so the sidebar's templated labels can be read
# back (M5 verification). Targets axan's own window only (not the whole desktop).
# PrintWindow with PW_RENDERFULLCONTENT (2) is required for the DirectComposition/WinUI
# content; if it comes back blank we fall back to a cropped grab of the window rectangle
# off the live desktop.
param([string]$Out = (Join-Path $PSScriptRoot '.axan-m5.png'))

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$p = Get-Process axan -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Host 'NO-AXAN-WINDOW'; exit 1 }
$h = $p.MainWindowHandle
[Win]::ShowWindow($h, 9) | Out-Null      # SW_RESTORE
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 600

$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $hgt = $r.Bottom - $r.Top
Write-Host "window $w x $hgt at $($r.Left),$($r.Top)"

$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [Win]::PrintWindow($h, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()

# Detect an all-black/blank PrintWindow result (common for GPU-composited windows).
$blank = $true
for ($y = 0; $y -lt $hgt -and $blank; $y += [Math]::Max(1, [int]($hgt/40))) {
  for ($x = 0; $x -lt $w -and $blank; $x += [Math]::Max(1, [int]($w/40))) {
    $c = $bmp.GetPixel($x, $y)
    if ($c.R -gt 12 -or $c.G -gt 12 -or $c.B -gt 12) { $blank = $false }
  }
}
if ($blank) {
  Write-Host 'PRINTWINDOW-BLANK -> desktop grab of window rect'
  $bmp.Dispose()
  $bmp = New-Object System.Drawing.Bitmap $w, $hgt
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $hgt))
  $g.Dispose()
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "SAVED $Out"
