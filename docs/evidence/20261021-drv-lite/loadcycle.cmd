@echo off
setlocal
for /l %%i in (1,1,%1) do (
  sc stop Drvlite >nul 2>&1
  sc start Drvlite >nul 2>&1
)
echo loadcycle: %1 cycles complete
