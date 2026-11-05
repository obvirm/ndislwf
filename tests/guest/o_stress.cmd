@echo off
C:\ndislwf\bin\nlstress.exe chaos 60 > C:\ndislwf2\o_chaos60.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_chaos60.txt
C:\ndislwf\bin\nlstress.exe inject 8000 > C:\ndislwf2\o_inj8000.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_inj8000.txt
C:\ndislwf\bin\nlstress.exe stats > C:\ndislwf2\o_stats2.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_stats2.txt
exit /b 0
