$ErrorActionPreference = 'Stop'
Copy-Item C:\ndislwg\nlbridge.dll C:\ndislwf\nlbridge.dll -Force
Copy-Item C:\ndislwg\nlstress.exe C:\ndislwf\nlstress.exe -Force
foreach ($f in @('C:\ndislwf\nlbridge.dll','C:\ndislwf\nlstress.exe','C:\ndislwf\bin\ndislwf.sys')) {
    $h = (Get-FileHash $f -Algorithm SHA256).Hash.Substring(0,8)
    "$(Split-Path -Leaf $f)=$h" | Out-File C:\ndislwf\h_fuzz_out.txt -Append -Encoding ascii
}
Set-Location C:\ndislwf
& C:\ndislwf\nlstress.exe fuzz 8 *>&1 | Out-File C:\ndislwf\fuzz_t1.txt -Encoding ascii
"run done $(Get-Date -Format o)" | Out-File C:\ndislwf\h_fuzz_done.txt -Encoding ascii
