@echo off
certutil -hashfile C:\Windows\System32\drivers\NDISLWF.sys SHA256 > C:\ndislwf2\o_hash.txt 2>&1
echo ret %ERRORLEVEL% >> C:\ndislwf2\o_hash.txt
sc query ndislwf >> C:\ndislwf2\o_hash.txt 2>&1
sc qc ndislwf >> C:\ndislwf2\o_hash.txt 2>&1
