@echo off
sc stop Drvlite >nul 2>&1
sc start Drvlite
if errorlevel 1 (
  sc stop Drvlite >nul 2>&1
  sc start Drvlite
)
