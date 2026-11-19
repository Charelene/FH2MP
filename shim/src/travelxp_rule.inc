/* The pure decision of travelxp_guard.inc (tested natively by tests/test_travelxp_guard.sh). */

/* The divisor the game's rounding gets at game+0x66a492 (idiv ecx): the component's RoundXPTo, or 1 when it is 0 (no rounding). Only ecx's low 32 bits
 * are the divisor; the rest of rcx is kept. */
static unsigned long long txg_divisor(unsigned long long rcx)
{
    return (unsigned)rcx ? rcx : (rcx & ~0xffffffffull) | 1u;
}

/* The game's own rounding at game+0x66a48d..0x66a49a, for the test: (xp - 1 + step) / step, one less when the remainder is negative, times step: xp
 * rounded up to a multiple of step. */
static int txg_round(int xp, int step)
{
    int n = xp - 1 + step, q = n / step, r = n % step;

    if (r < 0) {
        q--;
    }
    return q * step;
}
