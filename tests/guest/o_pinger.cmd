@echo off
start "pinger" cmd /c "ping -t -l 1000 192.168.204.2 > C:\ndislwf2\ping.txt 2>&1"
exit /b 0
