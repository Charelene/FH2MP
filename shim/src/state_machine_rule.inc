/* The pure decision of state_machine_guard.inc (tested natively by tests/test_state_machine.sh). */

#define SM_ZF 0x40ull
#define SM_SF 0x80ull
#define SM_OF 0x800ull

/* At the jle after the node count test: 1 when the game would walk the node list (the jle is not taken: ZF clear and SF equal to OF) and
 * the list is NULL: the walk's first read would be of address 0. */
static int sm_skip(unsigned long long rflags, unsigned long long list)
{
    int taken = (rflags & SM_ZF) != 0 || ((rflags & SM_SF) != 0) != ((rflags & SM_OF) != 0);
    return !taken && list == 0;
}
