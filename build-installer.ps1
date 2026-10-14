param([string]$Iscc = 'ISCC.exe')
$ErrorActionPreference = 'Stop'
$version = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION must contain major.minor.patch.' }
$required = @('Thermal.exe','support\ThermalOverlayCpuService.exe','support\setup-cpu.ps1','support\modules\IntelMSR.bin','support\modules\AMDFamily17.bin','support\modules\COPYING','support\cpu-support\PawnIO_setup.exe','support\cpu-support\source\PawnIO.Modules-0.2.11.tar.gz')
foreach ($relative in $required) {
    if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot "build\$relative"))) { throw "Missing build component: $relative. Run build.ps1 first." }
}
$driver = Join-Path $PSScriptRoot 'build\support\cpu-support\PawnIO_setup.exe'
if ((Get-FileHash -LiteralPath $driver -Algorithm SHA256).Hash -ne '1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032') { throw 'PawnIO checksum mismatch.' }
if ((Get-AuthenticodeSignature -LiteralPath $driver).Status -ne 'Valid') { throw 'PawnIO signature is invalid.' }
& $Iscc "/DAppVersion=$version" (Join-Path $PSScriptRoot 'installer\Thermal.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
$filename = "Thermal-Setup-$version-x64.exe"
$output = Join-Path $PSScriptRoot "build\release\$filename"
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$output.sha256", "$hash  $filename`n", [Text.Encoding]::ASCII)
Write-Host "Built: $output"
