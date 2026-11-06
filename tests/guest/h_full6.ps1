$ErrorActionPreference = 'Continue'
Set-Location C:\ndislwf
& C:\ndislwf\nlstress.exe full *>&1 | Out-File C:\ndislwf\full_t6.txt -Encoding ascii
"run done $(Get-Date -Format o)" | Out-File C:\ndislwf\h_fuzz_done.txt -Encoding ascii
