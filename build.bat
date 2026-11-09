@echo off
rem Wrapper so the build works even when PowerShell execution policy is Restricted.
rem Usage:  build.bat            (build only)
rem         build.bat -Flash     (build + flash + monitor, port COM69 by default)
rem         build.bat -Flash -Port COM12
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
