-- blook-drv-lite build script.
--
--   drvlite              kernel driver (.sys, C++17, manual-map loadable)
--   drvlite-sdk          user-mode SDK static library (C++20)
--   drvlite-cli          user-mode test CLI
--   drvlite-tests        host unit tests (gtest, no VM, no kernel)
--   drvlite-target       disposable guest process used by the e2e suite
--   drvlite-unsigned     deliberately unsigned guest driver (DSE e2e)
--
-- kchannel integration: pass --kchannel_dir=<checkout> or set KCHANNEL_DIR.
-- The library sources are compiled into each target. Until the blook-kchannel
-- implementation lands the build falls back to a development stub so the
-- repo (and CI) stay buildable; see scripts/prepare-kchannel.ps1.
set_project("blook-drv-lite")

-- The project version lives in version.txt (read by the release workflow).
-- xmake 3.0 sandboxes the project script and no longer exposes file I/O, so
-- keep this literal in sync; CI verifies the two match.
local version = "0.1.0"
set_version(version, {build = "%Y%m%d"})

add_requires("gtest 1.17.0")

set_arch("x64")
set_languages("cxx17")
set_defaultmode("releasedbg")
add_rules("mode.debug", "mode.releasedbg", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})

option("kchannel_dir")
    set_default("")
    set_showmenu(true)
    set_description("Path to the blook-kchannel checkout")
option_end()

option("wdk_dir")
    set_default("")
    set_showmenu(true)
    set_description("Path to a Windows Kits 10 root (forwarded to --wdk)")
option_end()

-- ---------------------------------------------------------------------------
-- MASM rule (same pattern as blook-drv: build .asm with ml64 through msvc).
-- Defined before the targets that use it.
-- ---------------------------------------------------------------------------
rule("blook.masm")
    set_extensions(".asm")
    on_build_file(function (target, sourcefile, opt)
        import("core.project.depend")
        import("core.tool.toolchain")
        local tc = toolchain.load("msvc", {plat = target:plat(), arch = target:arch()})
        local compiler = tc:tool("as")
        local objectfile = target:objectfile(sourcefile)
        table.insert(target:objectfiles(), objectfile)
        depend.on_changed(function ()
            os.mkdir(path.directory(objectfile))
            os.vrunv(compiler, {"/nologo", "/c", "/Fo" .. objectfile, sourcefile}, {envs = tc:runenvs()})
        end, {dependfile = target:dependfile(objectfile), files = {sourcefile}, changed = target:is_rebuilt()})
    end)
rule_end()

-- ---------------------------------------------------------------------------
-- Locate blook-kchannel (required for the driver/SDK targets; the host unit
-- tests are independent so a checkout without kchannel can still test core).
-- ---------------------------------------------------------------------------
local kchannel_dir = get_config("kchannel_dir")
if not kchannel_dir or kchannel_dir == "" then
    kchannel_dir = os.getenv("KCHANNEL_DIR")
end
if not kchannel_dir or kchannel_dir == "" then
    local candidate = path.join(os.projectdir(), "..", "blook-kchannel")
    if os.isdir(path.join(candidate, "include")) then
        kchannel_dir = candidate
    end
end
local kchannel_ok = kchannel_dir and kchannel_dir ~= ""
    and os.isdir(path.join(kchannel_dir, "include"))
if not kchannel_ok then
    print("blook-drv-lite: blook-kchannel not found; driver/SDK targets "
        .. "will not build. Pass --kchannel_dir=<checkout>.")
    kchannel_dir = ""
else
    print("blook-drv-lite: kchannel at " .. kchannel_dir)
end

local kc_include = kchannel_ok and path.join(kchannel_dir, "include") or ""
local kc_user_src = kchannel_ok and path.join(kchannel_dir, "src", "user") or ""
local kc_kernel_src = kchannel_ok and path.join(kchannel_dir, "src", "kernel") or ""
local kc_common_src = kchannel_ok and path.join(kchannel_dir, "src", "common") or ""
local kc_has_user = kchannel_ok and os.isdir(kc_user_src)
local kc_has_kernel = kchannel_ok and os.isdir(kc_kernel_src)
if kchannel_ok and (not kc_has_user or not kc_has_kernel) then
    print("blook-drv-lite: WARNING kchannel library sources are not present "
        .. "yet; building with the development stub (not for release)")
end

-- Forward our convenience --wdk_dir= option to xmake's built-in --wdk=.
local wdk_dir = get_config("wdk_dir")
if wdk_dir and wdk_dir ~= "" then
    set_config("wdk", wdk_dir)
end

local core_files = "src/core/*.cpp"
local common_includedirs = { "include", "src", "src/core" }

-- ---------------------------------------------------------------------------
-- Kernel driver (manual-map loadable)
-- ---------------------------------------------------------------------------
target("drvlite")
    add_rules("wdk.driver", "wdk.env.wdm", "blook.masm")
    set_values("wdk.env.winver", "win10_vb")
    set_languages("cxx17")
    set_exceptions("no-cxx")
    add_defines("NOMINMAX", "POOL_NX_OPTIN=1", "DRVLITE_VERSION=\"" .. version .. "\"")
    add_includedirs(common_includedirs)
    if kchannel_ok then
        add_includedirs(kc_include)
    end
    add_files("src/driver/*.cpp", "src/driver/**/*.cpp", core_files)
    add_files("src/driver/**/*.asm")
    if kc_has_kernel then
        add_files(path.join(kc_kernel_src, "*.cpp"))
        add_files(path.join(kc_common_src, "*.cpp"))
    else
        add_defines("DRVLITE_KC_STUB=1")
        add_files("src/driver/ipc/kc_stub.cpp")
    end
    add_cxxflags("/kernel", "/W4", "/GR-", "/EHs-c-", "/Zc:threadSafeInit-",
        "/GS-", "/utf-8", {force = true})
    add_syslinks("ntoskrnl", "hal", "wdmsec")
    add_ldflags("/INTEGRITYCHECK", "/ENTRY:DriverEntry", {force = true})
    set_encodings("utf8")

-- ---------------------------------------------------------------------------
-- User-mode SDK
-- ---------------------------------------------------------------------------
target("drvlite-sdk")
    set_kind("static")
    set_languages("cxx20")
    add_defines("NOMINMAX", "UNICODE", "_UNICODE", "DRVLITE_VERSION=\"" .. version .. "\"")
    add_includedirs(common_includedirs)
    if kchannel_ok then
        add_includedirs(kc_include)
    end
    add_files("src/sdk/*.cpp", core_files)
    if kc_has_user then
        add_files(path.join(kc_user_src, "*.cpp"))
        add_files(path.join(kc_common_src, "*.cpp"))
    else
        add_defines("DRVLITE_KC_STUB=1")
        add_files("src/sdk/kc_stub.cpp")
    end
    add_cxxflags("/utf-8", "/permissive-", "/W4", {tools = "cl"})
    add_syslinks("advapi32", "bcrypt")
    set_encodings("utf8")

-- ---------------------------------------------------------------------------
-- CLI
-- ---------------------------------------------------------------------------
target("drvlite-cli")
    set_kind("binary")
    set_languages("cxx20")
    add_defines("NOMINMAX", "UNICODE", "_UNICODE")
    add_includedirs({ "include", "src" })
    add_files("src/cli/*.cpp")
    add_deps("drvlite-sdk")
    add_cxxflags("/utf-8", "/permissive-", "/W4", {tools = "cl"})
    set_encodings("utf8")

-- ---------------------------------------------------------------------------
-- Host unit tests (no kernel, no VM)
-- ---------------------------------------------------------------------------
target("drvlite-tests")
    set_kind("binary")
    set_languages("cxx17")
    add_packages("gtest")
    add_defines("NOMINMAX", "DRVLITE_HOST_TESTS=1")
    add_includedirs(common_includedirs)
    add_files("tests/unit/*.cpp", core_files)
    add_cxxflags("/utf-8", "/permissive-", "/W4", {tools = "cl"})
    add_tests("unit")
    set_encodings("utf8")

-- ---------------------------------------------------------------------------
-- Guest artefacts for the e2e suite
-- ---------------------------------------------------------------------------
target("drvlite-target")
    set_kind("binary")
    set_languages("cxx20")
    add_defines("NOMINMAX", "UNICODE", "_UNICODE")
    add_includedirs({ "include", "src" })
    add_files("tests/guest/target/*.cpp")
    add_cxxflags("/utf-8", "/permissive-", "/W4", {tools = "cl"})
    set_encodings("utf8")

target("drvlite-unsigned")
    add_rules("wdk.driver", "wdk.env.wdm")
    set_values("wdk.env.winver", "win10_vb")
    set_languages("cxx17")
    set_exceptions("no-cxx")
    add_defines("NOMINMAX", "POOL_NX_OPTIN=1")
    add_files("tests/guest/unsigned/*.cpp")
    add_cxxflags("/kernel", "/W4", "/GR-", "/EHs-c-", "/utf-8", {force = true})
    add_syslinks("ntoskrnl", "hal", "wdmsec")
    add_ldflags("/ENTRY:DriverEntry", {force = true})
    set_encodings("utf8")
