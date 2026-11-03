# uninstall.ps1 -- pembongkaran driver ndislwf (NDIS 6 LWF).
# Jalankan sebagai administrator.
param(
    [string]$InfPath = "$PSScriptRoot\ndislwf.inf"
)

$ErrorActionPreference = "Continue"
function Info([string]$s) { Write-Host $s }

if (-not [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        .IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "butuh administrator"
}

Info "1. lepas binding adapter"
Get-NetAdapterBinding -ComponentID MS_NdisLwf -EA SilentlyContinue |
    Where-Object Enabled | ForEach-Object {
        Disable-NetAdapterBinding -Name $_.Name -ComponentID MS_NdisLwf | Out-Null
    }

Info "2. stop + hapus layanan"
sc.exe stop NDISLWF 2>$null | Out-Null
Start-Sleep 2
sc.exe delete NDISLWF 2>$null | Out-Null

Info "3. uninstall komponen"
netcfg -v -u MS_NdisLwf 2>$null | Out-Null

Info "4. hapus paket driver"
$enum = pnputil -e | Out-String
[regex]::Matches($enum, "oem\d+\.inf[^\r\n]*") | ForEach-Object {
    if ($_.Value -match "ndislwf\.inf") {
        $pn = [regex]::Match($_.Value, "oem\d+\.inf").Value
        if ($pn) { pnputil -f -d $pn | Out-Null }
    }
}

Info "5. hapus biner"
Remove-Item "$env:windir\System32\DRIVERS\ndislwf.sys" -EA SilentlyContinue

Info "selesai"
