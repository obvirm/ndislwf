$ErrorActionPreference = 'Stop'
$log = 'C:\ndislwf\h_m5copy_out.txt'
"start m5copy $(Get-Date -Format o)" | Out-File $log -Encoding ascii
function Cpy($a, $b) {
    Copy-Item $a $b -Force
}
Cpy C:\ndislwg\zbinslw.sys C:\ndislwf\bin\ndislwf.sys
Cpy C:\ndislwg\zbinslw.sys C:\ndislwf\NDISLWF.sys
Cpy C:\ndislwg\zbooins.inf C:\ndislwf\bin\ndislwf.inf
Cpy C:\ndislwg\zboufns.cat C:\ndislwf\bin\ndislwf.cat
Cpy C:\ndislwg\zcislnbr.dll C:\ndislwf\nlbridge.dll
Cpy C:\ndislwg\zdeislnx.exe C:\ndislwf\nlstress.exe
Get-FileHash -Algorithm SHA256 `
    C:\ndislwg\zbinslw.sys,
    C:\ndislwf\bin\ndislwf.sys,
    C:\ndislwg\zcislnbr.dll,
    C:\ndislwf\nlbridge.dll,
    C:\ndislwg\zdeislnx.exe,
    C:\ndislwf\nlstress.exe |
    ForEach-Object { "{0} {1}" -f $_.Hash.Substring(0, 8), $_.Path } |
    Out-File $log -Append -Encoding ascii
"done $(Get-Date -Format o)" | Out-File $log -Append -Encoding ascii
