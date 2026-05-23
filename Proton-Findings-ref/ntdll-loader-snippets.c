/*
 * ntdll-loader-snippets.c
 *
 * Hand-picked extracts from Wine 10's (Proton 10's) ntdll source, focused
 * on the code paths that interact with `steamclient64.dll` loading and the
 * FLS (Fiber Local Storage) callback walker.
 *
 * Source root: /home/max/Build/Proton/proton-arm64-nightlies/wine-source/
 *              (Proton's own wine fork — same file layout exists at
 *               /home/max/WinNative/Proton/dlls/ntdll/ for the local
 *               Wine 11 source tree.)
 *
 * Line numbers given are from Proton's wine source as of 2026-05-23.
 * Equivalent code lives in this repo at:
 *   /home/max/WinNative/Proton/dlls/ntdll/loader.c
 *   /home/max/WinNative/Proton/dlls/ntdll/thread.c
 *
 * Annotated for the WinNative Steam Launcher / Proton-Findings.md effort.
 *
 * STATUS: NOT A COMPILABLE FILE. Reference / read-only.
 */


/* =========================================================================
 * 1. dlls/ntdll/loader.c  ::  use_lsteamclient()
 *    Reads PROTON_DISABLE_LSTEAMCLIENT once and caches in `static int use`.
 *    Caching is suspicious — if any ntdll path calls this before the PEB
 *    env is fully populated, the cache locks at `use = 1` and the env var
 *    is silently ignored. Verified with our launcher's env probe that the
 *    var IS present in both libc env AND Win32 PEB env when our main() runs,
 *    so for OUR launches the gate does fire correctly — but this is worth
 *    de-caching in your patched build to keep experiments deterministic.
 *
 *    Original — Proton 10 (line 1151):
 * ========================================================================= */
static int use_lsteamclient(void)
{
    WCHAR env[32];
    static int use = -1;

    if (use != -1) return use;

    use = !get_env( L"PROTON_DISABLE_LSTEAMCLIENT", env, sizeof(env) ) || *env == '0';
    if (!use)
        ERR("lsteamclient disabled.\n");
    return use;
}

/* Suggested re-evaluating variant (no static cache): */
static int use_lsteamclient_recheck(void)
{
    WCHAR env[32];
    int use = !get_env( L"PROTON_DISABLE_LSTEAMCLIENT", env, sizeof(env) ) || *env == '0';
    return use;
}


/* =========================================================================
 * 2. dlls/ntdll/loader.c  ::  import_dll  (the IAT-rewrite branch, line 1201)
 *    When steamclient64.dll or gameoverlayrenderer64.dll imports
 *    tier0_s64.dll or vstdlib_s64.dll, ntdll silently REWRITES the import
 *    name to "ntdll.dll" and binds the import there. This is the Proton
 *    steamclient trampoline mechanism. With PROTON_DISABLE_LSTEAMCLIENT=1
 *    the gate is bypassed and the names are NOT rewritten — confirmed via
 *    our env probe.
 *
 *    Excerpt (around line 1196-1216):
 * ========================================================================= */
static BOOL import_dll( WINE_MODREF *wm, const IMAGE_IMPORT_DESCRIPTOR *descr,
                        LPCWSTR load_path, WINE_MODREF **pwm )
{
    /* ... preamble omitted: gets `name` = imported DLL name (e.g. "tier0_s64.dll") ... */

    if (use_lsteamclient())
    {
        if ((!strcmp(name, "tier0_s64.dll") || !strcmp(name, "vstdlib_s64.dll"))
            && wm->ldr.BaseDllName.Buffer
            && (!wcscmp(wm->ldr.BaseDllName.Buffer, L"steamclient64.dll")
                || !wcscmp(wm->ldr.BaseDllName.Buffer, L"gameoverlayrenderer64.dll")))
        {
            TRACE("%s -> ntdll.\n", name);
            name = "ntdll.dll";   /* <-- IAT name silently rewritten */
        }
    }

    /* ... rest of import_dll: builds wide name, loads the DLL, walks thunks ... */
}


/* =========================================================================
 * 3. dlls/ntdll/loader.c  ::  build_module  (steamclient trampoline install,
 *    line ~2376-2417)
 *    When steamclient*.dll or gameoverlayrenderer*.dll loads, ntdll FORCE-
 *    LOADS lsteamclient.dll and installs export-table trampolines via a
 *    Unix-side call (`unix_steamclient_setup_trampolines`, unix-call
 *    index 0xB). When trampolines install successfully, sets
 *    LDR_DONT_CALL_DLLMAIN — Valve's own DllMain never runs.
 *
 *    Excerpt:
 * ========================================================================= */
/* basename / basename_len are computed earlier in build_module; `nt` is the
 * NT headers; `module` is the mapped image base; `map_size` is the mapping
 * size; `lsteamclient` is a HMODULE that may already point at a loaded
 * lsteamclient.dll, otherwise loaded on demand here. */

if (use_lsteamclient()
    && ((is_steamclient32 = !RtlCompareUnicodeStrings(basename, basename_len, L"steamclient",            11, TRUE))
        ||                  !RtlCompareUnicodeStrings(basename, basename_len, L"steamclient64",          13, TRUE)
        ||                  !RtlCompareUnicodeStrings(basename, basename_len, L"gameoverlayrenderer",    19, TRUE)
        ||                  !RtlCompareUnicodeStrings(basename, basename_len, L"gameoverlayrenderer64",  21, TRUE))
    && RtlCreateUnicodeStringFromAsciiz(&lsteamclient_us, "lsteamclient.dll")
    && (lsteamclient
        || LdrLoadDll(load_path, 0, &lsteamclient_us, &lsteamclient) == STATUS_SUCCESS))
{
    struct steamclient_setup_trampolines_params params = {
        .src_mod = *module,
        .tgt_mod = lsteamclient,
    };
    WINE_UNIX_CALL( unix_steamclient_setup_trampolines, &params );
    NtFlushInstructionCache( NtCurrentProcess(), *module, map_size );

    if (is_steamclient32)
    {
        /* ... 32-bit-specific image-base fixup ... */
    }
    else
    {
        wm->ldr.Flags |= LDR_DONT_CALL_DLLMAIN;
    }
}


/* =========================================================================
 * 4. dlls/ntdll/loader.c  ::  RtlProcessFlsData call sites (lines 4111,
 *    4146, 4178)
 *    THREE call sites — process shutdown, thread shutdown, and TLS free.
 * ========================================================================= */

/* (a) LdrShutdownProcess context, around line 4111 */
{
    process_detaching = TRUE;
    if (!detaching)
        RtlProcessFlsData( NtCurrentTeb()->FlsSlots, 1 );

    process_detach();
}

/* (b) LdrShutdownThread context, around line 4146 */
void WINAPI LdrShutdownThread(void)
{
    /* ... preamble ... */

    /* don't do any detach calls if process is exiting */
    if (process_detaching) return;

    RtlProcessFlsData( NtCurrentTeb()->FlsSlots, 1 );

    RtlEnterCriticalSection( &loader_section );
    /* ... iterate InInitializationOrderModuleList Blink-first calling
     *     DLL_THREAD_DETACH ... */
}

/* (c) TLS / thread-free context, around line 4178 */
{
    /* ... free TLS expansion slots ... */
    RtlProcessFlsData( NtCurrentTeb()->FlsSlots, 2 );
    NtCurrentTeb()->FlsSlots = NULL;
    RtlFreeHeap( GetProcessHeap(), 0, NtCurrentTeb()->TlsExpansionSlots );
    NtCurrentTeb()->TlsExpansionSlots = NULL;
    RtlReleasePebLock();
}

/* NOTE: in our reproducer the main thread hits RtlProcessFlsData even
 * though no obvious thread/process shutdown is in flight. The likely
 * trigger is wine-10/11's loader running this as part of failure-cleanup
 * after steamclient64.dll's DllMain bailed. Adding the module-range
 * guard in (5) below is what prevents the SECONDARY AV.
 */


/* =========================================================================
 * 5. dlls/ntdll/thread.c  ::  RtlProcessFlsData body (line 698-740)
 *    This is THE function whose `callback(...)` invocation faults in our
 *    reproducer. The NULL / (void*)~0 guard is the ONLY validation. We
 *    propose adding a `RtlPcToFileHeader` module-range check below — see
 *    `Proton-Findings.md` §4.A for the diff.
 * ========================================================================= */
void WINAPI DECLSPEC_HOTPATCH RtlProcessFlsData( void *teb_fls_data, ULONG flags )
{
    TEB_FLS_DATA *fls = teb_fls_data;
    unsigned int i, index;

    TRACE_(thread)( "teb_fls_data %p, flags %#lx.\n", teb_fls_data, flags );

    if (flags & ~3)
        FIXME_(thread)( "Unknown flags %#lx.\n", flags );

    if (!fls)
        return;

    if (flags & 1)
    {
        lock_fls_data();
        for (i = 0; i < ARRAY_SIZE(fls->fls_data_chunks); ++i)
        {
            if (!fls->fls_data_chunks[i]
                || !fls_data.fls_callback_chunks[i]
                || !fls_data.fls_callback_chunks[i]->count)
                continue;

            for (index = 0; index < fls_chunk_size( i ); ++index)
            {
                PFLS_CALLBACK_FUNCTION callback =
                    fls_data.fls_callback_chunks[i]->callbacks[index].callback;

                if (!fls->fls_data_chunks[i][index + 1])
                    continue;

                if (callback && callback != (void *)~(ULONG_PTR)0)
                {
                    /* WinNative / Proton-Findings.md §4.A — proposed guard:
                     *
                     *   void *image_base = NULL;
                     *   RtlPcToFileHeader( (PVOID)callback, &image_base );
                     *   if (!image_base) {
                     *       ERR_(thread)("Stale FLS callback %p — skipped.\n", callback);
                     *       fls->fls_data_chunks[i][index + 1] = NULL;
                     *       continue;
                     *   }
                     */

                    TRACE_(relay)("Calling FLS callback %p, arg %p.\n",
                                  callback, fls->fls_data_chunks[i][index + 1]);

                    callback( fls->fls_data_chunks[i][index + 1] );   /* <-- AV here in reproducer */
                }
                fls->fls_data_chunks[i][index + 1] = NULL;
            }
        }
        /* Not using RemoveEntryList() as Windows does not zero list entry here. */
        fls->fls_list_entry.Flink->Blink = fls->fls_list_entry.Blink;
        fls->fls_list_entry.Blink->Flink = fls->fls_list_entry.Flink;
        unlock_fls_data();
    }

    if (flags & 2)
    {
        for (i = 0; i < ARRAY_SIZE(fls->fls_data_chunks); ++i)
            RtlFreeHeap( GetProcessHeap(), 0, fls->fls_data_chunks[i] );

        RtlFreeHeap( GetProcessHeap(), 0, fls );
    }
}


/* =========================================================================
 * 6. dlls/ntdll/loader.c  ::  get_env  (line 209-222)
 *    The helper use_lsteamclient calls. Reads ONLY the PEB env (not libc).
 *    This is why a "set the env var in the process" experiment must
 *    propagate through Wine's PEB construction, not just libc setenv.
 * ========================================================================= */
static BOOL get_env( const WCHAR *var, WCHAR *val, unsigned int len )
{
    UNICODE_STRING name, value;

    name.Length = wcslen( var ) * sizeof(WCHAR);
    name.MaximumLength = name.Length + sizeof(WCHAR);
    name.Buffer = (WCHAR *)var;

    value.Length = 0;
    value.MaximumLength = len;
    value.Buffer = val;

    return !RtlQueryEnvironmentVariable_U( NULL, &name, &value );
}


/* =========================================================================
 * END
 * ========================================================================= */
