@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File C:\ndislwf\f10_bench.ps1 > C:\ndislwf\f10_bench_console.txt 2>&1
echo err %ERRORLEVEL% >> C:\ndislwf\f10_bench_console.txt
exit 0
