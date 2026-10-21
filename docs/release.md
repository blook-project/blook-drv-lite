# Release and registry flow

Releases follow the shared `blook-project` contract
(`blook-xmake-repo/docs/RELEASE.md`): a tag leads to a GitHub Release asset
that the registry can install via `add_requires`.

## Checklist

1. **Evidence**: a real bvmenv VM e2e run is committed under
   `docs/evidence/<date>-drv-lite/` with `result.json` (see
   [evidence policy](evidence/README.md)). No dry runs.
2. **CI green** on `main`: build (driver + SDK + CLI + example + guest
   helpers), host unit tests, clang-format, package.
3. **Version bump**: `version.txt` and the `local version` literal in
   `xmake.lua` must match (CI checks this). Update `CHANGELOG.md`.
4. **Sign locally** (release asset): `./scripts/Build.ps1` signs
   `drvlite.sys` with the `signer/` SPC assets.
5. **Tag + release**:
   ```powershell
   git tag v1.0.0 && git push origin v1.0.0
   # release.yml builds, tests, packages and publishes:
   #   blook-drv-lite-1.0.0-windows-x64.zip
   #   blook-drv-lite-1.0.0-windows-x64.zip.sha256
   ```
6. **Registry** (run by `ci` after the release is visible):
   ```powershell
   pwsh scripts/publish-registry.ps1 -Package blook-drv-lite -Version 1.0.0
   ```
   The registry manifest is `packages/b/blook-drv-lite/xmake.lua`; the script
   reads the release via `gh`, appends `<version> <sha256>` to `versions.txt`
   and pushes. Dry run first if unsure:
   `-DryRun -Sha256 <64hex>`.

## Asset layout

```
blook-drv-lite-<version>-windows-x64.zip
  include/blook/drvlite/*.h     # SDK + protocol headers
  lib/x64/drvlite-sdk.lib       # SDK static library
  bin/x64/drvlite.sys           # kernel driver (signed locally)
  bin/x64/drvlite-cli.exe
  bin/x64/drvlite-target.exe
  bin/x64/drvlite-unsigned.sys  # DSE e2e helper
  share/xmake-package/          # xmake package definition
  docs/ scripts/ README.md LICENSE CHANGELOG.md version.txt
```

`scripts/package.ps1 -Mode release` builds exactly this layout; the archive
itself contains no build intermediates, PDBs, VM images or private keys.

## Registry consumer

```lua
add_repositories("blook https://github.com/blook-project/blook-xmake-repo.git")
add_requires("blook-drv-lite 1.0.0")
target("my-tool")
    add_packages("blook-drv-lite")
```

## Versioning

- `0.x` during bring-up, `1.0.0` once VM e2e evidence is green and all
  supported features are validated on the reference build.
- Protocol ABI changes bump `DRVLITE_ABI_MINOR` and require a release note;
  opcode additions are backward compatible.
