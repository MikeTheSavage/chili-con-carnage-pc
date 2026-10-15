# Puts pictures side by side on one sheet, each with its name under it, for looking at many at once.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File installer\test\sheet.ps1 -Folder <pictures> -Out <sheet.png>
#       [-Filter "0*.png"] [-Columns 3] [-Width 600] [-Names "01_welcome,02_disc_ok"]
param(
    [Parameter(Mandatory = $true)][string]$Folder,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Filter = '*.png',
    [int]$Columns = 3,
    [int]$Width = 600,
    [string]$Names = ''
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$files = @(Get-ChildItem -LiteralPath $Folder -Filter $Filter | Sort-Object Name)
if ($Names) { $wanted = $Names.Split(",") | ForEach-Object { $_.Trim() }; $files = @($files | Where-Object { $wanted -contains $_.BaseName }) }
if (-not $files.Count) { throw "No pictures in $Folder matching $Filter" }
$first = [Drawing.Image]::FromFile($files[0].FullName)
$cellH = [int]([double]$Width * $first.Height / $first.Width)
$first.Dispose()
$label = 22
$rows = [Math]::Ceiling($files.Count / $Columns)
$sheet = New-Object Drawing.Bitmap ($Columns * ($Width + 8) + 8), ($rows * ($cellH + $label + 8) + 8)
$g = [Drawing.Graphics]::FromImage($sheet)
$g.Clear([Drawing.Color]::FromArgb(24, 24, 24))
$g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$font = New-Object Drawing.Font 'Segoe UI', 11
for ($i = 0; $i -lt $files.Count; $i++) {
    $x = 8 + ($i % $Columns) * ($Width + 8)
    $y = 8 + [Math]::Floor($i / $Columns) * ($cellH + $label + 8)
    $img = [Drawing.Image]::FromFile($files[$i].FullName)
    $g.DrawImage($img, $x, $y, $Width, $cellH)
    $img.Dispose()
    $g.DrawString($files[$i].BaseName, $font, [Drawing.Brushes]::White, $x, $y + $cellH + 2)
}
$sheet.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $sheet.Dispose()
Write-Host "$Out ($($files.Count) pictures)"
