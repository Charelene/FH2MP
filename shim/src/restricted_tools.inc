/* X-FH2MP-Tools on every web request. No tool is reported any more: Redline.exe stopped being one 2026-11-05, NitroSystem 2026-11-07
 * (it is offline-only on FH2 Mod Platform 2.8.0, which holds it back online; the old check also saw its DLL loaded on the first online
 * launch, when the platform had refused to start it, and put the player in the restricted pool). Servers judge driving by the car's own
 * physics instead. The header stays: a server in FH2MP_RESTRICTED_TOOLS=quarantine refuses a request without "v1". */

static void restricted_tools_header(WCHAR *out)
{
    lstrcpyW(out, L"X-FH2MP-Tools: v1");
}
