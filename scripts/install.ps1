<#
.SYNOPSIS  Installs ZW3D-MCP: the plugin DLL into ZW3D's per-user apilibs folder, and the Python MCP server.
.EXAMPLE   powershell -ExecutionPolicy Bypass -File install.ps1
.EXAMPLE   powershell -ExecutionPolicy Bypass -File install.ps1 -Year 2026 -SkipPython
#>
param(
  [string]$Year = "",          # ZW3D release year; auto-detected from %APPDATA%\ZWSOFT\ZW3D\ZW3D <year> when empty
  [switch]$SkipPython,         # only install the DLL
  [switch]$SkipPlugin          # only install the Python package
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $SkipPlugin) {
  if (Get-Process -Name "ZW3D" -ErrorAction SilentlyContinue) {
    throw "ZW3D is running. Close it first (a loaded DLL cannot be overwritten), then run this script again."
  }
  $dll = Join-Path $here "zw3dmcp.dll"
  if (-not (Test-Path $dll)) { throw "zw3dmcp.dll not found next to install.ps1" }

  $root = Join-Path $env:APPDATA "ZWSOFT\ZW3D"
  if ($Year) { $years = @($Year) }
  else {
    $years = @(Get-ChildItem $root -Directory -Filter "ZW3D 20*" -ErrorAction SilentlyContinue |
               ForEach-Object { $_.Name -replace "^ZW3D ", "" } | Where-Object { [int]$_ -ge 2025 })
  }
  if (-not $years) { throw "No ZW3D 2025+ user folder under $root. Start ZW3D once, or pass -Year 2025." }
  foreach ($y in $years) {
    $dest = Join-Path $root "ZW3D $y\custom\apilibs"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item $dll $dest -Force
    Write-Host "Installed plugin -> $dest\zw3dmcp.dll"
  }
}

if (-not $SkipPython) {
  $py = Get-Command python -ErrorAction SilentlyContinue
  if (-not $py) { throw "Python 3.10+ not found in PATH." }
  $wheel = Get-ChildItem $here -Filter "zw3d_mcp-*.whl" | Select-Object -First 1
  if (-not $wheel) { throw "zw3d_mcp wheel not found next to install.ps1" }
  & python -m pip install --upgrade $wheel.FullName
  if ($LASTEXITCODE -ne 0) { throw "pip install failed" }
}

Write-Host ""
Write-Host "Done. Next:"
Write-Host "  1. Start ZW3D. The output window should show: [ZW3D-MCP] listening on 127.0.0.1:47823"
Write-Host "  2. Register the MCP server, e.g. for Claude Code:   claude mcp add zw3d -- python -m zw3d_mcp"
Write-Host "     Claude Desktop / Cursor: command 'python', args ['-m','zw3d_mcp']"
