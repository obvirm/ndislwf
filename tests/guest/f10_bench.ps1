$Rep = "C:\ndislwf\f10_bench_out.txt"
function Rep([string]$s) { Add-Content -Path $Rep -Value $s -Encoding UTF8 }
Set-Content -Path $Rep -Value ("start " + (Get-Date -Format o)) -Encoding UTF8

$gw = (Get-NetRoute -DestinationPrefix 0.0.0.0/0 | Select-Object -First 1).NextHop
Rep ("gateway=" + $gw)

Rep "=== latar ping (TX+RX via NAT gateway) ==="
$pg = Start-Process -FilePath ping.exe -ArgumentList "-t","-l","1000",$gw -WindowStyle Hidden -PassThru
Start-Sleep 1

Set-Location C:\ndislwf\bin

Rep "=== nlstress full (capture/inject/policy/shm/chaos) ==="
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath .\nlstress.exe -ArgumentList "full" -WorkingDirectory C:\ndislwf -Wait -PassThru -NoNewWindow -RedirectStandardOutput C:\ndislwf\f10_full.txt 2>$null
Get-Content C:\ndislwf\f10_full.txt | ForEach-Object { Rep $_ }
Rep ("full exit=" + $p.ExitCode + " dur=" + $sw.Elapsed.TotalSeconds.ToString("0.0") + "s")

if ($pg -and -not $pg.HasExited) { Stop-Process -Id $pg.Id -Force }
Start-Sleep 1

Rep "=== stats akhir ==="
(& .\nlstress.exe stats 2>&1 | Out-String) | ForEach-Object { Rep $_ }
Rep "done"
exit 0
