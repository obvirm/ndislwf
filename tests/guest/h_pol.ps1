$ErrorActionPreference = 'Continue'
Set-Location C:\ndislwf
& C:\ndislwf\h_re.cmd
& C:\ndislwf\nlstress.exe fuzzval 8 *>&1 | Out-File C:\ndislwf\fv_t1.txt -Encoding ascii
& C:\ndislwf\nlstress.exe policy *>&1 | Out-File C:\ndislwf\polB.txt -Encoding ascii
"val-only done $(Get-Date -Format o)" | Out-File C:\ndislwf\h_pol_done.txt -Encoding ascii
