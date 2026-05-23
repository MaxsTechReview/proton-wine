# Experiment Matrix — what we tried, what changed, what didn't

Reference for the Proton 10/11 Steam Launcher investigation.
Goal: tell your patched-Wine effort what you do NOT need to re-test.

All experiments run on device `192.168.50.144:44111` (CPH2749),
container `xuser-9` set to `Proton-10.0-4-x86_64-0`, target game
`MHST.exe` (appId 2356560). Each row produced an `wn-launcher.log`
in the container's `drive_c\` — see
`wn-launcher.log.proton10-fail.txt` for a representative trace.

Every experiment ended with **the same exit signature**: 5×
`LoadLibrary (LOAD_*) FAILED, GLE=998`, `DATAFILE load OK`, UEF AV in
`RtlProcessFlsData` at `ntdll+0x5d1d3` with fault address ending in
`0xae08` (varying upper bits per ASLR run).

## Env-var experiments

| Env var | Value tried | Source | Result |
|---|---|---|---|
| `PROTON_DISABLE_LSTEAMCLIENT` | `1` | Both container envVars + `XServerDisplayActivity.java:5491` (current) | env probe confirms `libc=1 win32=1`. Gate at `dlls/ntdll/loader.c:1151` fires (`use_lsteamclient` returns 0). Same crash. |
| `WINEDLLOVERRIDES` | `lsteamclient=` (disable) | `XServerDisplayActivity.java:~5470` | env probe confirms. Same crash. |
| `PROTON_USE_WOW64` | `0` | added during session, then **reverted** | Same crash. New-WoW64 path is irrelevant — we're 64-bit-to-64-bit. |
| `WINEESYNC` | `0` (force-disable esync) | added during session, then **reverted** | Same crash. esync ABI not the cause. |
| `WINENTSYNC` | `0` (force-disable ntsync) | added during session, then **reverted** | Same crash. |
| `WINEDEBUG` | `+warn,+err,+fixme,+module,+loaddll,+seh` | `XServerDisplayActivity.java:~5439` | env probe confirms. Notably **no Wine debug output ever reaches logcat** — the launcher's stderr is captured but Wine's TRACE/ERR doesn't flow through. So we couldn't see `lsteamclient disabled.` even if the gate fired (which it does — we patched the source to confirm). |

## Code-side experiments (`wn-steam-launcher/src/main.cpp`)

| Change | Result |
|---|---|
| Add `AddVectoredExceptionHandler(1, veh)` | The `AddVectoredExceptionHandler` call itself **hung the launcher under Proton 10**. Suspected Wine 10 bug in `RtlAddVectoredExceptionHandler` impl. Reverted to `SetUnhandledExceptionFilter` (top-level UEF) which works cleanly. |
| Add `SetUnhandledExceptionFilter(uef)` | Works on Proton 10. Catches the DEP AV with full diagnostic data (register file, page state, module list, stack chain). |
| Remove `tier0_s64.dll` + `vstdlib_s64.dll` from preload list | UEF still fires on same `tid` as main. Worker-thread-in-tier0-died theory **REFUTED**. |
| Add Win32 `GetEnvironmentVariableA` probe alongside libc `getenv` for every env var of interest | Both stores return identical values for every var probed. Confirms env is propagating through wine PEB construction correctly. |
| Add `dump_loaded_modules` snapshot pre-LoadLibrary + in UEF | Confirms the fault IP is OUTSIDE every loaded module → stale FLS callback to an unloaded DLL. |

## Preload experiment matrix (steamclient64.dll dependencies)

| Preload | Result |
|---|---|
| `msvcr120.dll` + `msvcp120.dll` + `vcruntime140.dll` + `msvcp140.dll` | Always succeed. Currently still preloaded. |
| `tier0_s64.dll` + `vstdlib_s64.dll` | Succeeded when present. Removed during session to test worker-thread theory; reinstate once the actual fix lands. |
| `video64.dll` + `SteamUI.dll` | Always fail with `GLE=126` (file not found in our staged Steam dir — Valve only ships these in the full Steam Client install we don't bundle). Failure is non-fatal — the launcher continues. |

## What changed on Proton 9 (the known-working baseline)

Same APK, same launcher binary, same Box64/FEX, **only the container's
`wineVersion` set to `Proton-9.0-x86_64-0` instead of `Proton-10.0-4-x86_64-0`**.

Result: `steamclient64.dll` loads on strategy 1/5. Logon succeeds.
`RequestAppInfoUpdate` returns 1. `IClientAppManager.LaunchApp` returns
a valid `HSteamAPICall` and the `LaunchAppResult_t` poll comes back with
`EAppUpdateError=0`. Game spawns via LaunchApp, not the CreateProcess
fallback. End-to-end working.

This is the ground truth that says **the entire WinNative + launcher +
Steam-DLL staging chain is correct** — the difference is purely
Proton 10's Wine 10 ntdll/loader rewrite.

## What we did NOT try (and why)

| Idea | Why not |
|---|---|
| Drop GameHub's ntdll into Proton 10 prefix | ABI-incompatible per `binary-comparison.md`. Would crash at first import resolution. |
| Drop Proton 9's ntdll into Proton 10 prefix | Same ABI problem. |
| Binary-patch Proton 10's `RtlProcessFlsData` to skip stale callbacks | Possible (`call *%r14` → `jmp +14` is 3 bytes), but the FLS AV is **secondary** — the primary failure is steamclient64.dll's DllMain itself. Skipping the FLS AV unblocks the cleanup path but doesn't unblock LoadLibrary. This is the patch in `Proton-Findings.md §4.A` — recommended as defensive but NOT a real fix. |
| Manual DLL mapping of steamclient64.dll (mmap + relocate + skip DllMain) | Heavy (~3-5 days), brittle per Valve build version. Punted in favor of asking the user. |
| Replace WinNative's Wine with vanilla wine-9 just for Steam Launcher mode | Big architectural change. Punted in favor of fixing the underlying Wine 10 issue, which is what this dossier is for. |
| `WINE_DISABLE_NTSYNC=1` (GE-Proton-10-specific env) | Only applies to GE-Proton 10-10 forks. Valve Proton 10.0-4 doesn't have ntsync enabled by default anyway. |

## Test command for your patched build

After rebuilding Wine in `/home/max/WinNative/Proton/` (or wherever)
and packaging it as a Proton component for WinNative:

```sh
# 1) Container set to your patched Proton.
# 2) Rebuild + sign + install the Emulator-1 APK (it bundles the launcher).
cd /home/max/WinNative/Emulator-1
bash app/src/main/cpp/wn-steam-launcher/build.sh
./gradlew :app:assembleStandardDebug
# sign + adb install (see reference_apk_signing memory in Emulator-1/CLAUDE.md)

# 3) Launch MHST on device, then pull the launcher log:
adb -s 192.168.50.144:<PORT> shell 'run-as com.winnative.cmod cat \
    "files/imagefs/home/xuser-9/.wine/drive_c/wn-launcher.log"'
```

Success criteria:
- `[wn-launcher] steamclient64.dll loaded at 0x...` (not `FAILED after all strategies`).
- `[wn-launcher] Steam_BLoggedOn=true after Nx100ms` (any reasonable N).
- `[wn-launcher] LaunchApp poll: COMPLETED ... EAppUpdateError=0`.
- `[wn-launcher] LaunchApp: "MHST.exe" is running` (NOT `falling back to CreateProcess`).

Anything short of that — diff against `wn-launcher.log.proton10-fail.txt`
and the patches in `Proton-Findings.md §4` to see what gave way.
