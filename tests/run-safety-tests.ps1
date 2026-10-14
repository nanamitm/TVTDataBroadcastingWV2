$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $repo 'work/safety-tests'
New-Item -ItemType Directory -Force $output | Out-Null
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools are required' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$batch = Join-Path $output 'run.cmd'
@"
@echo off
set "PATH=$(Split-Path $vswhere);%PATH%"
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$repo"
cl /nologo /EHsc /MD /std:c++20 /utf-8 /DUNICODE /D_UNICODE /I"packages\Microsoft.Windows.ImplementationLibrary.1.0.220201.1\include" /I"packages\Microsoft.Web.WebView2.1.0.4258.31\build\native\include" tests\ProxySafetyTests\ProxySafetyTests.cpp /Fo:"$output\proxy.obj" /Fe:"$output\proxy.exe" winhttp.lib ws2_32.lib
if errorlevel 1 exit /b 1
"$output\proxy.exe"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /MD /std:c++20 /I"TVTDataBroadcastingWV2" tests\StartupSmokeTests\StartupSmokeTests.cpp /Fo:"$output\startup.obj" /Fe:"$output\startup.exe"
if errorlevel 1 exit /b 1
"$output\startup.exe"
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Native safety tests failed' }
