/* What garage_rating_guard.inc leaves in a CCarInGarage it does not rate (tested natively by tests/test_garage_rating.sh): 0 in every field
 * game+0x2cfa50 would have written from the rating, except ClassID (+0x2cc, read from the save row by the ctor), +0x288 (written by
 * +0x2f5120) and the save row id (+0x2bc). The sample arrays at +0x2f4/+0x6cc are not touched: their count (+0xaa4) is 0. */
#include <string.h>

static void gr_clear(unsigned char *car_in_garage)
{
    memset(car_in_garage + 0x260, 0, 0x288 - 0x260);              /* ratings and simulated values */
    memset(car_in_garage + 0x28c, 0, 0x2bc - 0x28c);              /* shift/clutch times, mass differences, PI inputs */
    memset(car_in_garage + 0xaa4, 0, 4);                          /* no torque samples */
}
