@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File C:\ndislwf\f10_install.ps1 > C:\ndislwf\f10_install_console.txt 2>&1
echo err %ERRORLEVEL% >> C:\ndislwf\f10_install_console.txt
exit 0
