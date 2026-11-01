# SPDX-License-Identifier: Apache-2.0
# Starts a portable OBS with the plugin, waits, saves a screenshot of the OBS window, closes it.
#   powershell -File tests\obs-window-shot.ps1 -Obs <portable obs> -Out shot.png [-Wait 12]
param([string]$Obs, [string]$Out, [int]$Wait = 12)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hgt, uint flags);
}
"@
$exe = Join-Path $Obs 'bin\64bit\obs64.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--portable','--disable-updater','--disable-shutdown-check','--multi' -WorkingDirectory (Split-Path $exe) -PassThru
try {
  $h = [IntPtr]::Zero
  for ($i = 0; $i -lt 60 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 500; $p.Refresh(); $h = $p.MainWindowHandle }
  if ($h -eq [IntPtr]::Zero) { throw 'OBS window not found' }
  [Win]::ShowWindow($h, 9) | Out-Null
  [Win]::SetWindowPos($h, [IntPtr]::Zero, 40, 40, 1600, 1000, 0x0040) | Out-Null
  Start-Sleep -Seconds $Wait
  [Win]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 800
  $r = New-Object Win+RECT
  [Win]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  "saved $Out ($w x $hh)"
} finally {
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
