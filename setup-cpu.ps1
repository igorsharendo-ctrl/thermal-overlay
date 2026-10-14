#Requires -RunAsAdministrator
param([switch]$Managed)
$failureCode = 10
$ErrorActionPreference = 'Stop'
try {
    $driverInstalled = Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO'
    if (-not $driverInstalled) {
        $installer = Join-Path $PSScriptRoot 'cpu-support\PawnIO_setup.exe'
        if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne '1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032') { throw 'Unexpected PawnIO installer. Rebuild the complete CPU support package.' }
        if ((Get-AuthenticodeSignature -LiteralPath $installer).Status -ne 'Valid') { throw 'PawnIO installer signature is not valid.' }
        $failureCode = 20
        $install = Start-Process -FilePath $installer -ArgumentList '-install','-silent' -Wait -PassThru -WindowStyle Hidden
        if ($install.ExitCode -eq 3010) { if (-not $Managed) { Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.MessageBox]::Show('Перезагрузите Windows и повторите настройку CPU.','ThermalOverlay') | Out-Null }; exit 3010 }
        if ($install.ExitCode -ne 0) { throw "PawnIO installation failed: $($install.ExitCode)" }
    }
    $failureCode = 30
    $service = Get-Service -Name ThermalOverlayCpu -ErrorAction SilentlyContinue
    if (-not $service) {
        $destination = Join-Path $env:ProgramFiles 'ThermalOverlay'
        if ((Test-Path $destination) -and ((Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Service directory must not be a junction or symbolic link.' }
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        $acl = New-Object System.Security.AccessControl.DirectorySecurity
        $acl.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)')
        Set-Acl -LiteralPath $destination -AclObject $acl
        $moduleDir = Join-Path $destination 'modules'
        if ((Test-Path $moduleDir) -and ((Get-Item -LiteralPath $moduleDir).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Module directory must not be a junction or symbolic link.' }
        New-Item -ItemType Directory -Force -Path $moduleDir | Out-Null
        foreach ($name in @('ThermalOverlayCpuService.exe')) {
            $target = Join-Path $destination $name
            if ((Test-Path $target) -and ((Get-Item -LiteralPath $target).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Service file must not be a symbolic link.' }
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $target -Force
        }
        foreach ($name in @('IntelMSR.bin','AMDFamily17.bin','COPYING')) {
            $target = Join-Path $moduleDir $name
            if ((Test-Path $target) -and ((Get-Item -LiteralPath $target).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Module file must not be a symbolic link.' }
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot "modules\$name") -Destination $target -Force
        }
        New-Service -Name ThermalOverlayCpu -BinaryPathName ('"' + (Join-Path $destination 'ThermalOverlayCpuService.exe') + '"') -DisplayName 'ThermalOverlay CPU temperature' -StartupType Automatic -Description 'Read-only Intel/AMD CPU temperature monitoring via signed PawnIO modules. Publishes temperature readings locally; no network access.' | Out-Null
    }
    $service = Get-Service -Name ThermalOverlayCpu
    if ($service.Status -ne 'Running') { Start-Service -Name ThermalOverlayCpu }
    exit 0
} catch {
    if (-not $Managed) {
        Add-Type -AssemblyName System.Windows.Forms
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message,'ThermalOverlay: CPU setup','OK','Error') | Out-Null
    }
    exit $failureCode
}
