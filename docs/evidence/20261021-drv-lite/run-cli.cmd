@echo off
"C:\drvlite\drvlite-cli.exe" run --allow-kernel-live --service Drvlite --image C:\drvlite\drvlite.sys --bootstrap C:\drvlite\boot.bin --map-cmd "C:\drvlite\start-drv.cmd" -- %*
