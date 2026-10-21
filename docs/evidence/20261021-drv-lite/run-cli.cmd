@echo off
set DRVLITE_ALLOW_KERNEL_LIVE=1
"C:\drvlite\drvlite-cli.exe" run --allow-kernel-live --image C:\drvlite\drvlite.sys --map-cmd "C:\drvlite\drvlite-mapper-cli.exe --allow-kernel-live --image {image} --bootstrap {bootstrap}" -- %*
