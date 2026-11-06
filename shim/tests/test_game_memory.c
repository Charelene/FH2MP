/* read_game_memory (src/game_image.inc): the same answers on Windows (copy only) and on Wine (range walk + copy + range walk), and on Windows no VirtualQuery. */
#include <windows.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FH2_TIMESTAMP 0x565501edu
#define FH2_SIZE_OF_IMAGE 0x3a18000u

static volatile LONG g_vq_calls;
static SIZE_T WINAPI counting_query(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length)
{
    InterlockedIncrement(&g_vq_calls);
    return VirtualQuery(address, info, length);
}
#define VirtualQuery counting_query

#include "src/game_image.inc"

int main(void)
{
    unsigned char *page = VirtualAlloc(NULL, 4 * 0x1000, MEM_RESERVE, PAGE_NOACCESS);
    unsigned char out[64], src[64];
    DWORD old;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    int wine = ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;      /* its own answer, not the shim's */

    assert(game_reads_need_range_check() == wine);
    assert(page);
    /* page 0 readable, page 1 PAGE_NOACCESS, page 2 guard, page 3 reserved only */
    assert(VirtualAlloc(page, 3 * 0x1000, MEM_COMMIT, PAGE_READWRITE));
    memset(page, 0x5a, 0x1000);
    assert(VirtualProtect(page + 0x1000, 0x1000, PAGE_NOACCESS, &old));
    assert(VirtualProtect(page + 0x2000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old));
    for (size_t i = 0; i < sizeof src; i++) src[i] = (unsigned char)(i * 7 + 1);

    g_vq_calls = 0;
    memset(out, 0, sizeof out);
    assert(read_game_memory(src, out, sizeof src) == 1 && !memcmp(out, src, sizeof src));        /* ordinary memory (the stack) */
    assert(read_game_memory(page, out, 16) == 1 && out[0] == 0x5a && out[15] == 0x5a);          /* a committed page */
    assert(read_game_memory(page + 0x1000 - 16, out, 16) == 1);                                   /* its last 16 bytes */
    if (!wine) {
        assert(g_vq_calls == 0);                                                                  /* the point of the fast path */
    } else {
        assert(g_vq_calls > 0);                                                                   /* Wine keeps its range walks */
    }

    assert(read_game_memory(NULL, out, 8) == 0);                                                  /* nothing there */
    assert(read_game_memory((void *)(ULONG_PTR)8, out, 8) == 0);
    assert(read_game_memory(page + 0x1000, out, 8) == 0);                                         /* PAGE_NOACCESS */
    assert(read_game_memory(page + 0x1000 - 8, out, 16) == 0);                                    /* readable into no-access: no partial answer */
    assert(read_game_memory(page + 0x3000, out, 8) == 0);                                         /* reserved, not committed */
    assert(read_game_memory(page + 0x2000 - 8, out, 16) == 0);                                    /* into the next page, which is PAGE_NOACCESS */
    assert(read_game_memory(page + 0x2000, out, 8) == 0);                                         /* a guard page is not read */
    assert(read_game_memory(src, out, 0) == 1);                                                   /* zero bytes: nothing to copy */
    assert(read_game_memory(src, out, (SIZE_T)-1) == 0);                                          /* a range that wraps */
    assert(VirtualFree(page, 0, MEM_RELEASE));
    assert(read_game_memory(page, out, 8) == 0);                                                  /* unmapped after the fact */

    /* a mapping revoked between two reads of the same address fails the second one */
    {
        unsigned char *m = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        assert(m && read_game_memory(m, out, 8) == 1);
        assert(VirtualProtect(m, 0x1000, PAGE_NOACCESS, &old));
        assert(read_game_memory(m, out, 8) == 0);
        assert(VirtualFree(m, 0, MEM_RELEASE));
    }
    printf("game memory reads: ok (%s)\n", wine ? "Wine path" : "Windows path");
    return 0;
}
