param(
    [string]$RepositoryRoot='',
    [string]$OutputDirectory='',
    [string]$CodegenPath=''
)
$ErrorActionPreference='Stop'
if(!$RepositoryRoot){$RepositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))}
if(!$OutputDirectory){$OutputDirectory=Join-Path $RepositoryRoot 'profiles\chili\analysis\vfpu_pack_fixture'}
if(!$CodegenPath){$CodegenPath=Join-Path $RepositoryRoot 'out\framework\Release\psp_recomp.exe'}
if(!(Test-Path -LiteralPath $CodegenPath -PathType Leaf)){throw "Missing live generator: $CodegenPath"}
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null

# Documented byte-quad/short-pair/short-quad forms, row/column views, overlapping
# source/output and two invalid vector lengths. Each routine ends with JR RA.
[uint32[]]$words=@('D03D8282','D03C8383','D03DA088','D03C8084',
    'D03F8188','D03E8188','D03F0188','D03E0188',
    'D03D8080','D03C8080','D03D0282','D03C8384',
    'D03F81AC','D03F8121') | ForEach-Object { [Convert]::ToUInt32($_,16) }
[byte[]]$fixture=New-Object byte[] (0x80+$words.Count*0x20)
function Put16($where,[uint16]$value){[BitConverter]::GetBytes($value).CopyTo($fixture,$where)}
function Put32($where,[uint32]$value){[BitConverter]::GetBytes($value).CopyTo($fixture,$where)}
$fixture[0]=0x7F;$fixture[1]=0x45;$fixture[2]=0x4C;$fixture[3]=0x46;$fixture[4]=1;$fixture[5]=1;$fixture[6]=1
Put16 16 2;Put16 18 8;Put32 20 1;Put32 24 0x08804000;Put32 28 52
Put16 40 52;Put16 42 32;Put16 44 1;Put32 52 1;Put32 56 0x80
Put32 60 0x08804000;Put32 64 0x08804000;Put32 68 ($words.Count*0x20)
Put32 72 ($words.Count*0x20);Put32 76 5;Put32 80 16
$csv=[Collections.Generic.List[string]]::new();$csv.Add('name,address,size')
for($index=0;$index -lt $words.Count;$index++){
    Put32 (0x80+$index*0x20) $words[$index]
    Put32 (0x84+$index*0x20) 0x03E00008
    $csv.Add(('pack_{0},0x{1:X8},0x0C' -f $index,(0x08804000+$index*0x20)))
}
$fixturePath=Join-Path $OutputDirectory 'fixture.elf'
$functionsPath=Join-Path $OutputDirectory 'functions.csv'
$generatedPath=Join-Path $OutputDirectory 'generated.cpp'
[IO.File]::WriteAllBytes($fixturePath,$fixture)
[IO.File]::WriteAllLines($functionsPath,$csv)
& $CodegenPath $fixturePath $functionsPath $generatedPath
if($LASTEXITCODE -ne 0){throw "Live fixture generator failed: exit $LASTEXITCODE"}
