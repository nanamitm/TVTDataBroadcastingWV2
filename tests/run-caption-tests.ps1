param([ValidateSet('x64', 'Win32')][string]$Platform = 'x64')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $repo "work/caption-tests-$Platform"
New-Item -ItemType Directory -Force $output | Out-Null
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools are required' }
$vcvars = Join-Path $vs $(if ($Platform -eq 'Win32') { 'VC/Auxiliary/Build/vcvars32.bat' } else { 'VC/Auxiliary/Build/vcvars64.bat' })
# Reuse the library objects from the Release build, but compile the product's
# NativeCaptionRenderer.cpp in the test TU so its retained state is inspectable.
[xml]$project = Get-Content (Join-Path $repo 'TVTDataBroadcastingWV2/TVTDataBroadcastingWV2.vcxproj')
$nodes = Select-Xml -Xml $project -Namespace @{p='http://schemas.microsoft.com/developer/msbuild/2003'} -XPath '//p:ClCompile'
$library = $nodes | Where-Object { $_.Node.Include -like 'thirdparty\libaribcaption\src\base\*' }
$intermediate = if ($Platform -eq 'Win32') { 'Release' } else { "$Platform/Release" }
$objects = foreach ($source in ($library.Node.Include -split ';')) {
    $object = Join-Path $repo ("TVTDataBroadcastingWV2/$intermediate/" + [IO.Path]::GetFileNameWithoutExtension($source) + '.obj')
    if (-not (Test-Path -LiteralPath $object)) { throw "Build the $Platform Release solution before caption tests: $object" }
    '"' + $object + '"'
}
if ($objects.Count -lt 15) { throw 'libaribcaption object list not found in project' }
$response = Join-Path $output 'link.rsp'
@('"' + (Join-Path $output 'caption.obj') + '"') + $objects + @('d2d1.lib', 'dwrite.lib', 'windowscodecs.lib', 'ole32.lib', 'gdi32.lib', 'user32.lib', '/LTCG', ('/OUT:"' + (Join-Path $output 'caption.exe') + '"')) | Set-Content -LiteralPath $response -Encoding ascii
$batch = Join-Path $output 'run.cmd'
@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$repo"
cl /nologo /c /EHsc /MD /std:c++20 /utf-8 /I"TVTDataBroadcastingWV2\thirdparty\libaribcaption-config" /I"TVTDataBroadcastingWV2\thirdparty\libaribcaption\include" /I"TVTDataBroadcastingWV2\thirdparty\libaribcaption\src" /I"packages\Microsoft.Windows.ImplementationLibrary.1.0.220201.1\include" /I"packages\Microsoft.Web.WebView2.1.0.4258.31\build\native\include" tests\NativeCaptionRendererTests.cpp /Fo:"$output\caption.obj"
if errorlevel 1 exit /b 1
link /nologo @"$response"
if errorlevel 1 exit /b 1
"$output\caption.exe"
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Native caption tests failed' }
