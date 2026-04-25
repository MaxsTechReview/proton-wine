## Proton 11 Arm64EC — Android build

A Wine/Proton hybrid for Android, distributed as `.wcp` / `.wcp.xz`
packages consumable by **WinNative**, **Winlator**, **CMOD**, and
**Ludashi**.

The base is Valve's Wine fork ([ValveSoftware/wine][valve-wine],
`proton_11.0` branch). On top of that we apply an Android/Arm64EC port:
cross-build scripts targeting the Android NDK +
[bylaws/llvm-mingw][llvm-mingw] + a Termux aarch64 sysroot, runtime
workarounds for Bionic and noexec-mounted filesystems, eventfd-backed
esync/fsync synchronisation, and ADPF v2 performance-hint integration.

Steam support is provided by the Windows Steam client running inside
the prefix, so we do not ship Valve's `lsteamclient.dll` — the Windows
Steam runtime provides its own.

[valve-wine]: https://github.com/ValveSoftware/wine/tree/proton_11.0
[llvm-mingw]: https://github.com/bylaws/llvm-mingw


## What's in here

- `android/` — `android_sysvshm/` (userspace SysV SHM emulation for
  Bionic), `shm_utils/` (`shm_open` redirection), and `patches/` (a
  reference patch set of the Android-side source modifications).
- `build-scripts/` — staged cross-build orchestration:
  `build-step0.sh` builds `wine-tools` on the host, and
  `build-step-arm64ec.sh` / `build-step-x86_64.sh` configure, build and
  install the target-arch Wine tree against the NDK and llvm-mingw.
- `scripts/package-esync-wcp.sh` — turns an installed Wine tree into
  the two Winlator-compatible archives:
  - `<prefix>.wcp` — type `Proton`, consumed by WinNative
  - `<prefix>.wcp.xz` — type `Proton`, consumed by
    Winlator / CMOD / Ludashi
- `resources/prefixPack-arm64ec.txz` — the prefix pack that packagers
  bundle into the final `.wcp`, so no external download is required
  at build time.
- `.github/workflows/build-arm64ec.yml` — CI that reproduces the local
  build on `ubuntu-24.04`, caches the NDK / llvm-mingw / Termux
  sysroot, runs the build, and uploads the resulting `.wcp` files as
  workflow artifacts (and as release assets on tagged builds).


## Notable runtime changes

- **`dlls/ntdll/unix/virtual.c`** — Android filesystems that are mounted
  `noexec` reject `mprotect(PROT_EXEC)` on file-backed pages, which
  caused silent image-mapping failures. Added an anonymous-RWX fallback
  that remaps the section when `PROT_EXEC` returns `EACCES`.
- **`dlls/ntdll/unix/loader.c`** — Android-only defaults for
  `WINE_ADPF=8` and
  `VKD3D_CONFIG=virtual_heaps,no_upload_hvv` (`setenv` with
  `overwrite=0`, so anything the user exports still wins). ADPF v2
  integration via `libandroid`'s `APerformanceHint_*` APIs.
  `WINESTEAMNOEXEC` now defaults to `1` on Android (explicit
  `WINESTEAMNOEXEC=0` still disables it).
- **`dlls/ntdll/unix/esync.c` / `server/esync.c`** — eventfd-backed
  synchronisation primitives used throughout the port.
- **`dlls/ntdll/loader.c`** — `DllMain` failures stay fatal, but
  `load_dll` failures are demoted to `WARN` so late-bound Steam overlay
  DLLs don't produce noisy false fatals.
- **`dlls/user32/Makefile.in`** — `ws2_32` moved to `DELAYIMPORTS` so
  `wineboot` doesn't hang behind socket init on Android.
- **`dlls/winex11.drv/opengl.c`** — `WINE_X11FORCEGLX` env parsing fix.

See the individual files and `android/patches/*.patch` for the full
set of source-level changes.


## Building locally

Host requirements: Ubuntu 24.04 (or equivalent), the Android NDK r27d,
[bylaws/llvm-mingw][llvm-mingw] `20250920-ucrt`, and a Termux
aarch64 sysroot unpacked at `~/termuxfs/aarch64/`.

```
bash autogen.sh
bash build-scripts/build-step0.sh
bash build-scripts/build-step-arm64ec.sh --build-sysvshm
bash build-scripts/build-step-arm64ec.sh --configure
bash build-scripts/build-step-arm64ec.sh --build
bash build-scripts/build-step-arm64ec.sh --install
bash scripts/package-esync-wcp.sh \
  "$HOME/compiled-files-aarch64-esync-perf-lto" \
  "./dist/proton-11.0-arm64ec-$(date -u +%Y%m%d-%H%M)" \
  "proton-11.0-arm64ec" \
  "11" \
  "Proton 11 Arm64EC"
```

The build scripts hardcode paths for a specific layout
(`/home/max`, `/data/data/com.winnative.cmod/files/imagefs/...`); the CI
workflow pre-creates those directories on the runner so the same
scripts can run unchanged.


## Building in CI

The workflow is dispatched automatically on every push to
`proton_11.0` and on tags matching `v*`; it can also be run manually
via **Actions → Build Proton Wine Arm64EC → Run workflow**. Artifacts
are uploaded as `proton-11.0-arm64ec-<run>.wcp` and
`proton-11.0-arm64ec-<run>.wcp.xz` with a 30-day retention window. Tag
pushes additionally create a GitHub release with the same files.


## Packaging details

The packager produces two archives from one installed tree. Both are
xz-compressed tars (not zstd — matches what Winlator's content
installer expects). `bin/wine` is rewritten as a shell-script wrapper
that `exec`s `lib/wine/aarch64-unix/wine` so `/proc/self/exe` resolves
next to `ntdll.so`. Everything in `bin/` is real binaries or real
shell scripts — never symlinks, which would break extraction on
Android scoped storage and FAT32 SD cards.

`profile.json` fields:

- `type: Proton`
- `versionName: proton-11.0-arm64ec` — matches Ludashi's `WineInfo`
  regex `^(wine|proton)-([0-9.]+)-?([0-9.]+)?-(x86|x86_64|arm64ec)$`
- `versionCode: 11` — fixed for the Proton 11 line
- `description: "Proton 11 Arm64EC by WinNative <HHMM>"` — the `<HHMM>`
  suffix is the build timestamp the packager emits.


## Credits

- [ValveSoftware/wine][valve-wine] — base Wine tree, full commit
  history preserved on the `proton_11.0` branch.
- [bylaws/llvm-mingw][llvm-mingw] — ucrt64 toolchain used to compile
  the PE side of the build, including Arm64EC support.
- Upstream [Wine][winehq] authors (see `AUTHORS`).

[winehq]: https://www.winehq.org/


## License

Wine is distributed under the GNU LGPL 2.1-or-later; see `LICENSE`.
The Android port follows the same license as the code it modifies.
