/* Sharing a detailed design fails with "There was an error communicating with the Forza Horizon 2 server" and no request leaves the PC
 * (a player, 2026-11-15: strings/validate accepted, no StorefrontLivery.Create at nginx or the LSP, RemoveReference a few seconds later).
 * Why [confirmed: the exe's code; that the reporting player's design is over the limit is inferred, the request never reaches the server]:
 * PopupShareLivery (game+0xe412b0) calls StorefrontLivery.Create (game+0xf14b30) with two memory-stream blobs, the car photo (previewData)
 * and a second one, the design as the engine writes it (a CDynamicMemoryStream; the schema's trailing blob, which the LSP does not read).
 * The transport serializes the parameters in the calling thread (game+0xeea3b0 -> +0xf03710 -> +0xf034f0 -> the struct writer +0xee6640), and
 * the writer of a byte blob (+0xee6550) answers XLS_E_DATA_SIZE (0x8050000c) for one longer than 0x80000. The share screen reports every
 * negative answer through its error handler (popup+0x460), which is that generic dialog. Of 235 shared liveries on prod since 2026-11-02
 * the second blob was up to 518,178 bytes and none passed 524,288; it follows the design's content, not its layer count.
 * The limit is the Xbox service's; the LSP reads bodies up to 16 MiB. This raises it to 4 MiB, so the whole design goes up as the game made
 * it. The one change is the imm32 of that compare; every blob field of every request gets the same room, nothing else in the request
 * changes. Nothing else on the way checks a size (the buffer is allocated to the serialized length; the shim takes request bodies up to 64
 * MiB). FH2MP_NO_BLOBLIMIT=1 switches it off. */
#include "blob_limit_rule.inc"

static int patch_blob_limit(void)
{
    unsigned char *img = find_fh2_image();
    if (!img || !code_is("bloblimit", "the request blob writer", img, BL_FN, g_bl_head, sizeof g_bl_head) ||
        !code_is("bloblimit", "the blob size test", img, BL_FN + BL_SITE_OFF, g_bl_site, sizeof g_bl_site) ||
        !code_is("bloblimit", "the blob size answer", img, BL_FN + BL_FAIL_OFF, g_bl_fail, sizeof g_bl_fail)) {
        return 0;
    }
    if (!plat_patch("bloblimit: request blob limit", img + BL_FN + BL_IMM_OFF, g_bl_old, g_bl_new, sizeof g_bl_new)) {
        return 0;
    }
    log_begin();
    log_raw("hook: bloblimit: a byte blob in a web request may be 4 MiB, not 512 KiB (game+0xee6550); a detailed design's share is sent, not refused with XLS_E_DATA_SIZE\n");
    return 1;
}
