# Fails when a folder (the repository, a staging folder, an install) or an installer holds anything of the
# game or of a player: the disc's files by name, by extension or by their first and last bytes, audio,
# save folders, the generated code, pictures larger than the project's own.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File installer\test\check_no_game_data.ps1 -Path <folder or Setup.exe>
#       [-Allow "mods\hd_loading"]     relative folders whose pictures are allowed (the HD loading pictures of a
#                                      release)
#
# For an installer the payload's file list is checked (Setup.exe --list). The last line says PASS or FAIL;
# the exit code is the number of findings.
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Allow = ''
)
$ErrorActionPreference = 'Stop'
$Path = (Resolve-Path -LiteralPath $Path).Path
$allowed = @($Allow.Split(',') | ForEach-Object { $_.Trim().TrimEnd('\').Replace('/', '\') } | Where-Object { $_ })
$names = @('boot.bin', 'eboot.bin', 'umd_data.bin', 'icon0.png', 'icon1.pmf', 'pic0.png', 'pic1.png', 'snd0.at3', 'param.sfo', 'configpsp.txt', 'opnssmp.bin', 'data.bin')
$extensions = @('.iso', '.cso', '.naz', '.at3', '.pmf', '.prx', '.sfo', '.texture_psp', '.wav', '.mediastream_data_psp', '.dump')
$folders = @('savedata', 'savedata_previous', 'saves', 'texture_dump', 'captures')
$findings = New-Object System.Collections.Generic.List[string]
function Find([string]$What) { $findings.Add($What); Write-Host "FOUND $What" }
function IsAllowed([string]$Rel) { foreach ($a in $allowed) { if ($Rel.StartsWith($a + '\', [StringComparison]::OrdinalIgnoreCase)) { return $true } }; return $false }

function CheckName([string]$Rel) {
    $leaf = [IO.Path]::GetFileName($Rel).ToLowerInvariant()
    $parts = $Rel.ToLowerInvariant().Split('\')
    if ($names -contains $leaf) { Find "$Rel (a file of the disc by its name)" }
    if ($extensions -contains [IO.Path]::GetExtension($leaf)) { Find "$Rel (a game, disc or audio file by its extension)" }
    foreach ($p in $parts[0..([Math]::Max(0, $parts.Count - 2))]) {
        if ($folders -contains $p) { Find "$Rel (inside a save, dump or capture folder)"; break }
        if ($p -match '^ulus10216') { Find "$Rel (inside a save folder of the game)"; break }
    }
    if ($Rel -match '(?i)^profiles\\[^\\]+\\(analysis|game|captures|progress)\\') { Find "$Rel (generated or extracted from the disc)" }
    if ($Rel -match '(?i)^profiles\\vcs\\generated\\') { Find "$Rel (generated code of another game)" }
}

if (Test-Path -LiteralPath $Path -PathType Leaf) {
    # An installer: its payload's file list.
    $list = Join-Path ([IO.Path]::GetTempPath()) ("chili_list_" + [guid]::NewGuid().ToString('N') + '.txt')
    try {
        $p = Start-Process -FilePath $Path -ArgumentList '--list' -NoNewWindow -Wait -PassThru -RedirectStandardOutput $list
        $lines = @(Get-Content -LiteralPath $list)
    } finally { Remove-Item -LiteralPath $list -ErrorAction SilentlyContinue }
    if ($p.ExitCode -ne 0) { Find "$Path has no readable payload (exit $($p.ExitCode))" }
    $count = 0
    foreach ($line in $lines) {
        if ($line -notmatch '^(\S+)\s+(\d+)\s+(.+)$') { continue }
        $count++
        $rel = $Matches[3].Replace('/', '\')
        CheckName $rel
        if ($rel -match '(?i)\.png$' -and -not (IsAllowed $rel)) { Find "$rel (a picture in the payload outside the allowed folders)" }
    }
    Write-Host "$count files in the payload of $Path"
} else {
    $files = @(Get-ChildItem -LiteralPath $Path -Recurse -File -Force | Where-Object { $_.FullName -notmatch '\\\.git\\' })
    Add-Type -AssemblyName System.Drawing
    # The installer's build output (git-ignored) is not part of the tree.
    $builtOut = if ((Split-Path -Leaf $Path) -eq 'installer') { '(?i)^(bin|obj)\\' } else { '(?i)^installer\\(bin|obj)\\' }
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($Path.TrimEnd('\').Length + 1)
        if ($rel -match $builtOut) { continue }
        CheckName $rel
        if ($f.Length -lt 4) { continue }
        $stream = [IO.File]::OpenRead($f.FullName)
        try {
            $head = New-Object byte[] 64
            [void]$stream.Read($head, 0, 64)
            $tail = New-Object byte[] 22
            if ($f.Length -ge 22) { $stream.Position = $f.Length - 22; [void]$stream.Read($tail, 0, 22) }
        } finally { $stream.Dispose() }
        $magic = [Text.Encoding]::ASCII.GetString($head, 0, 4)
        if ($magic -eq 'CISO') { Find "$rel (a compressed disc image)" }
        if ($magic -eq "`0PBP" -or $magic -eq '~PSP') { Find "$rel (a PSP program)" }
        if ($magic -eq 'PSMF') { Find "$rel (a PSP movie)" }
        if ($magic -eq 'RIFF' -and [Text.Encoding]::ASCII.GetString($head, 8, 4) -eq 'WAVE') { Find "$rel (audio)" }
        if ($f.Length -ge 22 -and [BitConverter]::ToUInt32($tail, 0) -eq 0x16ED5B50) { Find "$rel (a NAZ archive of the game)" }
        if ($f.Length -gt 34000 -and $f.Length -ge 16 * 2048) {
            $s2 = [IO.File]::OpenRead($f.FullName)
            try { $s2.Position = 16 * 2048 + 1; $cd = New-Object byte[] 5; [void]$s2.Read($cd, 0, 5) } finally { $s2.Dispose() }
            if ([Text.Encoding]::ASCII.GetString($cd) -eq 'CD001') { Find "$rel (an ISO 9660 disc image)" }
        }
        if ($f.Extension -eq '.png' -and -not (IsAllowed $rel) -and $rel -notmatch '(?i)^installer\\') {
            try {
                $img = [Drawing.Image]::FromFile($f.FullName)
                try { if ($img.Width -gt 1024 -or $img.Height -gt 1024) { Find "$rel (a picture of $($img.Width)x$($img.Height), larger than any of the project's own)" } } finally { $img.Dispose() }
            } catch { }
        }
    }
    Write-Host "$($files.Count) files in $Path"
}
if ($findings.Count -eq 0) { Write-Host "PASS no game data and no player's data in $Path" } else { Write-Host "FAIL $($findings.Count) findings in $Path" }
exit $findings.Count
