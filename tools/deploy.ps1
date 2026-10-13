param([Parameter(Mandatory = $true)][string]$GameRoot, [switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path -Parent $PSScriptRoot
$gameDirectory = [IO.Path]::GetFullPath($GameRoot).TrimEnd('\')
if (-not (Test-Path -LiteralPath $gameDirectory -PathType Container)) { throw '游戏目录不存在' }
. (Join-Path $PSScriptRoot 'pe-exports.ps1')
$hostPath = Join-Path $gameDirectory 'dinput8.dll'
if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf)) { throw '请先在游戏目录安装 REFramework' }
$preflight = Test-REFIMEHostExports $hostPath (Join-Path $pluginRoot 'compatibility.json')
if (-not $preflight.CompatibleExports) { throw ('当前 REF 缺少所需导出，请升级兼容版本：' + ($preflight.Missing -join ', ')) }
if ($CheckOnly) { Write-Output 'Windows x64 和所需导出预检通过；启动时仍检查 ImGui 1.92.0/WCHAR32 布局。'; return }
# 通过可执行路径检查任意目标游戏及其目录中的辅助进程，不维护容易遗漏新游戏的进程名白名单。
$running = @(Get-Process | Where-Object {
    $executablePath = $_.Path
    $executablePath -and $executablePath.StartsWith($gameDirectory + '\', [StringComparison]::OrdinalIgnoreCase)
})
if ($running.Count) { throw ('游戏目录中仍有运行进程，请手动退出后再部署：' + (($running | ForEach-Object ProcessName) -join ', ')) }
$source = Join-Path $pluginRoot 'dist\REF-IME.dll'
if (-not (Test-Path -LiteralPath $source)) { $source = Join-Path $pluginRoot 'reframework\plugins\REF-IME.dll' }
$targetDirectory = Join-Path $gameDirectory 'reframework\plugins'
$target = Join-Path $targetDirectory 'REF-IME.dll'
if (-not (Test-Path -LiteralPath $source)) { throw '请先构建 DLL。' }
if ([IO.Path]::GetFullPath($source).Equals($target, [StringComparison]::OrdinalIgnoreCase)) { throw '请从构建目录或解压包目录运行部署，不从已安装目录部署到自身' }
New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
Copy-Item -LiteralPath $source -Destination $target -Force
if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $target).Hash) { throw '部署哈希不一致' }
Write-Output "已部署：$target"
