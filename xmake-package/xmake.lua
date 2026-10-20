-- blook-drv-lite xmake package definition.
--
-- Consumed through blook-project/blook-xmake-repo:
--   add_repositories("blook https://github.com/blook-project/blook-xmake-repo.git")
--   add_requires("blook-drv-lite 0.1.0")
--   target("x"):add_packages("blook-drv-lite")
--
-- The release archive installs include/ (SDK headers), lib/ (drvlite-sdk.lib)
-- and the driver image. `scripts/publish-registry.ps1` fills in the sha256.
package("blook-drv-lite")
    set_homepage("https://github.com/blook-project/blook-drv-lite")
    set_description("Lite manual-map kernel driver + user-mode SDK")
    set_license("MIT")
    set_urls("https://github.com/blook-project/blook-drv-lite/releases/download/v$(version)/blook-drv-lite-$(version).zip")

    on_install("windows|x64", function (package)
        os.cp("include", package:installdir())
        os.cp("lib/drvlite-sdk.lib", package:installdir("lib"))
        os.cp("drvlite.sys", package:installdir("bin"))
        os.cp("drvlite-cli.exe", package:installdir("bin"))
        os.cp("xmake-package/xmake.lua", package:installdir())
    end)
