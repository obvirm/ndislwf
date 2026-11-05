@echo off
rem Suite benchmark o_seq -- dijalankan via vmrun runProgramInGuest tunggal.
rem Datang setelah paket C:\ndislwf\bin terpasang (f10_install).
del C:\ndislwf2\o_*.txt >nul 2>&1
C:\ndislwf\bin\nlstress.exe version > C:\ndislwf2\o_version.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_version.txt
C:\ndislwf\bin\nlstress.exe enum > C:\ndislwf2\o_enum.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_enum.txt
C:\ndislwf\bin\nlstress.exe stats > C:\ndislwf2\o_stats0.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_stats0.txt
C:\ndislwf\bin\nlstress.exe capture 5 > C:\ndislwf2\o_capture.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_capture.txt
C:\ndislwf\bin\nlstress.exe inject 4000 > C:\ndislwf2\o_inject.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_inject.txt
C:\ndislwf\bin\nlstress.exe policy > C:\ndislwf2\o_policy.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_policy.txt
C:\ndislwf\bin\nlstress.exe policy2 > C:\ndislwf2\o_policy2.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_policy2.txt
C:\ndislwf\bin\nlstress.exe shm 10 > C:\ndislwf2\o_shm.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_shm.txt
C:\ndislwf\bin\nlstress.exe chaos 20 > C:\ndislwf2\o_chaos.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_chaos.txt
C:\ndislwf\bin\nlstress.exe stats > C:\ndislwf2\o_stats1.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_stats1.txt
exit /b 0
