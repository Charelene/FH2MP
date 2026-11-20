/* The pure part of carlist_room.inc (tested natively by tests/test_carlist_room.sh): the growth rule of the game's std::vector<ForzaCar*> as its own
 * code has it, so the test can count how often a list moves with and without the room the guard makes. */

#define CRG_ROOM 256u     /* cars the list holds before it has to move again */

/* game+0x305a20 (vec, n): when capacity - size < n, the new capacity is max(size + n, capacity + capacity / 2) (then game+0x305150 moves the
 * elements); otherwise nothing changes. -> the capacity after the call. */
static unsigned long long crg_reserve(unsigned long long size, unsigned long long capacity, unsigned long long n)
{
    unsigned long long grown;

    if (capacity - size >= n) {
        return capacity;
    }
    grown = capacity + capacity / 2;
    return grown < size + n ? size + n : grown;
}
