/* slot_put (src/slot_put.inc): an import slot whose page another thread keeps setting back to read-only (Azimuth, #472) is either
 * written whole or left as it was, and never faults. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>

static void **g_slot;
static unsigned char *g_page;
static volatile LONG g_stop;
static volatile LONG g_flips;

/* In #472 a log line was written between the protection change and the store; the yield here opens the same window, so the
 * re-protecting thread gets in between (a plain store there faults). */
static BOOL WINAPI widened_protect(LPVOID address, SIZE_T size, DWORD prot, PDWORD old)
{
    BOOL r = VirtualProtect(address, size, prot, old);
    LONG seen = g_flips;
    int spin;
    for (spin = 0; spin < 2000 && g_flips == seen && !g_stop; spin++) {
        SwitchToThread();
    }
    return r;
}
#define VirtualProtect widened_protect
#include "src/slot_put.inc"
#undef VirtualProtect

/* Azimuth's side: the page goes back to read-only over and over */
static DWORD WINAPI reprotect(LPVOID unused)
{
    DWORD old;
    (void)unused;
    while (!g_stop) {
        if (VirtualProtect(g_page, 0x1000, PAGE_READONLY, &old)) {
            InterlockedIncrement(&g_flips);
        }
    }
    return 0;
}

int main(void)
{
    unsigned char *page = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void *a = (void *)(ULONG_PTR)0x1111111111111111ull, *b = (void *)(ULONG_PTR)0x2222222222222222ull, *want, *before;
    DWORD old, err;
    SIZE_T w = 0;
    HANDLE t;
    long ok = 0, refused = 0, i;

    assert(page);
    g_page = page;
    g_slot = (void **)(page + 0xd80);
    *g_slot = a;
    assert(VirtualProtect(page, 0x1000, PAGE_READONLY, &old));

    /* a read-only slot is made writable and written */
    assert(slot_put(g_slot, b, &err) == 1 && err == 0 && *g_slot == b);
    /* the property the fix rests on: a store into a page that went read-only again is an error, not an access violation */
    assert(VirtualProtect(page, 0x1000, PAGE_READONLY, &old));
    want = a;
    if (!WriteProcessMemory(GetCurrentProcess(), g_slot, &want, sizeof want, &w)) {
        printf("WriteProcessMemory into a read-only page: refused, error %lu\n", GetLastError());
        assert(*g_slot == b);
    } else {
        printf("WriteProcessMemory into a read-only page: written (%lu bytes)\n", (unsigned long)w);
        assert(w == sizeof want && *g_slot == a);
    }

    /* the race of #472: every call either writes the whole slot or leaves it as it was */
    t = CreateThread(NULL, 0, reprotect, NULL, 0, NULL);
    assert(t);
    while (g_flips == 0) {
        Sleep(0);
    }
    for (i = 0; i < 20000; i++) {
        want = (i & 1) ? a : b;
        before = *g_slot;
        if (slot_put(g_slot, want, &err)) {
            ok++;
            assert(*g_slot == want);
        } else {
            refused++;
            assert(err != 0 && *g_slot == before);
        }
    }
    g_stop = 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    printf("race: %ld written, %ld refused, %ld re-protections\n", ok, refused, (long)g_flips);
    assert(ok + refused == 20000);
    if (GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") == NULL) {
        assert(refused > 0); /* Windows: the window was hit (Wine's WriteProcessMemory may write through the protection) */
    }
    puts("test_slot_put: ok");
    return 0;
}
