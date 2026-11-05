@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File C:\ndislwf\f10_stress.ps1 > C:\ndislwf\f10_stress_console.txt 2>&1
echo err %ERRORLEVEL% >> C:\ndislwf\f10_stress_console.txt
exit 0
