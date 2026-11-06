$ErrorActionPreference = 'Continue'
Set-Location C:\ndislwf
& C:\ndislwf\nlstress.exe mirror *>&1 | Out-File C:\ndislwf\mir_t1.txt -Encoding ascii
& C:\ndislwf\nlstress.exe full *>&1 | Out-File C:\ndislwf\full_t5.txt -Encoding ascii
"run done $(Get-Date -Format o)" | Out-File C:\ndislwf\h_mrun_out.txt -Encoding ascii
