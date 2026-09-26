# Rebuilds the zips that are stored in pieces (GitHub refuses files over 100 MB),
# then checks every zip against patches.md5. The PowerShell twin of join.sh.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

foreach ($first in Get-ChildItem -Filter '*.zip.part00') {
    $zip = $first.Name -replace '\.part00$', ''
    Write-Host "joining $zip"
    $out = [IO.File]::Create((Join-Path $PSScriptRoot $zip))
    try {
        foreach ($part in Get-ChildItem -Filter "$zip.part*" | Sort-Object Name) {
            $in = [IO.File]::OpenRead($part.FullName)
            try { $in.CopyTo($out) } finally { $in.Dispose() }
        }
    } finally {
        $out.Dispose()
    }
}

$bad = 0
foreach ($line in Get-Content patches.md5) {
    $hash, $name = $line -split '\s+', 2
    if ((Get-FileHash $name -Algorithm MD5).Hash -eq $hash) {
        Write-Host "$name`: OK"
    } else {
        Write-Host "$name`: FAILED"
        $bad++
    }
}
if ($bad) { exit 1 }
