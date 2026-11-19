/* The byte-blob limit of the game's web requests (tested natively by tests/test_blob_limit.sh): what the game checks, where, and what it is
 * changed to. Why it matters is in blob_limit_guard.inc. The bytes are the game's code of build 1.0.0.64, read from the exe on disk. */
#define BL_FN 0xee6550u                 /* the writer of one byte-blob field (type 0x14) of a request's parameters */
#define BL_SITE_OFF 0xc6u               /* in it: cmp rbx,0x80000 ; jbe (back to the byte swap and the write) */
#define BL_IMM_OFF (BL_SITE_OFF + 3u)   /* the imm32 of that cmp */
#define BL_FAIL_OFF 0xddu               /* after it: mov eax,0x8050000c (XLS_E_DATA_SIZE) ; ... ; ret */
#define BL_LIMIT_OLD 0x80000u           /* 512 KiB: what the game allows */
#define BL_LIMIT_NEW 0x400000u          /* 4 MiB: below the 16 MiB the LSP reads (ugc.MaxRequestBody) and the 20 MiB of its nginx */

/* game+0xee6550: mov [rsp+8],rbx ; mov [rsp+0x10],rsi ; push rdi ; sub rsp,0x20 ; mov rsi,r8 (first 16 bytes) */
static const unsigned char g_bl_head[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x49};
/* +0xc6: cmp rbx,0x80000 ; jbe +0x1c (the limit is kept) */
static const unsigned char g_bl_site[10] = {0x48, 0x81, 0xfb, 0x00, 0x00, 0x08, 0x00, 0x0f, 0x86, 0x49};
/* +0xdd: mov eax,0x8050000c */
static const unsigned char g_bl_fail[5] = {0xb8, 0x0c, 0x00, 0x50, 0x80};
static const unsigned char g_bl_old[4] = {0x00, 0x00, 0x08, 0x00};   /* BL_LIMIT_OLD, little endian */
static const unsigned char g_bl_new[4] = {0x00, 0x00, 0x40, 0x00};   /* BL_LIMIT_NEW */

/* 1 when the immediate in `site` (g_bl_site's layout) is `want`, a limit the cmp's sign-extended imm32 can hold. */
static inline int bl_limit_is(const unsigned char *site, unsigned want)
{
    unsigned v = (unsigned)site[3] | (unsigned)site[4] << 8 | (unsigned)site[5] << 16 | (unsigned)site[6] << 24;
    return v == want && want < 0x80000000u;
}
