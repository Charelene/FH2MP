/* The pure decisions of tune_gate.inc (tested natively by tests/test_tune_gate.sh). */

/* 1 when the Tune Car handler was called from outside the game image (a mod's press). No caller noted counts as the game's own. */
static int tg_mod_press(unsigned long long from, unsigned long long img, unsigned long long img_size)
{
    return from != 0 && img != 0 && (from < img || from - img >= img_size);
}

/* 1 when a mod's press in a network session must not open the tuning screen: the game's own tile could not be pressed now. */
static int tg_refuses(int tile_enabled, int tab_enabled)
{
    return !tile_enabled || !tab_enabled;
}
