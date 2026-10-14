param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force -Path 'build/support' | Out-Null
    & $Compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-Wpedantic' '-static' '-municode' '-mwindows' '-finput-charset=UTF-8' '-ffile-prefix-map=.=.' '-Wl,--no-insert-timestamp' '-Wl,--strip-all' 'src/main.cpp' '-o' 'build/Thermal.exe' '-ladvapi32' '-luser32' '-lgdi32' '-lcomdlg32' '-lshell32' '-lole32' '-loleaut32' '-lwbemuuid' '-lpdh' '-ldxgi' '-luuid'
    if ($LASTEXITCODE -ne 0) { throw 'C++ compilation failed.' }
    & $Compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-static' '-municode' '-mwindows' '-Wl,--no-insert-timestamp' '-Wl,--strip-all' 'src/cpu_service.cpp' '-o' 'build/support/ThermalOverlayCpuService.exe' '-ladvapi32' '-luser32'
    if ($LASTEXITCODE -ne 0) { throw 'CPU service compilation failed.' }
    New-Item -ItemType Directory -Force -Path 'build/support/modules','build/support/cpu-support' | Out-Null
    Copy-Item 'third_party/pawnio/modules/*' 'build/support/modules/' -Force
    Copy-Item 'setup-cpu.ps1' 'build/support/setup-cpu.ps1' -Force
    Copy-Item 'third_party/pawnio/source' 'build/support/cpu-support/' -Recurse -Force
    Copy-Item 'third_party/pawnio/README.md' 'build/support/cpu-support/THIRD-PARTY.md' -Force
    if (!(Test-Path 'build/support/cpu-support/PawnIO_setup.exe')) {
        Invoke-WebRequest 'https://github.com/namazso/PawnIO.Setup/releases/download/2.2.0/PawnIO_setup.exe' -OutFile 'build/support/cpu-support/PawnIO_setup.exe'
    }
    if ((Get-FileHash 'build/support/cpu-support/PawnIO_setup.exe' -Algorithm SHA256).Hash -ne '1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032') { throw 'PawnIO installer checksum mismatch.' }
    Write-Host 'Built: build/Thermal.exe and CPU support package.'
} finally { Pop-Location }
