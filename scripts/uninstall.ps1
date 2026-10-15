param([string]$Year = "")
$ErrorActionPreference = "Stop"
if (Get-Process -Name "ZW3D" -ErrorAction SilentlyContinue) { throw "Close ZW3D first." }
$root = Join-Path $env:APPDATA "ZWSOFT\ZW3D"
Get-ChildItem $root -Directory -Filter "ZW3D 20*" -ErrorAction SilentlyContinue | ForEach-Object {
  $f = Join-Path $_.FullName "custom\apilibs\zw3dmcp.dll"
  if ((-not $Year -or $_.Name -eq "ZW3D $Year") -and (Test-Path $f)) { Remove-Item $f -Force; Write-Host "Removed $f" }
}
python -m pip uninstall -y zw3d-mcp
