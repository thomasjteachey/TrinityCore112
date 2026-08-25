<#
.SYNOPSIS
  Regenerate server-side terrain data (maps / vmaps / mmaps) from the local
  client, for every map or just the ones you name, and optionally push the
  result to the game server.

.DESCRIPTION
  Runs the four TrinityCore tools in order, all pointed at one work directory:

    mapextractor    -i <client> -o <work> -e 1 [-m ids]      -> <work>\maps
    vmap4extractor  -d <client>\Data\ [-m ids]  (cwd <work>) -> <work>\Buildings
    vmap4assembler  Buildings vmaps [-m ids]    (cwd <work>) -> <work>\vmaps
    mmaps_generator [ids]                       (cwd <work>) -> <work>\mmaps

  -Maps is OPTIONAL. Leave it out and every tool runs exactly as it always did
  (all maps in Map.dbc - the full multi-hour extraction). Give one id or a
  list and only those maps are touched: the extractors walk only their
  WDT/ADTs, only the WMO/M2 models they place are converted, and the global
  gameobject model list is left alone. Nothing about the output format changes.

  With -Deploy the files produced by THIS run (maps/vmaps/mmaps of the named
  maps plus every .vmo model written by this run) are copied to each server
  data tree; anything overwritten there is first moved into
  <data>/backup-regen-<timestamp>/. Servers pick the files up on the next
  map/instance load - no restart needed for instance maps.

.EXAMPLE
  .\regen_map_data.ps1 -Maps 1608
  .\regen_map_data.ps1 -Maps 1608,1620 -Deploy
  .\regen_map_data.ps1                      # everything, like extractor.bat "4"
#>
[CmdletBinding()]
param(
    # Map ids to regenerate. Optional: omit for all maps.
    [uint32[]]$Maps = @(),
    [string]$Client = "C:\Projects\Gamedev\wow\clients\centurion",
    [string]$Work = "C:\Projects\Gamedev\wow\data\mapdata",
    # default: <repo>\Build\bin\RelWithDebInfo (resolved below; $PSScriptRoot is not usable in param defaults on PS 5.1)
    [string]$Tools = "",
    [switch]$SkipMaps,
    [switch]$SkipVmaps,
    [switch]$SkipMmaps,
    [switch]$KeepBuildings,
    # keep mmaps_generator's resume behaviour (existing valid .mmtile files are skipped)
    [switch]$Resume,
    [switch]$Deploy,
    [string]$SshHost = "brokilodeluxe@192.168.1.226",
    [string]$SshHostKey = "SHA256:A+/c0SFXvxUzvp9UtQeh/49loAJqkby0eEXNVy0O9Wc",
    [string[]]$DeployTargets = @(
        "/home/brokilodeluxe/wow/servers/tc-lplus-dev/data",
        "/home/brokilodeluxe/wow/servers/tc-legionnaireplus/data")
)

$ErrorActionPreference = "Stop"
$start = Get-Date
$mapList = ($Maps | ForEach-Object { [string]$_ }) -join ","
if (-not $Tools) {
    $repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
    $Tools = Join-Path $repo "Build\bin\RelWithDebInfo"
}

function Step($title) { Write-Host ""; Write-Host ("=== " + $title) -ForegroundColor Cyan }
function Run($exe, [string[]]$argv, $cwd) {
    Write-Host ("> " + $exe + " " + ($argv -join " ")) -ForegroundColor DarkGray
    Push-Location $cwd
    try {
        & $exe @argv
        if ($LASTEXITCODE -ne 0) { throw "$exe exited with $LASTEXITCODE" }
    } finally { Pop-Location }
}

foreach ($t in "mapextractor.exe", "vmap4extractor.exe", "vmap4assembler.exe", "mmaps_generator.exe") {
    if (-not (Test-Path (Join-Path $Tools $t))) { throw "missing tool $t in $Tools (build the TOOLS targets first)" }
}
if (-not (Test-Path (Join-Path $Client "Data\enUS"))) { throw "no client at $Client (expected Data\enUS)" }
New-Item -ItemType Directory -Force $Work | Out-Null

Write-Host ("client : " + $Client)
Write-Host ("work   : " + $Work)
Write-Host ("tools  : " + $Tools)
Write-Host ("maps   : " + $(if ($mapList) { $mapList } else { "ALL (no -Maps given)" }))

# mmaps_generator needs dbc\LiquidType.dbc next to maps/ and vmaps/.
if (-not (Test-Path (Join-Path $Work "dbc\LiquidType.dbc"))) {
    Step "dbc (LiquidType.dbc missing in work dir) - extracting client DBCs once"
    Run (Join-Path $Tools "mapextractor.exe") @("-i", $Client, "-o", $Work, "-e", "2") $Work
}

if (-not $SkipMaps) {
    Step "maps"
    $a = @("-i", $Client, "-o", $Work, "-e", "1")
    if ($mapList) { $a += @("-m", $mapList) }
    Run (Join-Path $Tools "mapextractor.exe") $a $Work
}

if (-not $SkipVmaps) {
    Step "vmaps: extract"
    $buildings = Join-Path $Work "Buildings"
    # vmap4extractor refuses (and waits on a keypress) if Buildings/dir_bin exists.
    if (Test-Path $buildings) { Remove-Item -Recurse -Force $buildings }
    $a = @("-d", (Join-Path $Client "Data\"))
    if ($mapList) { $a += @("-m", $mapList) }
    Run (Join-Path $Tools "vmap4extractor.exe") $a $Work

    Step "vmaps: assemble"
    $a = @("Buildings", "vmaps")
    if ($mapList) { $a += @("-m", $mapList) }
    Run (Join-Path $Tools "vmap4assembler.exe") $a $Work
    if (-not $KeepBuildings) { Remove-Item -Recurse -Force $buildings }
}

if (-not $SkipMmaps) {
    Step "mmaps"
    # mmaps_generator resumes: a tile whose .mmtile already exists with a valid
    # header is skipped. A regen must not inherit stale tiles, so drop the
    # requested maps' mmap files first (everything, for a full run) unless
    # -Resume asks for the tool's native resume behaviour.
    $mmapDir = Join-Path $Work "mmaps"
    if (-not $Resume -and (Test-Path $mmapDir)) {
        $stale = Get-ChildItem -File $mmapDir | Where-Object {
            if ($Maps.Count -eq 0) { return $true }
            foreach ($id in $Maps) {
                $p = "{0:D3}" -f $id
                if ($_.Name -match ("^" + [regex]::Escape($p) + "(\.mmap|\d{4}\.mmtile)$")) { return $true }
            }
            return $false
        }
        if ($stale) {
            Write-Host ("removing " + @($stale).Count + " existing mmap file(s) so they are rebuilt")
            $stale | Remove-Item -Force
        }
    }
    $a = @()
    if ($mapList) { $a += $mapList }
    Run (Join-Path $Tools "mmaps_generator.exe") $a $Work
}

# ---------------------------------------------------------------- results
Step "files produced by this run"
$produced = Get-ChildItem -Recurse -File (Join-Path $Work "maps"), (Join-Path $Work "vmaps"), (Join-Path $Work "mmaps") -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $start }
if ($Maps.Count -gt 0) {
    # keep only the named maps' tiles + every model written by this run.
    # Models are whatever the assembler wrote into vmaps\ that is not a
    # .vmtree/.vmtile: normally <name>.vmo, but WMO doodads that were .mdx in
    # the client come out as bare <name>.m2 (the extractor keeps the pre-rename
    # length, so the name carries a trailing NUL and fopen truncates it; the
    # server does the same truncation when loading, so the pair is consistent).
    $prefixes = $Maps | ForEach-Object { "{0:D3}" -f $_ }
    $vmapDir = Join-Path $Work "vmaps"
    $produced = $produced | Where-Object {
        $n = $_.Name
        if ($_.DirectoryName -eq $vmapDir -and $n -notmatch "^\d+\.vmtree$" -and $n -notmatch "^\d+_\d\d_\d\d\.vmtile$") { return $true }
        foreach ($p in $prefixes) {
            if ($n -match ("^" + [regex]::Escape($p) + "(\d{4}\.map|\.vmtree|_\d\d_\d\d\.vmtile|\.mmap|\d{4}\.mmtile)$")) { return $true }
        }
        return $false
    }
}
$produced | Sort-Object FullName | ForEach-Object { Write-Host ("  " + $_.FullName.Substring($Work.Length + 1) + "  " + $_.Length) }
Write-Host ("  " + @($produced).Count + " files")

if (-not $Deploy) {
    Write-Host ""
    Write-Host "Not deployed (add -Deploy to push to: $($DeployTargets -join ', '))" -ForegroundColor Yellow
    return
}

# ---------------------------------------------------------------- deploy
if (@($produced).Count -eq 0) { throw "nothing to deploy" }
$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$stage = "/home/brokilodeluxe/mapdata-staging/$ts"
Step ("deploy: staging " + @($produced).Count + " files to " + $stage)
& plink -batch -hostkey $SshHostKey $SshHost "mkdir -p $stage/maps $stage/vmaps $stage/mmaps"
if ($LASTEXITCODE -ne 0) { throw "plink mkdir failed" }
foreach ($sub in "maps", "vmaps", "mmaps") {
    $files = @($produced | Where-Object { $_.DirectoryName -eq (Join-Path $Work $sub) } | ForEach-Object { $_.FullName })
    if ($files.Count -eq 0) { continue }
    & pscp -batch -hostkey $SshHostKey @files "${SshHost}:$stage/$sub/"
    if ($LASTEXITCODE -ne 0) { throw "pscp $sub failed" }
}

$remote = @'
set -e
STAGE="__STAGE__"
TS="__TS__"
for DATA in __TARGETS__; do
  echo "== $DATA"
  [ -d "$DATA/maps" ] || { echo "   no maps dir, skipping"; continue; }
  BK="$DATA/backup-regen-$TS"
  for SUB in maps vmaps mmaps; do
    n=0; b=0
    mkdir -p "$DATA/$SUB"
    for f in "$STAGE/$SUB"/*; do
      [ -e "$f" ] || continue
      base=$(basename "$f")
      if [ -e "$DATA/$SUB/$base" ]; then
        mkdir -p "$BK/$SUB"
        cp -p "$DATA/$SUB/$base" "$BK/$SUB/$base"
        b=$((b+1))
      fi
      cp "$f" "$DATA/$SUB/.regen_tmp_$base"
      mv -f "$DATA/$SUB/.regen_tmp_$base" "$DATA/$SUB/$base"
      n=$((n+1))
    done
    echo "   $SUB: $n installed, $b backed up"
  done
  [ -d "$BK" ] && echo "   backups in $BK"
done
echo DEPLOY-DONE
'@
$remote = $remote.Replace("__STAGE__", $stage).Replace("__TS__", $ts).Replace("__TARGETS__", ($DeployTargets -join " ")).Replace("`r`n", "`n")
$tmp = [System.IO.Path]::GetTempFileName()
[System.IO.File]::WriteAllText($tmp, $remote, (New-Object System.Text.UTF8Encoding($false)))
& pscp -batch -hostkey $SshHostKey $tmp "${SshHost}:$stage/install.sh"
if ($LASTEXITCODE -ne 0) { throw "pscp install.sh failed" }
Remove-Item $tmp
& plink -batch -hostkey $SshHostKey $SshHost "bash $stage/install.sh"
if ($LASTEXITCODE -ne 0) { throw "remote install failed" }
Write-Host ""
Write-Host ("Deployed. Staging kept at " + $stage) -ForegroundColor Green
