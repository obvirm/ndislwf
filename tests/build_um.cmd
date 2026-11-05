@echo off
rem Env kompilasi user-mode: SDK + MSVC dibundel EWDK (SetupBuildEnv tidak
rem memasang INCLUDE karena -winsdk=none). Dipakai: build_um <args-cl>
call F:\BuildEnv\SetupBuildEnv.cmd amd64 >nul 2>&1
setlocal enabledelayedexpansion
set "KITINC=%WindowsSdkDir%Include\%Version_Number%"
set "KITLIB=%WindowsSdkDir%Lib\%Version_Number%"
set "MSVCBIN="
for /f "delims=" %%C in ('where cl 2^>nul') do if not defined MSVCBIN set "MSVCBIN=%%C"
if "!MSVCBIN!"=="" (
    echo build_um: cl tidak ketemu 1>&2
    endlocal
    exit /b 2
)
rem ...\VC\Tools\MSVC\14.xx\bin\Hostx64\x64\cl.exe -> root = MSVC\14.xx
for %%R in ("!MSVCBIN!") do set "P=%%~dpR"
if "!P:~-1!"=="\" set "P=!P:~0,-1!"
for %%A in ("!P!\..\..\..") do set "MSVCROOT=%%~fA"
set "INC=!KITINC!\ucrt;!KITINC!\um;!KITINC!\shared;!KITINC!\winrt;!MSVCROOT!\include"
set "LNP=!KITLIB!\ucrt\x64;!KITLIB!\um\x64;!MSVCROOT!\lib\x64"
endlocal & set "INCLUDE=%INC%" & set "LIB=%LNP%"
echo build_um: INCLUDE/LIB dari EWDK siap
cl %*
goto :eof
