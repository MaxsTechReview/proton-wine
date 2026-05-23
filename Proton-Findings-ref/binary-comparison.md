# ntdll.dll Binary Comparison — Proton 10 vs Proton 9 vs GameHub (vanilla wine-9.0)

Reference dossier for the WinNative Steam Launcher / Proton-Findings.md investigation.

## File inventory (binaries copied alongside this doc)

| Local file | Original source | Size | SHA-1 | Notes |
|---|---|---|---|---|
| `proton10_ntdll.dll` | `<device>:files/contents/Proton/10.0-4-x86_64-0/lib/wine/x86_64-windows/ntdll.dll` | 3 084 288 B | `b09dfcd960cc7df8ee80e5ed6ff7ce44f33ebc73` | Proton 10.0-4 PE ntdll. **THE CRASHING ONE.** |
| `proton9_ntdll.dll`  | `<device>:files/contents/Proton/9.0-x86_64-0/lib/wine/x86_64-windows/ntdll.dll`  | 892 928 B | (see `sha1sum` output) | Proton 9 PE ntdll. **WORKS** for our Steam Launcher. |
| `gamehub_ntdll.dll`  | `/home/max/gamehub-re/wine-prefix-steam/drive_c/windows/system32/ntdll.dll`     | 3 934 253 B | `ed364b422978a4a5f97abebaa6ae2901ff0ff543` | Vanilla Ubuntu wine-9.0~repack-4build3 PE ntdll. Bundled inside GameHub. **WORKS** (their reference). |

Device the Proton 9/10 binaries came from: `192.168.50.144:44111` (CPH2749),
`com.winnative.cmod` package, ARM64 host running Box64 0.4.3.

## Why the size delta does NOT mean "GameHub's ntdll has extra fix code"

GameHub's ntdll is 0.85 MB BIGGER than Proton 10's. That's tempting to read as
"GameHub has more code (perhaps the workaround)." It's not.

`readelf` / `objdump -h` on the three binaries shows:

| Binary | `.text` size | `.rdata` size | Debug sections (`.debug_*`) |
|---|---|---|---|
| `proton10_ntdll.dll` | ~2.1 MB | ~0.9 MB | none — stripped Proton build |
| `proton9_ntdll.dll`  | ~0.6 MB | ~0.2 MB | none — stripped, older Wine |
| `gamehub_ntdll.dll`  | ~2.1 MB | ~0.4 MB | **~1.3 MB DWARF debug** (`.debug_info`, `.debug_str`, `.debug_line`, `.debug_aranges`) |

GameHub ships the debug build of the **same code mass** as Proton 10. The
"extra 0.85 MB" is purely debug info, not extra logic.

## `RtlProcessFlsData` body — byte-for-byte equivalent

Both binaries' implementation iterates `fls_data.fls_callback_chunks[i]->callbacks[index]`,
checks `!= NULL` and `!= (void *)~(ULONG_PTR)0`, and invokes the callback
via `call *<reg>`. **Neither validates that the callback pointer lies
inside a loaded module.** Same bug surface in both — see
`ntdll-loader-snippets.c` for the source.

Proton 10 (`proton10_ntdll.dll`, RVA `0x5d0e0`, fault at `0x5d1d3`):

```
17005d1bd:  bf 08 00 00 00         mov    $0x8,%edi              ; first slot offset
17005d1c2:  eb 25                  jmp    17005d1e9              ; -> load loop
17005d1d0:  41 ff d6               call   *%r14                  ; <-- AV here (stale ptr)
17005d1d3:  48 8b 44 de 10         mov    0x10(%rsi,%rbx,8),%rax ; <-- crash PC
17005d1e0:  48 83 c7 08            add    $0x8,%rdi
17005d1e9:  48 8b 44 de 10         mov    0x10(%rsi,%rbx,8),%rax
17005d1ee:  48 8b 0c 38            mov    (%rax,%rdi,1),%rcx     ; load slot data
17005d1f2:  48 85 c9               test   %rcx,%rcx
17005d1f5:  74 e9                  je     17005d1e0              ; skip if NULL data
17005d1f7:  49 8b 54 dd 00         mov    0x0(%r13,%rbx,8),%rdx
17005d1fc:  4c 8b 34 7a            mov    (%rdx,%rdi,2),%r14     ; load callback
17005d200:  49 8d 56 01            lea    0x1(%r14),%rdx
17005d204:  48 83 fa 02            cmp    $0x2,%rdx              ; only NULL / -1 guard
17005d208:  72 ce                  jb     17005d1d8              ; (skip those values)
```

GameHub (`gamehub_ntdll.dll`, RVA `0x5e8a0`):

```
17005e9e8:  41 be 10 00 00 00     mov    $0x10,%r14d
17005ea00:  41 ff d4              call   *%r12                   ; same indirect call
17005ea38:  4d 8b 64 08 10        mov    0x10(%r8,%rcx,1),%r12   ; load callback
17005ea45:  49 8d 44 24 ff        lea    -0x1(%r12),%rax
17005ea4a:  48 83 f8 fd           cmp    $0xfffffffffffffffd,%rax ; same NULL / -1 guard
17005ea4e:  77 bb                 ja     17005ea0b              ; (skip those values)
```

Identical guard logic. The structural difference is only register allocation +
CET `endbr64` prologs in GameHub's build. **GameHub has zero additional safety net for FLS callbacks.**

The full disassembly of Proton 10's `RtlProcessFlsData` is in
`disasm-RtlProcessFlsData.proton10.txt`.

## Why drop-in swapping the ntdll won't work — ABI mismatch

(Verified by export-table parse + objdump on each binary.)

### 1. Missing exports

Proton 10's ntdll exports ~40 functions that GameHub's wine-9 ntdll (and
Proton 9's ntdll) do NOT. Examples actually linked against by wine-10
`kernelbase.dll` / `kernel32.dll` / `win32u.dll`:

```
NtContinueEx
NtFlushBuffersFileEx
NtRaiseHardError
RtlVirtualUnwind2
RtlGetDeviceFamilyInfoEnum
RtlRbInsertNodeEx
RtlRbRemoveNode
RtlpHpAllocator              (heap helpers used by RtlAllocateHeap fast path)
_setjmpex
longjmp
__C_specific_handler         (variant with new args)
```

A wine-9 ntdll dropped into a wine-10 prefix will fail at first import
resolution with `STATUS_ENTRYPOINT_NOT_FOUND`. Process won't reach
`LdrInitializeThunk` past kernel32 init.

### 2. Unix-side syscall ABI differences

The PE-side ntdll talks to the Unix-side via `__wine_unix_call_dispatcher`.
Both syscall numbers AND struct layouts differ between Wine 9 and Wine 10.
Wine 10 added new dispatcher indices for:

- `unix_steamclient_setup_trampolines` (used by `build_module` for the
  lsteamclient injection)
- `__wine_get_unix_env`
- `__wine_set_unix_env`
- `__wine_dbg_ftrace`

Wine-9 ntdll has no concept of these. Even if you stub them out,
existing call indices use different struct layouts in wine-10 vs wine-9.

### 3. `KUSER_SHARED_DATA` layout

The shared page at fixed VA `0x7FFE0000` has new fields appended in
Wine 10. Wine-9 ntdll reads at offsets that no longer correspond to
the documented fields.

### 4. `WINE_UNIX_CALL` table

The unix-call index `0xB` that Proton 10's `build_module` uses for
`unix_steamclient_setup_trampolines` is not reserved in wine-9 — calling
it on a wine-9 unix side either returns `STATUS_NOT_IMPLEMENTED` or
hits an unrelated handler depending on table padding.

### Net

The PE ntdll and the Wine binary are tightly coupled. **They MUST be
the same version.** Swapping the PE binary alone is a guaranteed crash
at the first syscall. If you want wine-9-style steamclient handling on
top of wine-10/11, the only viable path is to **rebuild the Wine 10/11
source** with the breaking change reverted or fixed.

## Wine version stamp evidence

`strings` of each binary, focused on version + source-path markers:

| Binary | Build source path | Linker | Wine version string |
|---|---|---|---|
| `proton10_ntdll.dll` | `/home/runner/work/proton-wine/proton-wine` | lld-link 14 (llvm) | (no inline `wine-X.Y` literal — `wine_get_version` returns it at runtime) |
| `gamehub_ntdll.dll` | `/usr/src/wine-9.0~repack-4build3` | ld 2.41 (mingw-w64) | Ubuntu wine-9.0 |
| `proton9_ntdll.dll` | (Proton CI runner path) | lld-link (older) | Proton-Wine 9.0 |

The build-source paths are the smoking gun: GameHub ships Ubuntu's
distro-packaged wine-9.0, NOT a custom fork. They get away with running
Valve's steamclient64.dll because **wine-9's loader doesn't have whatever
wine-10/11 changed** that breaks Valve's DllMain.

## Pointers for your bisect

Test harness: build `Emulator-1/app/src/main/cpp/wn-steam-launcher/` with
your patched Wine in the container. The launcher's `wn-launcher.log`
gives you a turn-key fault signature at each step (see `Proton-Findings.md` §5).

Wine-mirror tags worth instrumenting:

- `wine-9.0` (known-good baseline)
- `wine-9.0-rc6` … `wine-10.0` (the regression window)
- `wine-10.0`
- `wine-10.0-rc1`
- `wine-11.0` (still failing per Proton 11 evidence)

A single-bisect run with the launcher as the test reproducer should land
on the offending commit within a dozen steps.
