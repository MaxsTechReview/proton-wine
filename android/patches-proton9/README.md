# Proton 9.0 Arm64EC fix patches

These patches apply on top of the Proton 9 Arm64EC source tree
(`maxjivi05/proton-wine`, branch `proton_9.0_arm64ec`). That tree already carries
the Android/Arm64EC work in-tree — the start-menu fix
(`WINE_NO_DUPLICATE_EXPLORER` in `programs/explorer/desktop.c`), the FEX UnixLibs
support (`load_unixlib_by_name` / `MemoryWineLoadUnixLibByName`) and the Steam
patches — so nothing here touches any of that.

Both patches are backports of fixes that already exist in the Proton 11 tree of
this repository but were never carried back to Proton 9.

## `dlls_ntdll_unix_virtual.c.noexec-storage.patch`

Fixes: executables cannot be launched from any drive other than `C:`.

Android mounts the emulated external storage (`/storage/emulated/0`, i.e. the
`D:`, `F:` and `G:` drives) with `noexec`. Wine maps PE sections straight from
the file, so when it applies a section's protection with `PROT_EXEC` the kernel
refuses it with `EACCES`/`EPERM`. `map_image_into_view()` only logs

    failed to set %08x protection on ... section ..., noexec filesystem?

and carries on, so the image loads with a non-executable `.text` and the process
dies the moment it jumps into it. `drive_c` lives on the app's private data
directory, which is a normal executable mount — hence "it only works from C:".

The fix teaches `mprotect_exec()` to detach the range from the file and re-back
it with anonymous memory (which is executable regardless of the mount flags)
when, and only when, `PROT_EXEC` was requested and the kernel refused it. It
also keeps the existing Android W^X fallback for sections marked
`READ|WRITE|EXECUTE`.

This is the same approach the Proton 11 tree already uses, plus a guard that
makes the range readable before copying its contents out (a `PROT_NONE` range
would otherwise fault inside the fixup itself).

## `dlls_winebus.sys_bus_sdl.c.controller.patch`

Fixes: no controller input at all — XInput, DirectInput and in-game SDL alike.

The Arm64EC build is configured `--without-udev`, so `bus_sdl` is winebus' only
device backend. Proton 9 opens it with a single
`dlopen(SONAME_LIBSDL2, RTLD_NOW)`, and `SONAME_LIBSDL2` is whatever soname the
SDL2 in the build container happened to have. The shipped
`lib/wine/aarch64-unix/winebus.so` asks for `libSDL2-2.0.so.0`, but the runtime
image ships `usr/lib/libSDL2-2.0.so` and `usr/lib/libSDL2.so` — the `.0` name
only exists if the host app creates that symlink at launch. When it does not,
the `dlopen` fails, winebus enumerates zero devices, and every controller API
goes dark at once. The only trace is a `WARN`, which is invisible under the
default `WINEDEBUG=-all`.

The fix tries every name SDL2 is known to be installed under
(`SONAME_LIBSDL2`, `libSDL2-2.0.so.0`, `libSDL2-2.0.so`, `libSDL2.so`), matching
what the Proton 11 tree already does.

The same patch also stops a missing haptic subsystem from taking the whole bus
down: `SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC)` fails as a whole if
either subsystem fails, and haptics are frequently unavailable on Android. It
now retries with `SDL_INIT_GAMECONTROLLER` alone, so a device without rumble
still gets its buttons and axes.

## Applying them

The `build-proton9-arm64ec.yml` workflow does this automatically. By hand:

    git clone -b proton_9.0_arm64ec https://github.com/maxjivi05/proton-wine
    cd proton-wine
    git apply /path/to/android/patches-proton9/*.patch
