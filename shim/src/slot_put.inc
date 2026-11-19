/*
 * Import slots Azimuth fills at run time (patch_game_winsock, xhr2.inc) are written by slot_put, and their pages are left writable:
 * restoring the old protection raced with Azimuth, which binds the game's imports on other threads (0xc0000005 inside memcpy).
 *
 * Azimuth also sets the game's whole .rdata (the import block with it) back to read-only on its own thread as the game starts, so the
 * page can be read-only again between our VirtualProtect and the store: a plain store then faults (#472: 0xc0000005 writing
 * game+0x1dd0d80, the dump's region game+0x1dd0000..+0x2c47000 PAGE_READONLY in one piece). WriteProcessMemory reports that case as
 * an error instead. -> 1 when the slot holds `want`; 0 with *err set, and the slot keeps its old value (the monitor loop calls the
 * patchers again on its next pass). Tested by tests/test_slot_put.c.
 */
static int slot_put(void **slot, void *want, DWORD *err)
{
    DWORD old;
    SIZE_T w = 0;

    *err = 0;
    if (!VirtualProtect(slot, sizeof want, PAGE_READWRITE, &old)) {
        *err = GetLastError();
        return 0;
    }
    if (!WriteProcessMemory(GetCurrentProcess(), slot, &want, sizeof want, &w) || w != sizeof want) {
        *err = GetLastError();
        return 0;
    }
    return 1;
}
