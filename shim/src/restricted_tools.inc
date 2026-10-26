/* Only two tool identifiers leave the client. This is a compatibility/pool signal, not tamper-proof attestation.
 * NitroSystem registers this exact platform name; Redline's published executable is Redline.exe. */
#include <tlhelp32.h>

static LONG scan_restricted_tools(void)
{
    LONG found = 0;
    DWORD session, other;
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    if (g_api && g_api->mod_version && g_api->mod_version("NitroSystem")) {
        found |= 1;
    }
    if (GetModuleHandleW(L"FnFNitro.dll")) {
        found |= 1;
    }
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        return found;
    }
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return found;
    }
    entry.dwSize = sizeof entry;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (lstrcmpiW(entry.szExeFile, L"Redline.exe") == 0 &&
                ProcessIdToSessionId(entry.th32ProcessID, &other) && session == other) {
                found |= 2;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
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
    if (tools & 2) lstrcatW(out, L",redline");
    ReleaseSRWLockExclusive(&lock);
}
