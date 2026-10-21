@echo off
set DRVLITE_ALLOW_KERNEL_LIVE=1
"C:\drvlite\drvlite-cli.exe" cycle-test --allow-kernel-live --image C:\drvlite\drvlite.sys --cycles %1
