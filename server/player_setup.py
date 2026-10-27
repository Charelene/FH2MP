"""LobbyPlayer transfer codec, Xbox One executable 1.0.0.64.

Registration order: PlayerData (17f3230, first 12), LobbyPlayer (180bf50,
including 12 PlayerChatState groups at 1809e00), PlayerChoices (17e7bb0),
CarInitSynced (17e3db0), InstalledPartsSynced (17df240), CarTuneDefSynced
(17e0800), two CarTuneSuspTireAttribSynced groups (17dfa80), color (17e2790),
and SyncedRaceData (1820510). Wire ids are 8 bits; arrays are fixed width.
Negative widths below are bounded UTF-16 string lengths, with a prefix of
max_chars.bit_length() bits. This codec never searches within value bits.
"""

ID_BITS = 8
MODEL = 103
WHEEL_STYLE = 106
PARTS = 107
USE_STOCK = 144
TUNE_IDS = tuple(range(108, 144))
_WIDTHS = (
    64, 1, 1, -16, 64, 1, 32, 1, 32, 32, 32, 32, 1, 1, 1, 1, 1, 1,
    1, 4, 3, 16, 16, 32, 32, 32, 32, 32, 32, 1, 7, 8, 1, 1, 1, 1,
    1, 32, 2, -4, 5, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64,
    1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64,
    1, 1, 64, 1, 1, 3, 17, 16, 16, 32, 32, 32, 32, 32, 32, 32, 32, 1,
    1, 3, 32, 64, 64, 2, 2, 2, 1, 1, 2, 1, 3, 32, 32, 32, 32, 1376,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 288, 32, 32, 32, 32,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
    1, 32, 32, 32, 32, 1, 1, 1, 1, 1, 2, 2, 2, 2, 32, 32, 32, 32,
)


def fields(size, payload):
    """Return id -> (value offset, width, value); reject malformed deltas."""
    if not 0 <= size < 1 << 14 or payload < 0 or payload.bit_length() > size:
        raise ValueError("invalid player object size")
    pos, previous, out = 0, -1, {}
    while pos < size:
        if size - pos < ID_BITS:
            raise ValueError("truncated player variable id")
        vid = (payload >> pos) & 255
        pos += ID_BITS
        if vid <= previous or vid >= len(_WIDTHS):
            raise ValueError("unknown or unordered player variable")
        width = _WIDTHS[vid]
        if width < 0:
            prefix = (-width).bit_length()
            if pos + prefix > size:
                raise ValueError("truncated player string length")
            count = (payload >> pos) & ((1 << prefix) - 1)
            if count > -width:
                raise ValueError("oversize player string")
            width = prefix + 16 * count
        if pos + width > size:
            raise ValueError("truncated player variable value")
        out[vid] = pos, width, (payload >> pos) & ((1 << width) - 1)
        previous, pos = vid, pos + width
    return out


def encode(values):
    """Encode id -> raw value with schema widths; strings include their prefix."""
    size, payload = 0, 0
    for vid, value in sorted(values.items()):
        if not 0 <= vid < len(_WIDTHS) or value < 0:
            raise ValueError("invalid player variable")
        width = _WIDTHS[vid]
        if width < 0:
            prefix = (-width).bit_length()
            count = value & ((1 << prefix) - 1)
            if count > -width:
                raise ValueError("oversize player string")
            width = prefix + 16 * count
        if value.bit_length() > width:
            raise ValueError("player value does not fit")
        payload |= (vid | (value << ID_BITS)) << size
        size += ID_BITS + width
    if size >= 1 << 14:
        raise ValueError("player object too large")
    return size, payload


def part_words(value):
    return [(value >> (32 * k)) & 0xFFFFFFFF for k in range(43)]


def parts_value(words):
    if len(words) != 43 or any(not 0 <= w <= 0xFFFFFFFF for w in words):
        raise ValueError("invalid installed parts array")
    return sum(w << (32 * k) for k, w in enumerate(words))


def stock_correction(words, choices=None):
    """A complete stock setup delta, so omitted tune values cannot survive repair.

    -1 is the native per-slider default, zero the GearRatios array default.
    UseStockSetup requests stock construction. No save/garage writes are made.
    """
    values = {PARTS: parts_value(words), USE_STOCK: 1,
              WHEEL_STYLE: 0, 94: 0xFFFFFFFFFFFFFFFF}
    values.update({vid: 0xBF800000 for vid in TUNE_IDS})
    values[121] = 0
    if choices is not None:
        import struct
        values[81] = choices[0]
        values[82] = struct.unpack("<I", struct.pack("<f", choices[1]))[0]
    return values
