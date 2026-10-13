$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $pluginRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
$imguiRoot = Join-Path $pluginRoot '.deps\imgui'
$refRoot = Join-Path $pluginRoot '.deps\reframework'
New-Item -ItemType Directory -Path $imguiRoot,$refRoot -Force | Out-Null
foreach ($name in @('imgui.h','imgui.cpp','imgui_draw.cpp','imgui_widgets.cpp','imgui_tables.cpp','imgui_internal.h','imconfig.h','imstb_rectpack.h','imstb_textedit.h','imstb_truetype.h','LICENSE.txt')) {
    Invoke-WebRequest -Uri ('https://raw.githubusercontent.com/ocornut/imgui/' + $lock.imguiCommit + '/' + $name) -OutFile (Join-Path $imguiRoot $name) -TimeoutSec 30
}
Invoke-WebRequest -Uri ('https://raw.githubusercontent.com/praydog/REFramework/' + $lock.reframeworkCommit + '/include/reframework/API.h') -OutFile (Join-Path $refRoot 'API.h') -TimeoutSec 30
Invoke-WebRequest -Uri ('https://raw.githubusercontent.com/praydog/REFramework/' + $lock.reframeworkCommit + '/LICENSE') -OutFile (Join-Path $refRoot 'LICENSE.txt') -TimeoutSec 30

