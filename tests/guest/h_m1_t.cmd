@echo off
echo START M1 >> C:\ndislwf\m1_t.txt
C:\ndislwf\bin\nlstress.exe injr > C:\ndislwf\o_injrx.txt 2>&1
echo INJR exit=%errorlevel% >> C:\ndislwf\m1_t.txt
C:\ndislwf\bin\nlstress.exe hits > C:\ndislwf\o_hits.txt 2>&1
echo HITS exit=%errorlevel% >> C:\ndislwf\m1_t.txt
C:\ndislwf\bin\nlstress.exe policyrx > C:\ndislwf\o_policyrx.txt 2>&1
echo POLICYRX exit=%errorlevel% >> C:\ndislwf\m1_t.txt
exit /b 0
