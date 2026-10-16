<#
.SYNOPSIS  Builds the release zip: dist\zw3d-mcp-v<version>-win64.zip
           (the single plugin DLL + install guide; users copy the DLL by hand). Needs VS 2022 and CMake.
#>
param([string]$Version = "0.3.0")
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dist = Join-Path $repo "dist"
$stage = Join-Path $dist "zw3d-mcp-v$Version-win64"
Remove-Item $dist -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

# 1. plugin (clean Release build)
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { $vcvars = (Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat" | Select-Object -First 1).FullName }
$build = Join-Path $repo "plugin\build-release"
cmd /c "`"$vcvars`" >nul && cmake -S `"$repo\plugin`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build `"$build`""
if ($LASTEXITCODE -ne 0) { throw "plugin build failed" }
Copy-Item "$build\zw3dmcp.dll" $stage

# 2. docs
Copy-Item "$repo\README.md" $stage
Copy-Item "$repo\README.zh-CN.md" $stage
Copy-Item "$repo\docs\INSTALL.md" $stage
Compress-Archive -Path "$stage\*" -DestinationPath "$dist\zw3d-mcp-v$Version-win64.zip"
Get-FileHash "$dist\zw3d-mcp-v$Version-win64.zip" -Algorithm SHA256 | ForEach-Object { "$($_.Hash.ToLower())  $(Split-Path $_.Path -Leaf)" } | Set-Content "$dist\SHA256SUMS.txt"
Write-Host "Built $dist\zw3d-mcp-v$Version-win64.zip"
