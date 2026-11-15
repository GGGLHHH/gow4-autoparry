# 构建注入器和探针。用 VS Build Tools 自带的 CMake。用法：.\build.ps1
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { throw "找不到 VS 自带的 cmake：$cmake" }
& $cmake -S $PSScriptRoot -B "$PSScriptRoot\build" -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build "$PSScriptRoot\build" --config Release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "`n产物：$PSScriptRoot\build\Release\gow4_inject.exe + gow4_probe.dll"
