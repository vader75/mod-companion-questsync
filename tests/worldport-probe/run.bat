@echo off
setlocal
cd /d "%~dp0"
call "D:\Applications\VisualStudio2022BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 3
cl /nologo /EHsc /std:c++20 probe.cpp
if errorlevel 1 exit /b 4
probe.exe
exit /b %errorlevel%
