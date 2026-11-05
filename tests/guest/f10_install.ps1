$Rep = "C:\ndislwf\f10_install_out.txt"
function Rep([string]$s) { Add-Content -Path $Rep -Value $s -Encoding UTF8 }
Set-Content -Path $Rep -Value ("start " + (Get-Date -Format o)) -Encoding UTF8

Rep "=== -1. cek paket di C:\ndislwf\bin ==="
if (-not (Test-Path C:\ndislwf\bin\ndislwf.sys)) { Rep "ndislwf.sys TIDAK ada -- hentikan"; exit 1 }
Get-ChildItem C:\ndislwf\bin | ForEach-Object { Rep ("bin: " + $_.Name + " " + $_.Length) }

Rep "=== 0. testsigning? ==="
$bde = (bcdedit /enum "{current}" | Out-String)
if ($bde -match "testsigning\s+Yes") { Rep "testsigning=Yes" } else { Rep "testsigning=TIDAK aktif (hentikan)"; Rep $bde; exit 1 }

Rep "=== 1. disable adapter ==="
try { Disable-NetAdapter -Name Ethernet0 -Confirm:$false | Out-Null; Rep "adapter disabled" } catch { Rep ("disable err: " + $_.Exception.Message) }
Start-Sleep 3

Rep "=== 2. stop + delete service ==="
sc.exe stop NDISLWF | Out-Null
Start-Sleep 2
sc.exe delete NDISLWF 2>&1 | Out-String | ForEach-Object { Rep $_ }

Rep "=== 3. netcfg -u komponen ==="
(netcfg -v -u MS_NdisLwf 2>&1 | Out-String) | ForEach-Object { Rep $_ }

Rep "=== 4. hapus paket oem lama (ndislwf.inf) ==="
$enum = (pnputil /enum-drivers | Out-String)
$blocks = $enum -split "(?=\r?\nPublished Name)"
foreach ($blk in $blocks) {
    if ($blk -match "ndislwf\.inf") {
        $m = [regex]::Match($blk, "oem\d+\.inf")
        if ($m.Success) {
            Rep ("pnputil /delete-driver " + $m.Value + " /uninstall /force")
            (pnputil /delete-driver $m.Value /uninstall /force 2>&1 | Out-String) | ForEach-Object { Rep $_ }
        }
    }
}

Rep "=== 5. install paket baru ==="
(pnputil /add-driver C:\ndislwf\bin\ndislwf.inf /install 2>&1 | Out-String) | ForEach-Object { Rep $_ }

Rep "=== 6. salin .sys ==="
Copy-Item C:\ndislwf\bin\ndislwf.sys C:\Windows\System32\DRIVERS\ndislwf.sys -Force
Get-Item C:\Windows\System32\DRIVERS\ndislwf.sys | ForEach-Object { Rep ("sys size: " + $_.Length) }

Rep "=== 7. netcfg -l ==="
(netcfg -v -l C:\ndislwf\bin\ndislwf.inf -c s -i MS_NdisLwf 2>&1 | Out-String) | ForEach-Object { Rep $_ }

Rep "=== 8. enable adapter ==="
try { Enable-NetAdapter -Name Ethernet0 -Confirm:$false | Out-Null; Rep "adapter enabled" } catch { Rep ("enable err: " + $_.Exception.Message) }
Start-Sleep 10

Rep "=== 9. restart NIC PnP device ==="
$nic = Get-PnpDevice -Class Net | Where-Object { $_.InstanceId -like "PCI\VEN_8086*" } | Select-Object -First 1
if ($nic) {
    Rep ("NIC: " + $nic.FriendlyName + " status=" + $nic.Status)
    try {
        $nic | Disable-PnpDevice -Confirm:$false -ErrorAction Stop
        Start-Sleep 4
        $nic | Enable-PnpDevice -Confirm:$false -ErrorAction Stop
        Start-Sleep 8
        $nic2 = Get-PnpDevice -InstanceId $nic.InstanceId
        Rep ("NIC setelah restart: status=" + $nic2.Status)
    } catch { Rep ("restart NIC err: " + $_.Exception.Message) }
} else { Rep "NIC Intel tidak ditemukan" }
Start-Sleep 10

Rep "=== 10. state akhir ==="
Rep (sc.exe query NDISLWF | Out-String).Trim()
Rep (Get-NetAdapterBinding -Name Ethernet0 | Where-Object ComponentID -eq MS_NdisLwf | Format-Table -AutoSize | Out-String)
Rep "=== 11. smoke nlstress ==="
Set-Location C:\ndislwf\bin
$env:PATH = "C:\ndislwf\bin;" + $env:PATH
(& .\nlstress.exe version 2>&1 | Out-String) | ForEach-Object { Rep $_ }
(& .\nlstress.exe enum 2>&1 | Out-String) | ForEach-Object { Rep $_ }
Rep "done"
exit 0
