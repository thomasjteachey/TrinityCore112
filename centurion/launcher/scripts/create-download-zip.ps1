$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path $PSScriptRoot -Parent
$launcher = Join-Path $projectRoot 'dist\CenturionLauncher.exe'
$archive = Join-Path $projectRoot 'dist\centurionlauncher.zip'
$versionFile = Join-Path $projectRoot 'dist\centurionlauncher.version'

if (-not (Test-Path -LiteralPath $launcher)) {
	throw "Portable launcher not found: $launcher"
}

Compress-Archive `
	-LiteralPath $launcher `
	-DestinationPath $archive `
	-CompressionLevel Optimal `
	-Force

if (Test-Path -LiteralPath $versionFile) {
	Remove-Item -LiteralPath $versionFile -Force
}

Write-Host "Created public download: $archive"
Write-Host 'The server centurionlauncher.version is managed independently.'
