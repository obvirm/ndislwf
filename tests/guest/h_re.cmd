@echo off
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ndislwf\h_reinstall.ps1 > C:\ndislwf\h_re_console.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf\h_re_console.txt
