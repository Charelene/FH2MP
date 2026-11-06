/* Only one tool identifier leaves the client (NitroSystem, which registers this exact platform name). This is a compatibility/pool signal,
 * not tamper-proof attestation. Redline.exe is not reported any more (user decision 2026-11-05): older shims still send "redline" and the
 * servers ignore it. */

static LONG scan_restricted_tools(void)
{
    LONG found = 0;
    if (g_api && g_api->mod_version && g_api->mod_version("NitroSystem")) {
        found |= 1;
    }
    if (GetModuleHandleW(L"FnFNitro.dll")) {
        found |= 1;
    }
    return found;
}

static void restricted_tools_header(WCHAR *out)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static DWORD checked;
    static LONG tools;
    DWORD now = GetTickCount();
    AcquireSRWLockExclusive(&lock);
    if (!checked || now - checked >= 5000) {
        tools = scan_restricted_tools();
        checked = now;
    }
    lstrcpyW(out, L"X-FH2MP-Tools: v1");
    if (tools & 1) lstrcatW(out, L",nitrosystem");
    ReleaseSRWLockExclusive(&lock);
}
