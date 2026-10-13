param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('(?-i)\Av(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z')]
    [string]$Tag
)
$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path -Parent $PSScriptRoot
$distRoot = Join-Path $pluginRoot 'dist'
$dllPath = Join-Path $distRoot 'REF-IME.dll'
if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) { throw '请先构建 dist/REF-IME.dll' }
$archivePath = Join-Path $distRoot ("REF-IME-$Tag-windows-x64.zip")
$entryPath = 'reframework/plugins/REF-IME.dll'

# 直接写入唯一文件条目，不遍历项目/暂存目录，避免附带文档、配置、测试或旧产物。
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stream = [IO.File]::Open($archivePath, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $dllPath, $entryPath, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    } finally { $zip.Dispose() }
} finally { $stream.Dispose() }

# 构建和本地使用共用校验：ZIP 只能有一个 DLL 条目，解压后的字节必须与构建结果一致。
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    if ($zip.Entries.Count -ne 1 -or $zip.Entries[0].FullName -cne $entryPath) { throw 'ZIP 内容或目录层级不符合要求' }
    $entryStream = $zip.Entries[0].Open()
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $entryHash = [BitConverter]::ToString($sha.ComputeHash($entryStream)).Replace('-', '')
    } finally { $sha.Dispose(); $entryStream.Dispose() }
    if ($entryHash -cne (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash) { throw 'ZIP 内 DLL 与构建文件哈希不一致' }
} finally { $zip.Dispose() }
Write-Output $archivePath
Get-FileHash -LiteralPath $archivePath -Algorithm SHA256
