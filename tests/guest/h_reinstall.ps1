$Rep = "C:\ndislwf\h_reinstall_out.txt"
function Rep([string]$s) { Add-Content -Path $Rep -Value $s -Encoding UTF8 }
Set-Content -Path $Rep -Value ("start " + (Get-Date -Format o)) -Encoding UTF8

Rep "=== 1. hapus paket oem (unbind dulu!) ==="
$enum = (pnputil -e | Out-String)
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
(netcfg -v -u MS_NdisLwf 2>&1 | Out-String) | ForEach-Object { Rep $_ }
sc.exe stop NDISLWF | Out-Null
Start-Sleep 3
sc.exe delete NDISLWF 2>&1 | Out-String | ForEach-Object { Rep $_ }
Start-Sleep 5

Rep "=== 2. copy sys BARU (harus bebas lock) ==="
try {
  Copy-Item C:\ndislwf\bin\ndislwf.sys C:\Windows\System32\DRIVERS\ndislwf.sys -Force -ErrorAction Stop
  Rep "copy OK"
} catch { Rep ("COPY GAGAL: " + $_.Exception.Message) }
$cs = (certutil -hashfile C:\Windows\System32\DRIVERS\ndislwf.sys SHA256 | Select-String -Pattern "^[0-9a-f ]+$").Line
Rep ("hash DRIVERS sys: " + $cs)

Rep "=== 3. install paket baru ==="
(pnputil /add-driver C:\ndislwf\bin\ndislwf.inf /install 2>&1 | Out-String) | ForEach-Object { Rep $_ }
(netcfg -v -l C:\ndislwf\bin\ndislwf.inf -c s -i MS_NdisLwf 2>&1 | Out-String) | ForEach-Object { Rep $_ }

Rep "=== 4. enable adapter + restart NIC ==="
try { Enable-NetAdapter -Name Ethernet0 -Confirm:$false | Out-Null; Rep "adapter enabled" } catch { Rep ("enable err: " + $_.Exception.Message) }
Start-Sleep 5
$nic = Get-PnpDevice -Class Net | Where-Object { $_.InstanceId -like "PCI\VEN_8086*" } | Select-Object -First 1
if ($nic) {
    try {
        $nic | Disable-PnpDevice -Confirm:$false -ErrorAction Stop
        Start-Sleep 4
        $nic | Enable-PnpDevice -Confirm:$false -ErrorAction Stop
        Start-Sleep 8
        $nic2 = Get-PnpDevice -InstanceId $nic.InstanceId
        Rep ("NIC setelah restart: status=" + $nic2.Status)
    } catch { Rep ("restart NIC err: " + $_.Exception.Message) }
}
Start-Sleep 5

Rep "=== 5. state akhir ==="
Rep (sc.exe query NDISLWF | Out-String).Trim()
Rep (Get-NetAdapterBinding -Name Ethernet0 | Where-Object ComponentID -eq MS_NdisLwf | Format-Table -AutoSize | Out-String)
$cs2 = (certutil -hashfile C:\Windows\System32\DRIVERS\ndislwf.sys SHA256 | Select-String -Pattern "^[0-9a-f ]+$").Line
Rep ("hash DRIVERS sys final: " + $cs2)
Rep "done"
exit 0
