# 构建。优先用 VS / VS Build Tools 自带的 CMake，没有就用 PATH 上的（GitHub Actions 也是跑这个脚本）。
# 用法：.\build.ps1          # 开发版：全部目标 → build\Release（修改器用它旁边单独编出来的两个 DLL）
#       .\build.ps1 -Dist    # 成品：dist\gow4_trainer.exe 一个文件（两个 DLL 编在里面），在单独的 build-dist\ 里编，不碰 build\Release
# 注意：游戏开着并且加载着 build\Release 里的 DLL 时，开发版会因为 DLL 被占用而链接失败；-Dist 不受影响。
param([switch]$Dist)

$ErrorActionPreference = 'Stop'
$cmake = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) {
        $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $bundled) { $cmake = $bundled }
    }
}
if (-not $cmake) {
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { $cmake = $onPath.Source }
}
if (-not $cmake) { throw '找不到 cmake：装 Visual Studio 2022（或 Build Tools）的「使用 C++ 的桌面开发」，或者把 cmake 放进 PATH' }

$buildDir = if ($Dist) { "$PSScriptRoot\build-dist" } else { "$PSScriptRoot\build" }
$portable = if ($Dist) { 'ON' } else { 'OFF' }
& $cmake -S $PSScriptRoot -B $buildDir -G 'Visual Studio 17 2022' -A x64 "-DGOW4_PORTABLE=$portable"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
# 成品只要修改器（它依赖两个 DLL，会一起编）；探针、注入器这些开发工具不编
$target = if ($Dist) { @('--target', 'gow4_trainer') } else { @() }
& $cmake --build $buildDir --config Release @target
if ($LASTEXITCODE) { exit $LASTEXITCODE }

if (-not $Dist) {
    Write-Host "`n产物：$buildDir\Release\gow4_trainer.exe（界面）、gow4_autoparry.dll、gow4_esp.dll、gow4_inject.exe（命令行注入）等"
    exit 0
}

$distDir = "$PSScriptRoot\dist"
New-Item -ItemType Directory -Force $distDir | Out-Null
Copy-Item "$buildDir\Release\gow4_trainer.exe" $distDir -Force
Write-Host "`n产物：$distDir\gow4_trainer.exe（单文件）。DLL 和配置在运行时放到 %LOCALAPPDATA%\gow4_trainer\"
