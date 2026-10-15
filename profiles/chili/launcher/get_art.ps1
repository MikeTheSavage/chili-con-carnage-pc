# Fills the optional art folder beside the built launcher from textures extracted from the player's OWN
# disc (mods\TEXTURES.md: tools\extract_textures.py extract <archive> <folder>). The pictures are the
# game's and are never committed or put into the executable: the art folder is git-ignored, and the
# launcher looks finished without it.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File get_art.ps1 -Textures <extracted folder> [-To <folder with the launcher>]
#       [-Hero loadscreenmacho1] [-Mods <mods folder>]
#
# -Mods: where the player's texture packs are (mods\<pack>\textures\<key>.png). When a pack holds a larger
# copy of one of the three pictures, that copy is taken instead of the disc's 512 x 272 one: the launcher
# shows it at the same size, sharp on a large monitor. The key of a picture is read from the extracted
# folder's index.csv.
param(
    [Parameter(Mandatory = $true)][string]$Textures,
    [string]$To = "",
    [string]$Hero = "loadscreenmacho1",
    [string]$Mods = ""
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $To) { $To = Join-Path $here 'bin' }
$art = Join-Path $To 'art'
New-Item -ItemType Directory -Force $art | Out-Null
$wanted = [ordered]@{
    'logo.png'       = 'data\textures\menu\gamelogo_frontscreen.png'            # key 048e4eecf23e28a1
    'background.png' = 'data\textures\menu\backgrounds\background01.png'        # key e1215277c6aafa16
    'hero.png'       = "data\textures\menu\loadscreens\$Hero.png"
}

# png path (as index.csv writes it, with forward slashes) -> key
$keys = @{}
$index = Join-Path $Textures 'index.csv'
if ($Mods -and (Test-Path $index)) {
    foreach ($row in (Import-Csv $index)) { if ($row.png) { $keys[$row.png.ToLowerInvariant()] = $row.key } }
}

foreach ($name in $wanted.Keys) {
    $from = Join-Path $Textures $wanted[$name]
    $key = $keys[$wanted[$name].Replace('\', '/').ToLowerInvariant()]
    if ($key -and (Test-Path $Mods)) {
        # The largest copy any pack has of this picture.
        $better = Get-ChildItem $Mods -Directory | ForEach-Object { Join-Path $_.FullName 'textures' } | Where-Object { Test-Path $_ } |
            ForEach-Object { Get-ChildItem $_ -Recurse -File -Filter "$key.png" } | Sort-Object Length -Descending | Select-Object -First 1
        if ($better -and (-not (Test-Path $from) -or $better.Length -gt (Get-Item $from).Length)) { $from = $better.FullName }
    }
    if (Test-Path $from) { Copy-Item $from (Join-Path $art $name) -Force; Write-Host "$name <- $from" }
    else { Write-Host "${name}: not found ($from); the launcher draws its own" }
}
