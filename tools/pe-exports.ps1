# 只读取 PE 文件的机器类型与导出表，不加载或执行目标 DLL，避免部署前运行游戏初始化代码。
function Get-REFIMEPeExports([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    function Assert-ByteRange([long]$Offset, [long]$Length) {
        if ($Offset -lt 0 -or $Length -lt 0 -or $Offset -gt $bytes.LongLength - $Length) { throw 'PE 文件截断或偏移越界' }
    }
    function Read-UInt16([long]$Offset) {
        Assert-ByteRange $Offset 2
        return [BitConverter]::ToUInt16($bytes, [int]$Offset)
    }
    function Read-UInt32([long]$Offset) {
        Assert-ByteRange $Offset 4
        return [BitConverter]::ToUInt32($bytes, [int]$Offset)
    }
    if ((Read-UInt16 0) -ne 0x5a4d) { throw '不是 PE 文件' }
    $header = [long](Read-UInt32 0x3c)
    if ((Read-UInt32 $header) -ne 0x4550) { throw '无效 PE 签名' }
    $machine = Read-UInt16 ($header + 4)
    $sectionCount = Read-UInt16 ($header + 6)
    $optionalSize = Read-UInt16 ($header + 20)
    $optional = $header + 24
    Assert-ByteRange $optional $optionalSize
    if ($machine -ne 0x8664 -or (Read-UInt16 $optional) -ne 0x20b) { throw 'REF-IME 只支持 Windows x64 PE32+' }
    if ($optionalSize -lt 120 -or (Read-UInt32 ($optional + 108)) -lt 1) { throw 'PE 缺少导出目录' }
    $headerSize = [long](Read-UInt32 ($optional + 60))
    $sectionStart = $optional + $optionalSize
    Assert-ByteRange $sectionStart ($sectionCount * 40)
    $sections = @(for ($index = 0; $index -lt $sectionCount; $index++) {
        $entry = $sectionStart + $index * 40
        [pscustomobject]@{
            Address = [long](Read-UInt32 ($entry + 12))
            RawSize = [long](Read-UInt32 ($entry + 16))
            RawOffset = [long](Read-UInt32 ($entry + 20))
        }
    })
    function Convert-Rva([long]$Rva, [long]$Length = 1) {
        if ($Rva -lt $headerSize -and $Rva + $Length -le $headerSize) { Assert-ByteRange $Rva $Length; return $Rva }
        foreach ($section in $sections) {
            $delta = $Rva - $section.Address
            if ($delta -ge 0 -and $delta + $Length -le $section.RawSize) {
                $offset = $section.RawOffset + $delta
                Assert-ByteRange $offset $Length
                return $offset
            }
        }
        throw 'PE RVA 未映射到文件数据'
    }
    $exportRva = [long](Read-UInt32 ($optional + 112))
    $names = @()
    if ($exportRva) {
        $directory = Convert-Rva $exportRva 40
        $nameCount = [long](Read-UInt32 ($directory + 24))
        if ($nameCount -gt 1000000) { throw '异常 PE 导出数量' }
        if ($nameCount) {
            $nameTable = Convert-Rva (Read-UInt32 ($directory + 32)) ($nameCount * 4)
            $names = @(for ($index = 0; $index -lt $nameCount; $index++) {
                $nameRva = [long](Read-UInt32 ($nameTable + $index * 4))
                $begin = Convert-Rva $nameRva
                $length = 0
                while ($length -lt 4096) {
                    $position = Convert-Rva ($nameRva + $length)
                    if ($position -ne $begin + $length) { throw 'PE 导出名称不连续' }
                    if ($bytes[$position] -eq 0) { break }
                    $length++
                }
                if ($length -eq 4096) { throw 'PE 导出名称未终止' }
                [Text.Encoding]::ASCII.GetString($bytes, [int]$begin, $length)
            })
        }
    }
    return [pscustomobject]@{ Machine = $machine; Exports = $names }
}

# 静态预检仅验证导出存在；真实 ImGui 版本/结构布局由插件初始化时验证，不执行 DLL 代码。
function Test-REFIMEHostExports([string]$Path, [string]$CompatibilityPath) {
    $profile = Get-Content -LiteralPath $CompatibilityPath -Raw | ConvertFrom-Json
    $pe = Get-REFIMEPeExports $Path
    $missing = @($profile.requiredExports | Where-Object { $_ -notin $pe.Exports })
    return [pscustomobject]@{ CompatibleExports = $missing.Count -eq 0; Missing = $missing; Machine = $pe.Machine }
}
