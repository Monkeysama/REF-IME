param(
    [switch]$SkipTests,
    [ValidatePattern('\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z')]
    [string]$Version
)
$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw '未找到 Visual Studio C++ 工具链' }
$vsVersion = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
# GitHub windows-2022 使用 VS 2022；本机仍可使用 VS 2026，不依赖单一机器上的生成器版本。
$generator = switch (([version]$vsVersion).Major) {
    17 { 'Visual Studio 17 2022' }
    18 { 'Visual Studio 18 2026' }
    default { throw "未适配的 Visual Studio 版本：$vsVersion" }
}
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$testing = if ($SkipTests) { 'OFF' } else { 'ON' }
$versionArgument = if ($Version) { "-DREF_IME_VERSION=$Version" } else { '-UREF_IME_VERSION' }
& $cmake -S $pluginRoot -B (Join-Path $pluginRoot 'build') -G $generator -A x64 "-DBUILD_TESTING=$testing" $versionArgument
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败' }
& $cmake --build (Join-Path $pluginRoot 'build') --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw '构建失败' }
if (-not $SkipTests) {
    & $ctest --test-dir (Join-Path $pluginRoot 'build') -C Release --output-on-failure --timeout 20
    if ($LASTEXITCODE -ne 0) { throw '独立测试未通过' }
}
$distRoot = Join-Path $pluginRoot 'dist'
New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $pluginRoot 'build\Release\REF-IME.dll') -Destination (Join-Path $distRoot 'REF-IME.dll') -Force
Write-Output (Join-Path $distRoot 'REF-IME.dll')

