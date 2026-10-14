param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
Push-Location $projectRoot
try {
    & $Compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-Wpedantic' '-static' '-municode' '-mwindows' '-finput-charset=UTF-8' '-ffile-prefix-map=.=.' '-Wl,--no-insert-timestamp' '-Wl,--strip-all' 'src/main.cpp' '-o' 'build/ThermalOverlay.exe' '-ladvapi32' '-luser32' '-lgdi32' '-lcomdlg32' '-lshell32' '-lole32' '-loleaut32' '-lwbemuuid' '-lpdh' '-ldxgi' '-luuid'
    if ($LASTEXITCODE -ne 0) { throw 'C++ compilation failed.' }
    Write-Host 'Built: build/ThermalOverlay.exe'
} finally { Pop-Location }
