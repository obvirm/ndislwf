@echo off
echo === smoke ===
C:\ndislwf\bin\nlstress.exe version > C:\ndislwf2\smoke.txt 2>&1
echo vret %ERRORLEVEL% >> C:\ndislwf2\smoke.txt
C:\ndislwf\bin\nlstress.exe enum >> C:\ndislwf2\smoke.txt 2>&1
echo eret %ERRORLEVEL% >> C:\ndislwf2\smoke.txt
exit /b 0
