@echo off
set DRVLITE_ALLOW_KERNEL_LIVE=1
"C:\drvlite\drvlite-mapper-cli.exe" --allow-kernel-live --unmap %1
