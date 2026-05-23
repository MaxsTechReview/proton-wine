/*
 * launcher-main-snippets.cpp
 *
 * Hand-picked excerpts from the WinNative Steam Launcher's in-Wine PE host:
 *   Full source: /home/max/WinNative/Emulator-1/app/src/main/cpp/wn-steam-launcher/src/main.cpp
 *   Branch:      steam-launcher
 *   Build:       MinGW-w64 x86_64, /home/max/WinNative/Emulator-1/app/src/main/cpp/wn-steam-launcher/build.sh
 *   Output:      /home/max/WinNative/Emulator-1/app/src/main/assets/wnsteam/bionic/steam.exe
 *
 * These are the EXACT diagnostic paths that triggered + caught the AV
 * described in ../Proton-Findings.md. Use them as a reference test harness:
 * if your patched Wine build no longer produces the GLE=998 / RtlProcessFlsData
 * UEF trace, you've fixed it.
 *
 * STATUS: NOT A COMPILABLE FILE on its own (references log_line, env probes,
 *         WN_THISCALL, etc. from the full source). Reference / read-only.
 */


/* =========================================================================
 * SECTION A — env probe (libc getenv vs Win32 GetEnvironmentVariableA)
 *
 * Tells us whether the env var made it into BOTH libc env AND PEB env.
 * ntdll's get_env() reads PEB (RtlQueryEnvironmentVariable_U) so the
 * Win32 column is the one that matters for use_lsteamclient().
 *
 * Expected output on a Proton 10 launch with our planW env block set:
 *
 *   env probe: PROTON_DISABLE_LSTEAMCLIENT    libc=1          win32=1
 *   env probe: WINEDLLOVERRIDES               libc=lsteamclient= win32=lsteamclient=
 *   env probe: WINEDEBUG                      libc=+warn,+err,+fixme,+module,+loaddll,+seh win32=+warn,+err,+fixme,+module,+loaddll,+seh
 *   env probe: WINEESYNC                      libc=1          win32=1
 *   env probe: WINENTSYNC                     libc=(unset)    win32=(unset)
 *   env probe: PROTON_USE_WOW64               libc=(unset)    win32=(unset)
 * ========================================================================= */
{
    const char* probes[] = {
        "PROTON_DISABLE_LSTEAMCLIENT",
        "WINEDLLOVERRIDES",
        "WINEDEBUG",
        "WINEESYNC",
        "WINENTSYNC",
        "PROTON_USE_WOW64",
    };
    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); ++i) {
        const char* libc = getenv(probes[i]);
        char winv[256] = {0};
        DWORD wlen = GetEnvironmentVariableA(probes[i], winv, sizeof(winv));
        log_line("[wn-launcher] env probe: %-30s libc=%-10s win32=%s",
                 probes[i],
                 (libc && *libc) ? libc : "(unset)",
                 (wlen > 0 && wlen < sizeof(winv)) ? winv : "(unset)");
    }
}


/* =========================================================================
 * SECTION B — preload list (the dependencies steamclient64.dll needs).
 *
 * tier0_s64 + vstdlib_s64 PREVIOUSLY in this list; REMOVED during the
 * Proton 10 debug session to test whether the FLS AV was triggered by
 * tier0's DllMain spawning a worker thread that died in concurrent
 * cleanup. It was NOT — the crash persists either way. Restore them
 * if your patched Wine works, since the in-Wine steamclient runs
 * cleaner with explicit preloads.
 * ========================================================================= */
{
    struct Preload { const char* name; bool fullPath; };
    const Preload preloads[] = {
        { "msvcr120.dll", false }, { "msvcp120.dll", false },
        { "vcruntime140.dll", false }, { "msvcp140.dll", false },
        /* { "tier0_s64.dll",  true }, { "vstdlib_s64.dll", true }, */
        { "video64.dll",   true }, { "SteamUI.dll",     true },
    };
    for (const Preload& p : preloads) {
        HMODULE dm;
        if (p.fullPath) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s\\%s", kSteamDir, p.name);
            dm = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        } else {
            dm = LoadLibraryA(p.name);
        }
        if (dm) {
            log_line("[wn-launcher] preload %s: ok (%p)", p.name, dm);
        } else {
            log_line("[wn-launcher] preload %s: failed GLE=%lu (continuing)",
                     p.name, GetLastError());
        }
    }
}


/* =========================================================================
 * SECTION C — Unhandled Exception Filter (UEF).
 *
 * VEH (AddVectoredExceptionHandler) was tried first. The call itself
 * HUNG under Proton 10's ntdll — a Wine 10 bug independent of our
 * steamclient issue. SetUnhandledExceptionFilter doesn't go through
 * the same code path and works cleanly on all Proton versions tested.
 *
 * When LoadLibrary's primary AV propagates past Wine's internal
 * handlers (it does on Proton 10/11 — see Proton-Findings.md §3), this
 * UEF fires with full diagnostics.
 * ========================================================================= */
static LONG WINAPI launcher_unhandled_filter(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_EXECUTE_HANDLER;
    const EXCEPTION_RECORD* er = info->ExceptionRecord;
    void* ip = er->ExceptionAddress;

    char modName[MAX_PATH] = {0};
    HMODULE faultMod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ip, &faultMod)) {
        GetModuleFileNameA(faultMod, modName, sizeof(modName));
    }

    char bytes[3 * 16 + 1] = {0};
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(ip, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
            const unsigned char* p = (const unsigned char*)ip;
            int hp = 0;
            for (int i = 0; i < 16 && hp + 3 < (int)sizeof(bytes); ++i) {
                hp += snprintf(bytes + hp, sizeof(bytes) - hp, "%02x ", p[i]);
            }
        }
    }

    log_line("[wn-launcher] UEF: tid=%lu pid=%lu exc=0x%lx at %p mod='%s' bytes=%s",
             (unsigned long) GetCurrentThreadId(),
             (unsigned long) GetCurrentProcessId(),
             er->ExceptionCode, ip, modName[0] ? modName : "(unknown)", bytes);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* op = (er->ExceptionInformation[0] == 0) ? "read"
                       : (er->ExceptionInformation[0] == 1) ? "write"
                       : (er->ExceptionInformation[0] == 8) ? "DEP" : "?";
        log_line("[wn-launcher] UEF: AV %s fault_addr=0x%llx",
                 op, (unsigned long long) er->ExceptionInformation[1]);
    }

    /* Page info around the fault IP — confirms NOT executable. */
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(ip, &mbi, sizeof(mbi))) {
            log_line("[wn-launcher] UEF: page base=%p size=0x%llx state=0x%lx "
                     "protect=0x%lx alloc_protect=0x%lx type=0x%lx",
                     mbi.BaseAddress, (unsigned long long) mbi.RegionSize,
                     mbi.State, mbi.Protect, mbi.AllocationProtect, mbi.Type);
        }
    }

    /* CONTEXT register dump — tells us which register held the bad fnptr. */
    if (info->ContextRecord) {
        const CONTEXT* c = info->ContextRecord;
        log_line("[wn-launcher] UEF: ctx Rip=%llx Rsp=%llx Rbp=%llx",
                 (unsigned long long) c->Rip,
                 (unsigned long long) c->Rsp,
                 (unsigned long long) c->Rbp);
        log_line("[wn-launcher] UEF: ctx Rax=%llx Rcx=%llx Rdx=%llx Rbx=%llx",
                 (unsigned long long) c->Rax, (unsigned long long) c->Rcx,
                 (unsigned long long) c->Rdx, (unsigned long long) c->Rbx);
        log_line("[wn-launcher] UEF: ctx Rsi=%llx Rdi=%llx R8=%llx R9=%llx",
                 (unsigned long long) c->Rsi, (unsigned long long) c->Rdi,
                 (unsigned long long) c->R8,  (unsigned long long) c->R9);

        /* First 8 qwords on the stack — return-address chain hints. */
        const uint64_t* sp = (const uint64_t*) c->Rsp;
        MEMORY_BASIC_INFORMATION smbi;
        if (sp && VirtualQuery((LPCVOID) sp, &smbi, sizeof(smbi))
            && smbi.State == MEM_COMMIT) {
            char chain[256]; int p = 0;
            for (int i = 0; i < 8; ++i) {
                p += snprintf(chain + p, sizeof(chain) - p, "%llx ",
                              (unsigned long long) sp[i]);
            }
            log_line("[wn-launcher] UEF: stack[0..7]=%s", chain);
        }
    }

    dump_loaded_modules("UEF");
    return EXCEPTION_EXECUTE_HANDLER;
}


/* =========================================================================
 * SECTION D — module dumper (called pre-LoadLibrary AND in UEF).
 *
 * Lets you map the fault IP to a loaded module's address range. In our
 * failing trace the IP is OUTSIDE all loaded modules — confirming a
 * stale FLS callback pointing into a region that's no longer mapped.
 * ========================================================================= */
static void dump_loaded_modules(const char* when) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        log_line("[wn-launcher] modules(%s): CreateToolhelp32Snapshot failed GLE=%lu",
                 when, GetLastError());
        return;
    }
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    int n = 0;
    if (Module32First(snap, &me)) {
        do {
            log_line("[wn-launcher] modules(%s): base=%p size=0x%lx name=%s path=%s",
                     when, me.modBaseAddr, (unsigned long) me.modBaseSize,
                     me.szModule, me.szExePath);
            n++;
        } while (Module32Next(snap, &me));
    }
    log_line("[wn-launcher] modules(%s): total=%d", when, n);
    CloseHandle(snap);
}


/* =========================================================================
 * SECTION E — the 5-strategy LoadLibrary cascade for steamclient64.dll.
 *
 * Each strategy exercises a different path in Wine's PE loader. All 5
 * return GLE=998 on Proton 10/11; all 5 succeed on Proton 9.
 * The DATAFILE smoke test maps the image without running code — proves
 * the file is intact and the failure is in DllMain / runtime init.
 * ========================================================================= */
struct LoadAttempt { DWORD flags; const char* desc; };
const LoadAttempt attempts[] = {
    { LOAD_WITH_ALTERED_SEARCH_PATH,                                            "LOAD_WITH_ALTERED_SEARCH_PATH" },
    { 0,                                                                        "no flags" },
    { LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,                                         "SEARCH_DEFAULT_DIRS" },
    { LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_IGNORE_CODE_AUTHZ_LEVEL,          "SEARCH_DEFAULT_DIRS + IGNORE_CFG" },
    { LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32,          "DLL_LOAD_DIR + SYSTEM32" },
};
HMODULE lsc = NULL;
DWORD lastErr = 0;
for (int i = 0; i < (int)(sizeof(attempts)/sizeof(*attempts)) && !lsc; i++) {
    lsc = LoadLibraryExA(steamclientPath, NULL, attempts[i].flags);
    if (!lsc) {
        lastErr = GetLastError();
        log_line("[wn-launcher] load strategy %d/5 (%s) FAILED, GLE=%lu",
                 i+1, attempts[i].desc, lastErr);
    } else {
        log_line("[wn-launcher] steamclient64.dll loaded at %p (strategy %d/5: %s)",
                 lsc, i+1, attempts[i].desc);
    }
    Sleep(50);
}

/* DATAFILE smoke test — maps the image as a resource, no code runs. */
if (!lsc) {
    HMODULE probe = LoadLibraryExA(steamclientPath, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (probe) {
        log_line("[wn-launcher] diag: DATAFILE load OK — file is well-formed; "
                 "failure is in DllMain/runtime init");
        FreeLibrary(probe);
    }
}


/* =========================================================================
 * END
 * ========================================================================= */
