/* Remote car slots (CarCacheModule::vfunc_2, game+0x47a570).
 * The game builds a car cache at module init with two groups: "Player LOD0" with 1 slot (the local car) and "Non-LOD0" with min(NumAICars if set else 11,
 * 12) slots, a pre-built ForzaCar each. A remote player's car takes a Non-LOD0 slot (CarNetworkedModule::OnNetworkPlayerJoined -> request queue ->
 * helper_47ba20); with no free slot the request stays queued, so the 12th car of a world never exists and its RemoteCarPrediction is never reached. A real
 * client with 17 or 75 players showed exactly 11 remote cars (dev 2026-11-02).
 * Patched: the 11 (imm32 of `mov r9d, 0xb` at +0x28) becomes FH2MP_CAR_SLOTS, and the clamp to 12 (`cmovge rax, rcx` at +0x4d) is nopped, so the group
 * holds that many slots. A user-set NumAICars still wins, unclamped. Not patched: NumAICars itself (it also sets AI/Drivatar counts). Unset FH2MP_CAR_SLOTS
 * changes nothing; the value is 11 to 75. Each slot is a whole ForzaCar built on demand: memory per slot was not measured. */
#define CAR_POOL_RVA 0x47a570
#define CAR_POOL_NATIVE 11
#define CAR_POOL_MAX 75

static int car_pool_from_env(void)
{
    char v[16];
    DWORD n = GetEnvironmentVariableA("FH2MP_CAR_SLOTS", v, sizeof v);
    int i, total = 0;

    if (n == 0 || n >= sizeof v) {
        return 0;
    }
    for (i = 0; v[i]; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return 0;
        }
        total = total * 10 + (v[i] - '0');
    }
    return total > CAR_POOL_NATIVE && total <= CAR_POOL_MAX ? total : 0;
}

static int patch_car_pool(void)
{
    static const unsigned char head[] = {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x40, 0xB8, 0xFE, 0xFF, 0xFF, 0xFF};
    static const unsigned char native[] = {0x41, 0xB9, 0x0B, 0x00, 0x00, 0x00, 0x85, 0xD2, 0x44, 0x0F, 0x49, 0xCA};   /* +0x26 */
    static const unsigned char clamp[] = {0x41, 0x83, 0xF9, 0x0C, 0x48, 0x0F, 0x4D, 0xC1};                            /* +0x49 */
    static const unsigned char nop4[] = {0x90, 0x90, 0x90, 0x90};
    unsigned char *img = find_fh2_image();
    unsigned char slots[4] = {0, 0, 0, 0};
    int want = car_pool_from_env(), done = 0;

    if (!want || !img) {
        return 0;
    }
    if (!code_is("carpool", "the car cache's module init", img, CAR_POOL_RVA, head, sizeof head) ||
        !code_is("carpool", "the Non-LOD0 slot count (11)", img, CAR_POOL_RVA + 0x26, native, sizeof native) ||
        !code_is("carpool", "the clamp to 12 slots", img, CAR_POOL_RVA + 0x49, clamp, sizeof clamp)) {
        return 0;
    }
    slots[0] = (unsigned char)want;
    done += plat_patch("carpool: Non-LOD0 car slots", img + CAR_POOL_RVA + 0x28, native + 2, slots, 4);
    done += plat_patch("carpool: no clamp to 12 slots", img + CAR_POOL_RVA + 0x4d, clamp + 4, nop4, 4);
    if (done == 2) {
        log_begin();
        log_raw("carpool: remote car slots 11 -> ");
        log_dec((unsigned long)want);
        log_raw(" (FH2MP_CAR_SLOTS)\n");
    }
    return done == 2;
}
