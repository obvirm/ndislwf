$Rep = "C:\ndislwf\f10_stress_out.txt"
function Rep([string]$s) { Add-Content -Path $Rep -Value $s -Encoding UTF8 }
Set-Content -Path $Rep -Value ("start " + (Get-Date -Format o)) -Encoding UTF8

$gw = (Get-NetRoute -DestinationPrefix 0.0.0.0/0 | Select-Object -First 1).NextHop
Rep ("gateway=" + $gw)

$pg = Start-Process -FilePath ping.exe -ArgumentList "-t","-l","1000",$gw -WindowStyle Hidden -PassThru
Start-Sleep 1

Set-Location C:\ndislwf\bin

Rep "=== nlstress chaos 60s (lanjutan dari full 20s) ==="
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath .\nlstress.exe -ArgumentList "chaos","60" -WorkingDirectory C:\ndislwf -Wait -PassThru -NoNewWindow -RedirectStandardOutput C:\ndislwf\f10_chaos.txt 2>$null
Get-Content C:\ndislwf\f10_chaos.txt | ForEach-Object { Rep $_ }
Rep ("chaos exit=" + $p.ExitCode + " dur=" + $sw.Elapsed.TotalSeconds.ToString("0.0") + "s")

Rep "=== inject benchmark 8000 ==="
$p = Start-Process -FilePath .\nlstress.exe -ArgumentList "inject","8000" -WorkingDirectory C:\ndislwf -Wait -PassThru -NoNewWindow -RedirectStandardOutput C:\ndislwf\f10_inj.txt 2>$null
Get-Content C:\ndislwf\f10_inj.txt | ForEach-Object { Rep $_ }
Rep ("inject exit=" + $p.ExitCode)

if ($pg -and -not $pg.HasExited) { Stop-Process -Id $pg.Id -Force }
Start-Sleep 1

Rep "=== stats akhir ==="
(& .\nlstress.exe stats 2>&1 | Out-String) | ForEach-Object { Rep $_ }
Rep "done"
exit 0
