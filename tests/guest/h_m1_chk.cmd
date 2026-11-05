@echo off
dir C:\ndislwf\bin > C:\ndislwf\lb_out.txt 2>&1
for %%F in (ndislwf.sys ndislwf.inf ndislwf.cat nlbridge.dll nlstress.exe) do cmd /c "echo HASH %%F >> C:\ndislwf\lb_out.txt & certutil -hashfile C:\ndislwf\bin\%%F SHA256 >> C:\ndislwf\lb_out.txt 2>&1"
cmd /c "echo HASH DRIVERS >> C:\ndislwf\lb_out.txt & certutil -hashfile C:\Windows\System32\DRIVERS\ndislwf.sys SHA256 >> C:\ndislwf\lb_out.txt 2>&1"
cmd /c "echo SC >> C:\ndislwf\lb_out.txt & sc query NDISLWF >> C:\ndislwf\lb_out.txt 2>&1"
exit /b 0
