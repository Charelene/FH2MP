// The Xbox 360 theme's sounds (sfx.h): the WAV files in memory, and a thread that mixes the ones playing into a waveOut stream of the
// system winmm.dll (the winmm.dll next to azimuth.exe is Extra Options' proxy). PlaySound would do for one sound, but it stops the one
// before: a focus tick would cut the Guide's opening off. The device is opened on the first sound and closed after three quiet seconds.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sfx.h"

namespace {

const int kRate = 44100, kBlock = 441, kBlocks = 4;  // 10 ms blocks, 40 ms queued
const int kVoices = 8, kQuietBlocks = 300;           // eight at once; closed after 3 s of silence

struct Player {
    std::vector<int16_t> pcm[int(Sfx::Count)];
    struct Voice {
        int id;
        size_t at;
    };
    std::mutex mu;
    std::vector<Voice> voices;
    HANDLE kick = nullptr;
    bool started = false;
    void (*log)(const char*) = nullptr;
    decltype(&waveOutOpen) open;
    decltype(&waveOutPrepareHeader) prepare;
    decltype(&waveOutUnprepareHeader) unprepare;
    decltype(&waveOutWrite) write;
    decltype(&waveOutReset) reset;
    decltype(&waveOutClose) close;
} g;

void logf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g.log) g.log(buf);
}

std::vector<unsigned char> read_file(const std::wstring& path) {
    std::vector<unsigned char> out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    if (size != INVALID_FILE_SIZE && size < (8u << 20)) {
        out.resize(size);
        if (!ReadFile(f, out.data(), size, &got, nullptr)) got = 0;
        out.resize(got);
    }
    CloseHandle(f);
    return out;
}

uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t le16(const unsigned char* p) { return uint16_t(p[0] | (p[1] << 8)); }

// A RIFF WAVE of 16-bit PCM at 44.1 kHz, mono or stereo (taken down to mono). Anything else: empty.
std::vector<int16_t> parse_wav(const std::vector<unsigned char>& f) {
    std::vector<int16_t> out;
    if (f.size() < 12 || memcmp(f.data(), "RIFF", 4) || memcmp(f.data() + 8, "WAVE", 4)) return out;
    int channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t p = 12;
    while (p + 8 <= f.size()) {
        uint32_t n = le32(&f[p + 4]);
        size_t body = p + 8;
        if (n > f.size() - body) break;
        if (!memcmp(&f[p], "fmt ", 4) && n >= 16) {
            if (le16(&f[body]) != 1) return out;  // PCM only
            channels = le16(&f[body + 2]), rate = le32(&f[body + 4]), bits = le16(&f[body + 14]);
        } else if (!memcmp(&f[p], "data", 4)) {
            if (bits != 16 || rate != kRate || (channels != 1 && channels != 2)) return out;
            size_t frames = n / (2 * size_t(channels));
            out.resize(frames);
            for (size_t i = 0; i < frames; i++) {
                const unsigned char* s = &f[body + i * 2 * channels];
                out[i] = channels == 1 ? int16_t(le16(s)) : int16_t((int(int16_t(le16(s))) + int(int16_t(le16(s + 2)))) / 2);
            }
            return out;
        }
        p = body + n + (n & 1);
    }
    return out;
}

bool load_winmm() {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    HMODULE m = n ? LoadLibraryW((std::wstring(sys, n) + L"\\winmm.dll").c_str()) : nullptr;
    if (!m) return false;
    g.open = (decltype(g.open))(void*)GetProcAddress(m, "waveOutOpen");
    g.prepare = (decltype(g.prepare))(void*)GetProcAddress(m, "waveOutPrepareHeader");
    g.unprepare = (decltype(g.unprepare))(void*)GetProcAddress(m, "waveOutUnprepareHeader");
    g.write = (decltype(g.write))(void*)GetProcAddress(m, "waveOutWrite");
    g.reset = (decltype(g.reset))(void*)GetProcAddress(m, "waveOutReset");
    g.close = (decltype(g.close))(void*)GetProcAddress(m, "waveOutClose");
    return g.open && g.prepare && g.unprepare && g.write && g.reset && g.close;
}

// One block of the sounds playing; false when nothing played in it.
bool mix(int16_t* outp) {
    int acc[kBlock] = {};
    std::lock_guard<std::mutex> l(g.mu);
    bool any = !g.voices.empty();
    for (size_t v = 0; v < g.voices.size();) {
        const std::vector<int16_t>& s = g.pcm[g.voices[v].id];
        size_t& at = g.voices[v].at;
        for (int i = 0; i < kBlock && at < s.size(); i++) acc[i] += s[at++];
        if (at >= s.size()) g.voices.erase(g.voices.begin() + long(v));
        else v++;
    }
    for (int i = 0; i < kBlock; i++) outp[i] = int16_t(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
    return any;
}

void play_thread() {
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    static int16_t mem[kBlocks][kBlock];
    bool told = false;
    for (;;) {
        WaitForSingleObject(g.kick, INFINITE);
        {
            std::lock_guard<std::mutex> l(g.mu);
            if (g.voices.empty()) continue;  // a kick that came while the last stream was still open
        }
        HWAVEOUT out = nullptr;
        WAVEFORMATEX fmt = {WAVE_FORMAT_PCM, 1, kRate, kRate * 2, 2, 16, 0};
        MMRESULT r = g.open(&out, WAVE_MAPPER, &fmt, (DWORD_PTR)done, 0, CALLBACK_EVENT);
        if (r != MMSYSERR_NOERROR) {
            if (!told) logf("guide: 360 sounds: no playback device (waveOutOpen 44.1 kHz mono: %u): they stay silent", unsigned(r)), told = true;
            std::lock_guard<std::mutex> l(g.mu);
            g.voices.clear();
            continue;
        }
        WAVEHDR hdr[kBlocks] = {};
        for (int i = 0; i < kBlocks; i++) {
            mix(mem[i]);
            hdr[i].lpData = (LPSTR)mem[i], hdr[i].dwBufferLength = kBlock * 2;
            g.prepare(out, &hdr[i], sizeof hdr[i]);
            g.write(out, &hdr[i], sizeof hdr[i]);
        }
        int next = 0, quiet = 0;
        while (quiet < kQuietBlocks) {
            WaitForSingleObject(done, 100);
            while (hdr[next].dwFlags & WHDR_DONE) {
                quiet = mix(mem[next]) ? 0 : quiet + 1;
                hdr[next].dwFlags &= ~WHDR_DONE;
                g.write(out, &hdr[next], sizeof hdr[next]);
                next = (next + 1) % kBlocks;
            }
        }
        g.reset(out);
        for (auto& h : hdr) g.unprepare(out, &h, sizeof h);
        g.close(out);
    }
}

}  // namespace

const char* sfx_file(Sfx s) {
    static const char* names[int(Sfx::Count)] = {"HUD_open.wav",   "HUD_close.wav",   "BladeOpen.wav",         "BladeSwitch_1.wav", "BladeSwitch_2.wav",
                                                  "BladeSwitch_3.wav", "BladeSwitch_4.wav", "btn_focusG.wav",    "btn_InactiveFocus.wav", "btn_selectG.wav",
                                                  "btn_InactiveSelect.wav", "btn_backG.wav", "tab_Switch.wav",    "vk_Focus.wav",      "vk_Select.wav",
                                                  "NotifyPopup.wav"};
    return int(s) >= 0 && int(s) < int(Sfx::Count) ? names[int(s)] : "";
}

int sfx_load(const std::wstring& dir, void (*log)(const char*)) {
    g.log = log;
    int n = 0;
    for (int i = 0; i < int(Sfx::Count); i++) {
        const char* f = sfx_file(Sfx(i));
        g.pcm[i] = parse_wav(read_file(dir + std::wstring(f, f + strlen(f))));
        n += !g.pcm[i].empty();
    }
    return n;
}

bool sfx_has(Sfx s) { return int(s) >= 0 && int(s) < int(Sfx::Count) && !g.pcm[int(s)].empty(); }

void sfx_play(Sfx s) {
    if (!sfx_has(s)) return;
    {
        std::lock_guard<std::mutex> l(g.mu);
        if (!g.started) {
            g.started = true;
            if (!load_winmm()) {
                logf("guide: 360 sounds: the system winmm.dll has no waveOut: they stay silent");
                g.open = nullptr;
            } else {
                g.kick = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                std::thread(play_thread).detach();
            }
        }
        if (!g.open) return;
        if (int(g.voices.size()) >= kVoices) g.voices.erase(g.voices.begin());
        g.voices.push_back({int(s), 0});
    }
    SetEvent(g.kick);
}
