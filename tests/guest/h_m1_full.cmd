@echo off
C:\ndislwf\bin\nlstress.exe full > C:\ndislwf\o_full_m1.txt 2>&1
echo FULL exit=%errorlevel% >> C:\ndislwf\o_full_m1.txt
exit /b 0
