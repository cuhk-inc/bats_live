@echo off
cd /d "%~dp0"
..\bats\tools\tcc\tcc.exe -std=c11 -I..\bats -o bats_live.exe bats_live.c ..\bats\field.c ..\bats\source.c ..\bats\dest.c ..\bats\codec.c -lws2_32
exit /b %ERRORLEVEL%
