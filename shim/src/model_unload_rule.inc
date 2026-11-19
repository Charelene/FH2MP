/* The pure decision of model_unload_guard.inc (tested natively by tests/test_model_unload.sh). */

#define MU_ZF 0x40ull

/* At the je after the car's key test: 1 when the event is this car's (ZF clear), the car has no model loader and the event asks for the
 * immediate swap to NUL_CAR_00 (its byte +0x20 is 0): the branch would call through the NULL loader. */
static int mu_skip(unsigned long long rflags, unsigned long long loader, unsigned char deferred)
{
    return !(rflags & MU_ZF) && loader == 0 && deferred == 0;
}
