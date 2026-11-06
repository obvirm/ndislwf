$ErrorActionPreference = 'Continue'
Set-Location C:\ndislwf
Remove-Item C:\ndislwf\loop_ampl.txt -ErrorAction SilentlyContinue
for ($k = 1; $k -le 6; $k++) {
    "### siklus $k ###" | Out-File C:\ndislwf\loop_ampl.txt -Append -Encoding ascii
    & C:\ndislwf\nlstress.exe fuzz 8 *>&1 | Out-Null
    & C:\ndislwf\nlstress.exe policy *>&1 | Out-File C:\ndislwf\loop_ampl.txt -Append -Encoding ascii
    $f = Select-String -Path C:\ndislwf\loop_ampl.txt -Pattern "\[FAIL\]" | Select-Object -Last 1
    if ($f) { "FAIL DI SIKLUS $k" | Out-File C:\ndislwf\loop_ampl.txt -Append -Encoding ascii; break }
}
"done $(Get-Date -Format o)" | Out-File C:\ndislwf\h_loop_done.txt -Encoding ascii
