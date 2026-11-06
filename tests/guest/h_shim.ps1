$ErrorActionPreference = 'Stop'
Copy-Item C:\ndislwg\ndisapi.dll C:\ndislwf\ndisapi.dll -Force
Copy-Item C:\ndislwg\shimprobe.exe C:\ndislwf\shimprobe.exe -Force
$hashes = ""
foreach ($f in @('C:\ndislwf\nlbridge.dll','C:\ndislwf\ndisapi.dll','C:\ndislwf\shimprobe.exe')) {
    $h = (Get-FileHash $f -Algorithm SHA256).Hash.Substring(0,8)
    $hashes += "$(Split-Path -Leaf $f)=$h`r`n"
}
$hashes | Out-File C:\ndislwf\h_shim_out.txt -Encoding ascii
Set-Location C:\ndislwf
& C:\ndislwf\shimprobe.exe *>&1 | Out-File C:\ndislwf\shim_t1.txt -Encoding ascii
"run done $(Get-Date -Format o)" | Out-File C:\ndislwf\h_shim_done.txt -Encoding ascii
