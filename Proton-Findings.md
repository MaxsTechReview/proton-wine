# Proton 10 / 11 — `steamclient64.dll` DllMain Regression

**Author:** WinNative Steam Launcher debug session, 2026-05-23
**Repo affected:** `/home/max/WinNative/Emulator-1` (branch `steam-launcher`)
**Wine source under repair:** `/home/max/WinNative/Proton/` (Wine 11.0 baseline; Wine 10 needs equivalent patches)
**Companion source under repair:** `/home/max/WinNative/Proton-11/` (build outputs only — no source here)
**Reference files:** `./Proton-Findings-ref/` (see [§Reference Inventory](#reference-inventory))

---

## 1. TL;DR

WinNative's "Steam Launcher" mode runs a custom Wine PE host (`steam.exe`,
built from `app/src/main/cpp/wn-steam-launcher/`) that `LoadLibrary`s
Valve's REAL Windows `steamclient64.dll`. Launches refresh-token logon
via `IClientEngine` → `IClientUser`, calls
`IClientAppManager::LaunchApp`, and spawns the game in-process.

| Proton version | Wine base | Status |
|---|---|---|
| Proton 9.0 x86_64 | wine-9.0 + Proton patches | **✅ Works end-to-end** |
| GameHub bundled wine | vanilla Ubuntu wine-9.0 | **✅ Works** (reference / proof point) |
| Proton 10.0-4 x86_64 | wine-10.x + Proton patches | ❌ `LoadLibrary` → `GLE=998` |
| Proton 10.0.99 arm64ec | wine-10.x + Proton patches | ❌ Same failure |
| Proton 11.0-5 x86_64 | wine-11.x + Proton patches | ❌ Same failure |

`LoadLibrary("steamclient64.dll")` returns `GLE=998` (`ERROR_NOACCESS`)
across all 5 PE-loader strategies on Proton 10/11.
`LOAD_LIBRARY_AS_DATAFILE` smoke load **succeeds** ⇒ the DLL file is
fine, the fault is inside DllMain / loader-time initialization.

A vectored UEF captures a **secondary** DEP AV inside `RtlProcessFlsData`
(`ntdll!RtlProcessFlsData+0xf3`) — Wine's loader runs FLS callbacks as
part of failure-cleanup after `steamclient64.dll`'s DllMain bails. One
FLS slot has a callback pointer to **completely unmapped memory** (page
state `MEM_FREE`); invoking it produces a second AV which surfaces.
**The FLS AV is downstream of the real DllMain failure** — patching FLS
alone won't unblock LoadLibrary.

---

## 2. What we proved is NOT the cause (so don't waste time here)

These were all verified on-device with the diagnostic launcher
(see `Proton-Findings-ref/launcher-main-snippets.cpp` and the captured
launcher log `Proton-Findings-ref/wn-launcher.log.proton10-fail.txt`):

| Hypothesis | Verdict | How disproved |
|---|---|---|
| Box64 / FEX translator bug | **NO** | Same Box64 binary works on Proton 9. The crash is in Wine's PE-side ntdll, which Box64 just faithfully translates. |
| WoW64 (32-bit-in-64-bit thunks) | **NO** | The failing DLL is 64-bit, host is 64-bit. WoW64 isn't on the path. `PROTON_USE_WOW64=0` had no effect. |
| Esync / ntsync thread-sync rework | **NO** | `WINEESYNC=0 WINENTSYNC=0` left the crash identical. |
| Proton's lsteamclient injection | **NO** | `PROTON_DISABLE_LSTEAMCLIENT=1` verified in PEB env (Win32 `GetEnvironmentVariableA` returns `"1"` — see `env probe` lines), the gate at `dlls/ntdll/loader.c:1151` fires, the IAT-rewrite (`import_dll` @ 1201) is skipped. Same crash anyway. |
| tier0_s64.dll / vstdlib_s64.dll preload triggering a worker thread that dies & runs RtlProcessFlsData | **NO** | Removed those preloads from the launcher; crash unchanged. UEF reports `tid == main_tid`, so a worker thread is not the trigger. |
| Differences in `RtlProcessFlsData` body between Proton and vanilla Wine | **NO** | Byte-for-byte equivalent logic, same NULL/`-1` guard, neither validates that the callback pointer lies inside a loaded module. |
| GameHub having a magic "fixed" ntdll | **NO** | GameHub ships **vanilla Ubuntu wine-9.0** ntdll. The 3.94 MB vs Proton 10's 3.08 MB difference is entirely DWARF debug sections (`.debug_info` = 1.3 MB) — same code mass. |
| Drop-in-swap GameHub's ntdll (or Proton 9's ntdll) into Proton 10 prefix | **WILL NOT WORK** | ABI mismatch: 40 exports missing (`NtContinueEx`, `RtlVirtualUnwind2`, `RtlGetDeviceFamilyInfoEnum`, `_setjmpex`, `longjmp`, …). Wine-10 kernelbase/kernel32/win32u link against these → `STATUS_ENTRYPOINT_NOT_FOUND` at init. `__wine_unix_call_dispatcher` syscall numbers + struct layouts also differ between Wine 9 and 10. Process won't reach `LdrInitializeThunk` past kernel32 init. |

---

## 3. What IS the cause (deep-RE conclusion)

**Wine 10/11's loader broke `steamclient64.dll`'s DllMain.** The specific
change has not been bisected yet, but the bisection range is narrow:
**wine-9.0 (works) → wine-10.0 (broken)**. Major loader-touching
changes in that window:

1. Headline Wine 10 rework: ARM64EC support landed, which restructured
   the PE loader (`dlls/ntdll/loader.c`) for hybrid module handling
   even on x86_64 builds.
2. New "Windows-like WoW64" loader path (touched 32-bit-in-64-bit
   spawn even though it's not on the x86_64-only path).
3. TLS slot accounting and DllMain ordering changes.
4. Tightened DllMain reentrancy / loader-lock acquisition.
5. New FLS chunk allocation layout (`dlls/ntdll/thread.c:545`+) using
   `fls_callback_chunks[]` array of dynamically-grown chunks instead
   of a single static array.

Valve's `steamclient64.dll`'s `DllMain` is famously fragile:
- Calls `LoadLibrary` reentrantly from `DLL_PROCESS_ATTACH` (the exact
  pattern Microsoft's NT loader docs warn against).
- Touches its own IAT for `tier0_s64.dll` / `vstdlib_s64.dll` very
  early during attach.
- Walks TLS callback pointers.

Any of (1)–(5) above could expose the latent DllMain fragility. The
**FLS AV we see in `RtlProcessFlsData` is the SECOND fault** — Wine's
loader catches the original DllMain AV, returns `ERROR_NOACCESS`
from LoadLibrary, then triggers FLS cleanup as part of unwinding the
failed module, which hits a stale FLS slot left over from process
startup.

### 3.1 The stale FLS slot

UEF diagnostics across two Proton 10 runs:

| Run | Fault `Rip` | Page state | Rax | Rsi |
|---|---|---|---|---|
| 1 | `0x7ffc90ae08` | `MEM_FREE` (0x10000), `PAGE_NOACCESS` (0x1) | `0x5cd20` | `0x59260` |
| 2 | `0x7ffdf2ae08` | `MEM_FREE` (0x10000), `PAGE_NOACCESS` (0x1) | `0x5cd60` | `0x592a0` |

Both fault addresses **end in `0xae08`** — same function offset within
some DLL. The base differs per run because ASLR varies. The "missing"
DLL is not in the loaded-module list at fault time → it loaded during
Wine 10 process init, registered an FLS callback via `FlsAlloc`, then
unloaded **without clearing its FLS slot**. Wine 9 either skipped this
cleanup path or had cleaner startup.

`RtlProcessFlsData` body (`dlls/ntdll/thread.c:698-740`) only guards
against `NULL` and `(void *)~(ULONG_PTR)0` — it has no check that the
callback lies inside a loaded module. That's the bug to fix in your
build.

---

## 4. What you should patch in your Proton build

There are two levels of fix. **(A) is the symptom suppressor** — needed
to unblock LoadLibrary's secondary AV. **(B) is the real fix** — needed
to identify and unbreak the Wine 10 loader change that broke DllMain.

### 4.A — Symptom suppressor (low-risk, drop-in patch)

**File:** `dlls/ntdll/thread.c`
**Function:** `RtlProcessFlsData` (currently around line 698)
**Change:** add a module-range guard before invoking the callback. Pseudo-diff:

```diff
--- a/dlls/ntdll/thread.c
+++ b/dlls/ntdll/thread.c
@@ -722,7 +722,18 @@ void WINAPI DECLSPEC_HOTPATCH RtlProcessFlsData( void *teb_fls_data, ULONG flags )
                 if (callback && callback != (void *)~(ULONG_PTR)0)
                 {
+                    /* WinNative / Proton-Findings.md: guard against stale
+                     * FLS callbacks left by DLLs that unloaded without
+                     * clearing their slots. RtlPcToFileHeader returns NULL
+                     * for addresses outside any loaded module; skipping
+                     * those callbacks is strictly safer than invoking a
+                     * pointer into a freed region. Triggered by Valve's
+                     * steamclient64.dll DllMain failure-cleanup path under
+                     * the wine-10/11 loader. */
+                    void *image_base = NULL;
+                    RtlPcToFileHeader( (PVOID)callback, &image_base );
+                    if (!image_base) { ERR_(thread)("Stale FLS callback %p — skipped.\n", callback); continue; }
                     TRACE_(relay)("Calling FLS callback %p, arg %p.\n", callback,
                             fls->fls_data_chunks[i][index + 1]);
```

This is **belt-and-suspenders** — strictly defensive. It does NOT fix
the primary LoadLibrary failure. It just prevents the secondary AV from
crashing the process during the failure-cleanup path. You'd still see
the LoadLibrary return `GLE=998` from steamclient64.dll's DllMain. But
the process would survive long enough for our launcher's `CreateProcess`
fallback to spawn the game.

Apply the equivalent diff to `/home/max/WinNative/Proton-11/` source
once you've cloned it.

### 4.B — Real fix (find what broke DllMain)

Bisect `wine-9.0..wine-10.0` for the loader change that breaks
`steamclient64.dll`'s DllMain. Suggested approach:

1. Build vanilla wine-9.0 → confirm `steamclient64.dll` loads cleanly
   under our `steam.exe` host (use `launcher-main-snippets.cpp` in
   `Proton-Findings-ref/` as the test harness).
2. Build vanilla wine-10.0 RC1 → confirm failure reproduces.
3. `git bisect` between those two tags inside `wine-mirror/wine`.
   Focus the search on commits touching:
   - `dlls/ntdll/loader.c` (especially `build_module`, `import_dll`,
     `process_attach`, `MODULE_InitDLL`).
   - `dlls/ntdll/thread.c` (FLS slot allocation rework).
   - `dlls/ntdll/unix/loader.c` (Unix-side PE mapping).
   - `dlls/ntdll/relay.c` (TLS callback dispatch).
4. The bisect endpoint identifies the single Wine commit that
   regressed steamclient64.dll's DllMain. From there, either:
   - Revert that one commit in your Proton build, OR
   - Forward-port a smaller fix that preserves the intent without
     breaking the steamclient case.

### 4.C — Already-known Proton hooks worth re-checking

Even though `PROTON_DISABLE_LSTEAMCLIENT=1` did NOT fix our crash, these
two Proton-specific patches are on the failing code path and may be
worth disabling (or making conditional) to narrow further:

**File:** `dlls/ntdll/loader.c`

| Hook | Location | What it does | Why it might matter |
|---|---|---|---|
| `use_lsteamclient()` | ~line 1151 | Reads `PROTON_DISABLE_LSTEAMCLIENT`; default = enabled | Force-loads `lsteamclient.dll` on every `steamclient*` load and sets `LDR_DONT_CALL_DLLMAIN`. |
| `import_dll` rewrite | ~line 1201 | Renames `tier0_s64.dll`/`vstdlib_s64.dll` imports to `ntdll.dll` for `steamclient64.dll` | Hard-coded IAT rewriting. Even when bypassed by `PROTON_DISABLE_LSTEAMCLIENT=1`, the body of `use_lsteamclient()` reads from `RtlQueryEnvironmentVariable_U` whose first-call value is cached in a `static int use`. Race: if anything in wine-10 calls `use_lsteamclient()` BEFORE the PEB env is fully populated, `use=1` gets cached and the env var is ignored. **Worth checking your build for that race.** |
| `build_module` steamclient injection | ~line 2376-2417 | Force-loads `lsteamclient.dll`, installs trampolines via `unix_steamclient_setup_trampolines` (unix-call index `0xB`) | Same caching concern. |

To make experimentation easier in your build, replace the `static int use = -1` cache with a **re-evaluating** version, or just always read the env var:

```diff
-static int use_lsteamclient(void)
-{
-    WCHAR env[32];
-    static int use = -1;
-    if (use != -1) return use;
-    use = !get_env( L"PROTON_DISABLE_LSTEAMCLIENT", env, sizeof(env) ) || *env == '0';
-    if (!use) ERR("lsteamclient disabled.\n");
-    return use;
-}
+static int use_lsteamclient(void)
+{
+    WCHAR env[32];
+    /* WinNative / Proton-Findings.md: removed static cache so the env
+     * var is honored regardless of when ntdll's loader first asks. */
+    int use = !get_env( L"PROTON_DISABLE_LSTEAMCLIENT", env, sizeof(env) ) || *env == '0';
+    return use;
+}
+
```

(Mass-call overhead is negligible — `RtlQueryEnvironmentVariable_U`
hits the PEB, not disk.)

---

## 5. How to test patched builds against our reproducer

The diagnostic launcher is in this repo at:
`/home/max/WinNative/Emulator-1/app/src/main/cpp/wn-steam-launcher/src/main.cpp`

Build it with:
```sh
bash /home/max/WinNative/Emulator-1/app/src/main/cpp/wn-steam-launcher/build.sh
```
Output: `app/src/main/assets/wnsteam/bionic/steam.exe` (PE32+ x86_64, ~60 KB).

The launcher writes a detailed trace to `C:\wn-launcher.log` inside the
Wine prefix it runs in, and includes:

- libc `getenv` + Win32 `GetEnvironmentVariableA` probe of
  `PROTON_DISABLE_LSTEAMCLIENT`, `WINEDLLOVERRIDES`, `WINEDEBUG`,
  `WINEESYNC`, `WINENTSYNC`, `PROTON_USE_WOW64`.
- 5-strategy `LoadLibraryExA` cascade for `steamclient64.dll`.
- `LOAD_LIBRARY_AS_DATAFILE` smoke test (proves the file itself is OK).
- `SetUnhandledExceptionFilter` UEF that on AV logs:
  - `tid` / `pid`
  - exception code + fault IP
  - `MEMORY_BASIC_INFORMATION` at IP (state / protect)
  - full register file (Rip/Rsp/Rbp + Rax/Rcx/Rdx/Rbx + Rsi/Rdi/R8/R9)
  - first 8 qwords on the stack (return-address chain)
  - dump of all loaded modules at exception time

A patched Wine where `RtlProcessFlsData` no longer AVs should show:

```
diag: DATAFILE load OK — file is well-formed; failure is in DllMain/runtime init
steamclient64.dll loaded at 0x... (strategy 1/5: LOAD_WITH_ALTERED_SEARCH_PATH)
```

instead of the current 5×`FAILED, GLE=998` + UEF AV trace.

### Test apparatus (read-only — for understanding what the launcher does)

- `Proton-Findings-ref/launcher-main-snippets.cpp` — the launcher
  source sections that exercise the broken path. Concretely:
  - dependency preloads (`tier0_s64.dll`, `vstdlib_s64.dll`,
    `msvcr120.dll`, `msvcp120.dll`, `vcruntime140.dll`, `msvcp140.dll`)
  - the 5 LoadLibraryEx strategies + 3 cold-start retries
  - the DATAFILE smoke test
  - the UEF body that dumps the diagnostics above
- `Proton-Findings-ref/wn-launcher.log.proton10-fail.txt` — full
  log of a failed Proton 10 launch with the diagnostic build, including
  all `env probe`, `modules(pre-LoadLibrary)`, `modules(UEF)` blocks.

---

## 6. Reference Inventory

All under `/home/max/WinNative/Proton/Proton-Findings-ref/`:

| File | What it is | Where it came from |
|---|---|---|
| `wn-launcher.log.proton10-fail.txt` | Full launcher trace of the failing Proton 10.0-4 launch (MHST appId 2356560). Contains env probes, module dumps, the 5 LoadLibrary strategies all returning GLE=998, the DATAFILE smoke test passing, and the UEF-captured AV in `RtlProcessFlsData`. | Pulled from device 192.168.50.144 at `/data/data/com.winnative.cmod/files/imagefs/home/xuser-9/.wine/drive_c/wn-launcher.log` after launching MHST in a Proton 10 container. |
| `ntdll-loader-snippets.c` | Hand-picked extracts of the relevant Wine 10/11 source from `dlls/ntdll/loader.c` + `dlls/ntdll/thread.c`: `use_lsteamclient`, `import_dll`'s IAT-rewrite branch, `build_module`'s steamclient injection, the three call sites of `RtlProcessFlsData`, the full body of `RtlProcessFlsData`. Cross-reference comments link each excerpt to its file:line. | Extracted from `/home/max/Build/Proton/proton-arm64-nightlies/wine-source/` (Proton's own wine fork, current head). The same paths exist in this repo's `dlls/ntdll/`. |
| `launcher-main-snippets.cpp` | The diagnostic body of our launcher: preloads, the 5-strategy LoadLibrary cascade, the SetUnhandledExceptionFilter handler, the module-dump helper, the FLS-relevant register dump. Self-contained — paste into any MinGW x86_64 PE to reproduce on a custom Wine build. | Extracted from `/home/max/WinNative/Emulator-1/app/src/main/cpp/wn-steam-launcher/src/main.cpp` head of branch `steam-launcher`. |
| `disasm-RtlProcessFlsData.proton10.txt` | `objdump -d` of `RtlProcessFlsData` in Proton 10.0-4's ntdll.dll, with the fault instruction (`call *%r14` at RVA `0x5d1d0`) annotated. | Disassembled from the device's `files/contents/Proton/10.0-4-x86_64-0/lib/wine/x86_64-windows/ntdll.dll` (pulled to `/tmp/proton10_ntdll.dll`). |
| `binary-comparison.md` | Sha-1 hashes, sizes, version strings, and key disassembly diffs for: Proton 10.0-4 ntdll vs Proton 9.0 ntdll vs vanilla wine-9.0 ntdll (from GameHub). Includes the ABI-mismatch export list (40 functions missing from wine-9 ntdll that wine-10 kernelbase/kernel32/win32u link against). | Synthesized from `objdump -p` / `objdump -d` and our parsed export tables. The three ntdll binaries themselves are NOT included here (3 MB each); local paths: `/tmp/proton10_ntdll.dll`, `/tmp/proton9_ntdll.dll`, `/home/max/gamehub-re/wine-prefix-steam/drive_c/windows/system32/ntdll.dll`. |
| `experiment-matrix.md` | Catalog of every env var, preload change, and Wine flag tried, with the launcher's resulting log signature for each. Tells you which experiments you do NOT need to repeat. | Authored from the debugging session. |

---

## 7. The "ABI mismatch" — why you can't shortcut by swapping ntdll.dll

Multiple times in the debug session we considered dropping vanilla
wine-9 ntdll.dll into a Proton 10 prefix. This **WILL NOT WORK** and
the agent's RE in `binary-comparison.md` shows why concretely. Quick
summary:

1. **40 exports** are present in Proton 10 ntdll but absent in wine-9
   ntdll. Wine-10 kernelbase/kernel32/win32u link against them. First
   import resolution → `STATUS_ENTRYPOINT_NOT_FOUND`.
2. **Unix-side syscall ABI** differs. Proton 10 PE ntdll calls
   `__wine_unix_call_dispatcher` (RVA `0x70f76`) with new dispatcher
   indices the wine-9 PE ntdll doesn't know about
   (`__wine_get_unix_env`, `__wine_set_unix_env`, `__wine_dbg_ftrace`).
3. **`KUSER_SHARED_DATA` offset layout** differs between Wine 9 and 10.
   Mixing breaks `SystemTime` / `TickCount` reads.
4. **`WINE_UNIX_CALL` table indices** differ. The number 0xB that
   Proton 10's `build_module` uses for `unix_steamclient_setup_trampolines`
   isn't reserved at all in wine-9.

The PE ntdll and the Wine binary are tightly coupled. They MUST match
versions. If you want to ship "wine-9-style" steamclient handling on
top of wine-10/11, the only viable path is to **rebuild the Wine 10/11
source with the breaking change reverted or fixed** — not swap binaries.

---

## 8. Quick fault-isolation hints for your patched build

If your patched build still fails, the diagnostic launcher will tell
you exactly how. Look at the `wn-launcher.log` in the container at
`<prefix>/drive_c/wn-launcher.log` and check:

| What you see | What it means |
|---|---|
| `steamclient64.dll loaded at 0x...` then `Steam_BLoggedOn=true` | **YOU FIXED IT.** Continue testing LaunchApp end-to-end. |
| Same 5×`FAILED GLE=998` + DATAFILE OK + UEF AV in `RtlProcessFlsData` | Same regression. Your patch didn't take. Confirm the .so / .dll built into the container. |
| Same 5×`FAILED GLE=998` but UEF AV at a different function (no longer `RtlProcessFlsData`) | Your `RtlProcessFlsData` guard worked. The next AV up the chain is now visible — this is the **real** DllMain fault site that wine-10/11 introduced. Iterate from there. |
| `LaunchApp returned a null call handle` | Loader OK but `IClientAppManager::LaunchApp` itself misbehaves. Cross-reference with `Emulator-1/app/src/main/cpp/wn-steam-launcher/src/main.cpp` STEP 5 + the IClientUtils poll. |

---

## 9. Useful absolute paths for your Proton build effort

- This Wine 11.0 source tree: `/home/max/WinNative/Proton/`
- Proton 11 build outputs: `/home/max/WinNative/Proton-11/`
- WinNative repo with the test harness: `/home/max/WinNative/Emulator-1/` (branch `steam-launcher`)
- Launcher source: `app/src/main/cpp/wn-steam-launcher/src/main.cpp`
- Launcher build script: `app/src/main/cpp/wn-steam-launcher/build.sh`
- Proton ntdll source already consulted: `/home/max/Build/Proton/proton-arm64-nightlies/wine-source/dlls/ntdll/`
- GameHub reference: `/home/max/gamehub-re/wine-prefix-steam/drive_c/windows/system32/ntdll.dll` (vanilla Ubuntu wine-9.0 — proof point that wine-9-class code works for steamclient64.dll)
- Device for testing: `192.168.50.144:44111` (CPH2749, ARM64, Box64 0.4.3, FEX Fex_20260509)
- Test app & game: `com.winnative.cmod` package, MHST (appId 2356560) container `xuser-9` set to Proton 10.0-4-x86_64
- APK build + sign + install commands: see `Emulator-1` CLAUDE.md / memory `reference_apk_signing.md`

---

*Document end. Open `Proton-Findings-ref/` for the binaries, logs, and source extracts cited above.*
