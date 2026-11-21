/*
 * quickchat_bridge.h -- QuickChat's interface for the mod that carries its messages online (GPL-3.0, see LICENSE).
 * A copy of the QuickChat mod's include/quickchat.h: FH2MP is that mod. Keep it in step with QuickChat's.
 *
 * QuickChat sends no network traffic itself. It publishes "QuickChat.Bridge" from its fh2mod_init,
 * and the online mod (FH2MP's) reads it after wait_for_mods:
 *
 *     const QuickChatBridge* qc = (const QuickChatBridge*)api->get_interface(QUICKCHAT_BRIDGE, 1);
 *     if (!qc) ...   (QuickChat isn't installed: carry on without it)
 *
 * Only a phrase number ever needs to cross the network: every player has the same fixed list, so
 * there's no free text to check, and each player can see the phrase in their own language. Phrase
 * numbers never change meaning; new phrases only ever go at the end (phrase_count grows).
 *
 * Messages are state, not events: `latest` gives the newest message this player sent, with a number
 * that goes up by one per message. Send it until the server has it; the receiving side shows each
 * (player, seq) once, so a repeat is harmless and a lost packet heals on the next one.
 *
 * Interface rules: members are only ever added at the end, and `version` goes up when they are.
 */
#ifndef QUICKCHAT_H
#define QUICKCHAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUICKCHAT_BRIDGE "QuickChat.Bridge"
#define QUICKCHAT_BRIDGE_VERSION 1

typedef struct QuickChatBridge {
    uint32_t size;          /* sizeof(QuickChatBridge) as QuickChat knows it */
    uint32_t version;       /* QUICKCHAT_BRIDGE_VERSION */
    uint32_t phrase_count;  /* phrases 0 to phrase_count - 1 exist */

    /* The newest message this player sent: returns 1 with its number (1, 2, 3 ... for this run of the
       game), its phrase and GetTickCount64() when it was sent; 0 when nothing was sent yet. Any thread. */
    int (*latest)(uint32_t* seq, uint32_t* phrase, uint64_t* sent_ms);

    /* Another player's message: `from` is a number that stays the same for that player while they're
       in the session (the server's owner id), `name` their gamertag (UTF-8, may be NULL), `seq` their
       message number and `phrase` the phrase. Shown once per (from, seq); a phrase this QuickChat
       doesn't know is ignored. Returns 1 if it will be shown. Any thread. */
    int (*deliver)(uint32_t from, const char* name, uint32_t seq, uint32_t phrase);

    /* The phrase's text in English (UTF-8), or NULL past phrase_count. For logs. */
    const char* (*phrase_text)(uint32_t phrase);
} QuickChatBridge;

#ifdef __cplusplus
}
#endif

#endif
