@echo off
setlocal enabledelayedexpansion
set FAIL=0
for /l %%i in (1,1,%1) do (
  call "C:\drvlite\run-cli.cmd" shutdown >nul 2>&1 || set FAIL=1
  call "C:\drvlite\unmap.cmd" >nul 2>&1
)
if %FAIL%==1 (echo cycle-test: %1 cycles with failures & exit /b 1)
echo cycle-test: %1 cycles, 0 failures
