// Voice chat for FH2 (build 1.0.0.64): our own Microsoft.Xbox.GameChat.ChatManager (RE 2026-11-12, Notion "Voice A0: Microsoft's GameChat
// engine, read for its wire format"; local docs/re/voice.md).
//
// The game carries voice itself; only the GameChat engine is missing. Its chat layer (game+0x36d720, made when a multiplayer session
// starts, game+0xa140c0) activates the ChatManager in game+0x363a70, sets its ChatManagerSettings (game+0x374f30) and subscribes to
// OnOutgoingChatPacketReady (ABI +0x48, game+0x37acb0). Each ChatPacketEventArgs it gets is queued under a lock (game+0x378930) and sent
// on the game's thread (game+0x3835b0 -> game+0x374010): unreliable ones (get_SendReliable +0x38 = false) go into the voice queue
// (game+0x369630) and leave in their own datagrams after the messages as `0xC4A7:16 | size:16 | payload` (game+0x37f860, +0x176a860).
// The server relays them as `0xC4A7:16 | size:16 | source console id:64 | payload` (HeaderFromServer, read by game+0x1767030, size below
// 0x55d); the game's voice loop (game+0x379390, only with the option enablevoicechat) copies each payload into a buffer (game+0x36d830)
// and passes it with the boxed id (IReference<UINT64>) to ProcessIncomingChatMessage (ABI +0x88, game+0x3786b0).
//
// So the engine is ours on both ends: what is inside the payload is our format, and the console id is what our game server stamps (the
// sender's xuid). Reliable chat packets (NetV2 msg 33, 64 bytes at most) are never used.
//
// Sound (user decision 2026-11-12, "scuffed as on 360", bandwidth and CPU first): microphone at 8 kHz, a voice gate (silence sends
// nothing), Opus narrowband at 6 kbps in 40 ms frames, two frames per packet (about 10 kbps on the wire while talking); playback through
// the 360 headset chain: band-pass 300-3400 Hz, soft clip and an 8-bit mu-law round trip.
//
// Payload (ours): 'V' | version 1 | seq:16 | frames:8 | per frame: length:8, Opus bytes | zero padding to at least 16 bytes (the game's
// voice loop only reads a relayed packet while more than 0xc0 bits remain, so a short one would be skipped).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <inspectable.h>
#include <mmsystem.h>

#include <opus/opus.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fh2mod.h"
#include "voice.h"

namespace {

void (*g_log)(const char*);

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) g_log(buf);
}

const GUID IID_Inspectable = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
const GUID IID_Agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
const GUID IID_AsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID IID_AsyncAction = {0x5a648006, 0x843a, 0x4da9, {0x86, 0x5b, 0x9d, 0x26, 0xe5, 0xdf, 0xad, 0x7b}};
// Windows.Storage.Streams.IBuffer and IBufferByteAccess (the game asks for the second at game+0x370970, its IID at game+0x1e09f28)
const GUID IID_Buffer = {0x905a0fe0, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
const GUID IID_BufferByteAccess = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
// IReference<UINT64>, the boxed console id (game+0x1df80e8)
const GUID IID_RefUInt64 = {0x6755e376, 0x53bb, 0x568b, {0xa1, 0x1d, 0x17, 0x23, 0x98, 0x68, 0x30, 0x9e}};
// IChatManager (game+0x1df8188)
const GUID IID_ChatManager = {0x5ca654f2, 0x7640, 0x3bac, {0x9c, 0x35, 0x1a, 0x42, 0xa0, 0xe4, 0x1f, 0x38}};

// ---- small WinRT objects ------------------------------------------------------------------------------------------------------------

// IInspectable's three for objects of ours (no class name, no IIDs listed).
#define INSPECTABLE_BODY                                                         \
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override {      \
        if (count) *count = 0;                                                    \
        if (iids) *iids = nullptr;                                                \
        return S_OK;                                                              \
    }                                                                             \
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override {      \
        if (name) *name = nullptr;                                                \
        return S_OK;                                                              \
    }                                                                             \
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override {        \
        if (level) *level = BaseTrust;                                            \
        return S_OK;                                                              \
    }

struct IBufferAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Capacity(UINT32* n) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Length(UINT32* n) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Length(UINT32 n) = 0;
};
struct IByteAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE** bytes) = 0;
};

// A Windows.Storage.Streams.Buffer of ours: what we hand the game with each packet, and what the game's voice loop copies a relayed
// payload into (game+0x36d830 is hooked, so the engine needs no system Buffer class).
class Buf final : public IBufferAbi, public IByteAccess {
public:
    Buf(const void* data, size_t n) : bytes_((const BYTE*)data, (const BYTE*)data + n), length_(UINT32(n)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_Inspectable) || IsEqualGUID(iid, IID_Agile) || IsEqualGUID(iid, IID_Buffer))
            *out = static_cast<IBufferAbi*>(this);
        else if (IsEqualGUID(iid, IID_BufferByteAccess))
            *out = static_cast<IByteAccess*>(this);
        else
            return *out = nullptr, E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        long n = --refs_;
        if (n == 0) delete this;
        return ULONG(n);
    }
    INSPECTABLE_BODY
    HRESULT STDMETHODCALLTYPE get_Capacity(UINT32* n) override { return n ? (*n = UINT32(bytes_.size()), S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE get_Length(UINT32* n) override { return n ? (*n = length_, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE put_Length(UINT32 n) override { return n <= bytes_.size() ? (length_ = n, S_OK) : E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE Buffer(BYTE** b) override { return b ? (*b = bytes_.data(), S_OK) : E_POINTER; }

private:
    std::atomic<long> refs_{1};
    std::vector<BYTE> bytes_;
    UINT32 length_;
};

// Reads an IBuffer of anyone's: its bytes through IBufferByteAccess.
bool buffer_bytes(IUnknown* b, const BYTE** data, UINT32* n) {
    if (!b) return false;
    IBufferAbi* buf = nullptr;
    IByteAccess* access = nullptr;
    bool ok = false;
    if (SUCCEEDED(b->QueryInterface(IID_Buffer, (void**)&buf)) && buf && SUCCEEDED(buf->get_Length(n)) &&
        SUCCEEDED(b->QueryInterface(IID_BufferByteAccess, (void**)&access)) && access) {
        BYTE* p = nullptr;
        ok = SUCCEEDED(access->Buffer(&p)) && p;
        *data = p;
    }
    if (access) access->Release();
    if (buf) buf->Release();
    return ok;
}

struct IChatPacketArgs : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_PacketBuffer(IUnknown** b) = 0;               // +0x30
    virtual HRESULT STDMETHODCALLTYPE get_SendReliable(boolean* v) = 0;                 // +0x38
    virtual HRESULT STDMETHODCALLTYPE get_SendInOrder(boolean* v) = 0;                  // +0x40
    virtual HRESULT STDMETHODCALLTYPE get_SendPacketToAllConsoles(boolean* v) = 0;      // +0x48
    virtual HRESULT STDMETHODCALLTYPE get_UniqueTargetConsoleIdentifier(IInspectable** v) = 0;  // +0x50
    virtual HRESULT STDMETHODCALLTYPE get_ChatMessageType(INT32* v) = 0;                // +0x58
    virtual HRESULT STDMETHODCALLTYPE get_ChatUser(IInspectable** v) = 0;               // +0x60
};

// One voice packet for the game to send: unreliable, to every console (the server picks who hears it), in order.
class PacketArgs final : public IChatPacketArgs {
public:
    explicit PacketArgs(Buf* b) : buf_(b) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_Inspectable) || IsEqualGUID(iid, IID_Agile)) {
            *out = static_cast<IChatPacketArgs*>(this);
            AddRef();
            return S_OK;
        }
        return *out = nullptr, E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        long n = --refs_;
        if (n == 0) delete this;
        return ULONG(n);
    }
    INSPECTABLE_BODY
    HRESULT STDMETHODCALLTYPE get_PacketBuffer(IUnknown** b) override {
        if (!b) return E_POINTER;
        *b = static_cast<IBufferAbi*>(buf_);
        buf_->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SendReliable(boolean* v) override { return v ? (*v = 0, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE get_SendInOrder(boolean* v) override { return v ? (*v = 1, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE get_SendPacketToAllConsoles(boolean* v) override { return v ? (*v = 1, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE get_UniqueTargetConsoleIdentifier(IInspectable** v) override { return v ? (*v = nullptr, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE get_ChatMessageType(INT32* v) override { return v ? (*v = 1, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE get_ChatUser(IInspectable** v) override { return v ? (*v = nullptr, S_OK) : E_POINTER; }

private:
    ~PacketArgs() { buf_->Release(); }
    std::atomic<long> refs_{1};
    Buf* buf_;
};

struct IActionAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};
struct IInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// An IAsyncAction that is already Completed: what the channel and console calls return (the game makes PPL tasks of them,
// game+0x361ce0); its Completed handler runs at once, as WinRT does for a finished operation.
class DoneAction final : public IActionAbi, public IInfoAbi {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_Inspectable) || IsEqualGUID(iid, IID_Agile) || IsEqualGUID(iid, IID_AsyncAction))
            *out = static_cast<IActionAbi*>(this);
        else if (IsEqualGUID(iid, IID_AsyncInfo))
            *out = static_cast<IInfoAbi*>(this);
        else
            return *out = nullptr, E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        long n = --refs_;
        if (n == 0) delete this;
        return ULONG(n);
    }
    INSPECTABLE_BODY
    HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* h) override {
        if (!h) return S_OK;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (handler_) return HRESULT(0x80000018L);  // ILLEGAL_DELEGATE_ASSIGNMENT
            handler_ = h;
            h->AddRef();
        }
        // AsyncActionCompletedHandler::Invoke(asyncInfo, status) is the delegate's slot 3; 1 = Completed
        typedef HRESULT(STDMETHODCALLTYPE * InvokeFn)(IUnknown*, IActionAbi*, int);
        ((InvokeFn)(*(void***)h)[3])(h, static_cast<IActionAbi*>(this), 1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** h) override {
        if (!h) return E_POINTER;
        std::lock_guard<std::mutex> l(mu_);
        *h = handler_;
        if (handler_) handler_->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetResults() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) override { return id ? (*id = 1, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE get_Status(int* s) override { return s ? (*s = 1, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* c) override { return c ? (*c = S_OK, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE Cancel() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

private:
    ~DoneAction() {
        if (handler_) handler_->Release();
    }
    std::atomic<long> refs_{1};
    std::mutex mu_;
    IUnknown* handler_ = nullptr;
};

// ChatManagerSettings: the game only sets fields (game+0x374f30: +0x38 = 40, +0x48 = 4, +0x68 = 0, +0x88 = 3, +0xb8 = 0, +0xc8 = 1),
// so every slot past IInspectable's is a setter that keeps nothing.
struct RawObj {
    void** vt;
    std::atomic<long> refs;
};
HRESULT STDMETHODCALLTYPE raw_qi(RawObj* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_Inspectable) || IsEqualGUID(iid, IID_Agile)) {
        *out = self;
        ++self->refs;
        return S_OK;
    }
    return *out = nullptr, E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE raw_addref(RawObj* self) { return ULONG(++self->refs); }
ULONG STDMETHODCALLTYPE raw_release(RawObj* self) { return ULONG(--self->refs); }  // static object: never freed
HRESULT STDMETHODCALLTYPE raw_iids(RawObj*, ULONG* count, IID** iids) {
    if (count) *count = 0;
    if (iids) *iids = nullptr;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE raw_name(RawObj*, HSTRING* name) { return name ? (*name = nullptr, S_OK) : E_POINTER; }
HRESULT STDMETHODCALLTYPE raw_trust(RawObj*, TrustLevel* t) { return t ? (*t = BaseTrust, S_OK) : E_POINTER; }
HRESULT STDMETHODCALLTYPE raw_setter() { return S_OK; }

void* g_settings_vt[48];
RawObj g_settings = {g_settings_vt, {1}};

void init_settings() {
    void* head[6] = {(void*)raw_qi, (void*)raw_addref, (void*)raw_release, (void*)raw_iids, (void*)raw_name, (void*)raw_trust};
    for (int i = 0; i < 48; i++) g_settings_vt[i] = i < 6 ? head[i] : (void*)raw_setter;
}

// ---- the engine -----------------------------------------------------------------------------------------------------------------------

const int kFrameMs = 40, kFramesPerPacket = 2, kBitrate = 6000, kMaxFrameBytes = 120, kMinPayload = 16;
const int kJitterStart = 2;   // frames buffered before a talker is played (80 ms)
const int kJitterMax = 8;     // more than this and the oldest are dropped (320 ms)
const int kConcealFrames = 2; // lost frames filled by the decoder before a talker stops

// winmm from the system folder: the winmm.dll next to azimuth.exe is Extra Options' proxy.
struct Wave {
    decltype(&waveInOpen) inOpen;
    decltype(&waveInPrepareHeader) inPrepare;
    decltype(&waveInUnprepareHeader) inUnprepare;
    decltype(&waveInAddBuffer) inAdd;
    decltype(&waveInStart) inStart;
    decltype(&waveInReset) inReset;
    decltype(&waveInClose) inClose;
    decltype(&waveInGetDevCapsW) inCaps;
    decltype(&waveOutOpen) outOpen;
    decltype(&waveOutPrepareHeader) outPrepare;
    decltype(&waveOutUnprepareHeader) outUnprepare;
    decltype(&waveOutWrite) outWrite;
    decltype(&waveOutReset) outReset;
    decltype(&waveOutClose) outClose;
    bool ok = false;
} g_wave;

bool load_wave() {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    HMODULE m = n ? LoadLibraryW((std::wstring(sys, n) + L"\\winmm.dll").c_str()) : nullptr;
    if (!m) return false;
#define WAVE_FN(field, name) g_wave.field = (decltype(g_wave.field))(void*)GetProcAddress(m, name)
    WAVE_FN(inOpen, "waveInOpen");
    WAVE_FN(inPrepare, "waveInPrepareHeader");
    WAVE_FN(inUnprepare, "waveInUnprepareHeader");
    WAVE_FN(inAdd, "waveInAddBuffer");
    WAVE_FN(inStart, "waveInStart");
    WAVE_FN(inReset, "waveInReset");
    WAVE_FN(inClose, "waveInClose");
    WAVE_FN(inCaps, "waveInGetDevCapsW");
    WAVE_FN(outOpen, "waveOutOpen");
    WAVE_FN(outPrepare, "waveOutPrepareHeader");
    WAVE_FN(outUnprepare, "waveOutUnprepareHeader");
    WAVE_FN(outWrite, "waveOutWrite");
    WAVE_FN(outReset, "waveOutReset");
    WAVE_FN(outClose, "waveOutClose");
#undef WAVE_FN
    g_wave.ok = g_wave.inOpen && g_wave.inPrepare && g_wave.inUnprepare && g_wave.inAdd && g_wave.inStart && g_wave.inReset && g_wave.inClose &&
                g_wave.inCaps && g_wave.outOpen && g_wave.outPrepare && g_wave.outUnprepare && g_wave.outWrite && g_wave.outReset && g_wave.outClose;
    return g_wave.ok;
}

// The 360 headset: band-pass, soft clip, 8-bit mu-law.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(bool high, double f, double fs) {
        double w = 2 * 3.14159265358979323846 * f / fs, c = cos(w), s = sin(w), alpha = s / (2 * 0.7071), a0 = 1 + alpha;
        if (high)
            b0 = (1 + c) / 2, b1 = -(1 + c), b2 = (1 + c) / 2;
        else
            b0 = (1 - c) / 2, b1 = 1 - c, b2 = (1 - c) / 2;
        b0 /= a0, b1 /= a0, b2 /= a0, a1 = -2 * c / a0, a2 = (1 - alpha) / a0;
    }
    double run(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

double mulaw_round_trip(double x) {
    const double mu = 255;
    double s = x < 0 ? -1 : 1, m = fabs(x) > 1 ? 1 : fabs(x);
    double c = log1p(mu * m) / log1p(mu);
    c = floor(c * 127 + 0.5) / 127;  // 8 bits with the sign
    return s * (pow(1 + mu, c) - 1) / mu;
}

struct Talker {
    OpusDecoder* dec = nullptr;
    std::deque<std::vector<unsigned char>> frames;  // an empty frame = lost (the decoder conceals it)
    int expect = -1;  // the next seq
    bool playing = false;
    int concealed = 0;
    DWORD heard = 0;  // GetTickCount of the last packet
    Biquad hp, lp;
    uint64_t packets = 0, lost = 0;
};

struct Engine {
    std::mutex mu;
    std::map<uint64_t, Talker> talkers;
    IUnknown* outgoing = nullptr;  // the game's OnOutgoingChatPacketReady handler
    int64_t next_token = 1, outgoing_token = 0;
    std::atomic<bool> running{false};
    std::atomic<DWORD> mic_loud{0};  // GetTickCount when the gate was last open
    int out_rate = 8000;
    bool mic = true;
} g_engine;

IInspectable* chat_manager_self();

// Hands one packet to the game (any thread: the game's handler queues it under a lock).
void raise_packet(const std::vector<unsigned char>& payload) {
    IUnknown* h = nullptr;
    {
        std::lock_guard<std::mutex> l(g_engine.mu);
        h = g_engine.outgoing;
        if (h) h->AddRef();
    }
    if (!h) return;
    Buf* b = new Buf(payload.data(), payload.size());
    PacketArgs* a = new PacketArgs(b);
    // EventHandler<ChatPacketEventArgs^>::Invoke(sender, args): the delegate's slot 3 (game lambda at game+0x376940)
    typedef HRESULT(STDMETHODCALLTYPE * InvokeFn)(IUnknown*, IInspectable*, IChatPacketArgs*);
    ((InvokeFn)(*(void***)h)[3])(h, chat_manager_self(), a);
    a->Release();
    h->Release();
}

// The microphone: 40 ms blocks, the gate, Opus, a packet every two frames.
void capture_thread() {
    int rates[3] = {8000, 16000, 48000};
    HWAVEIN in = nullptr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int rate = 0;
    for (int r : rates) {
        WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, DWORD(r), DWORD(r * 2), 2, 16, 0};
        if (g_wave.inOpen(&in, WAVE_MAPPER, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
            rate = r;
            break;
        }
        in = nullptr;
    }
    if (!in) {
        logf("voice: no microphone could be opened (8, 16 or 48 kHz mono): this player only listens");
        CloseHandle(ev);
        return;
    }
    WAVEINCAPSW caps = {};
    g_wave.inCaps(WAVE_MAPPER, &caps, sizeof caps);
    char name[96] = "";
    WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name, sizeof name, nullptr, nullptr);
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &err);
    if (!enc) {
        logf("voice: the Opus encoder could not be made (error %d): this player only listens", err);
        g_wave.inClose(in);
        CloseHandle(ev);
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND));
    opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    const int n = rate * kFrameMs / 1000;
    const int kBlocks = 4;
    std::vector<short> mem(size_t(n) * kBlocks);
    WAVEHDR hdr[kBlocks] = {};
    for (int i = 0; i < kBlocks; i++) {
        hdr[i].lpData = (LPSTR)&mem[size_t(i) * n], hdr[i].dwBufferLength = DWORD(n * 2);
        g_wave.inPrepare(in, &hdr[i], sizeof hdr[i]);
        g_wave.inAdd(in, &hdr[i], sizeof hdr[i]);
    }
    g_wave.inStart(in);
    logf("voice: microphone open: %s at %d Hz (Opus narrowband %d bit/s, %d ms frames, %d per packet, a voice gate: silence sends nothing)",
         name[0] ? name : "the default device", rate, kBitrate, kFrameMs, kFramesPerPacket);
    double floor_rms = 300;
    int hang = 0, next = 0;
    uint16_t seq = 0;
    std::vector<std::vector<unsigned char>> pending;
    auto flush = [&] {
        if (pending.empty()) return;
        std::vector<unsigned char> p = {'V', 1, (unsigned char)(seq & 0xff), (unsigned char)(seq >> 8), (unsigned char)pending.size()};
        for (auto& f : pending) {
            p.push_back((unsigned char)f.size());
            p.insert(p.end(), f.begin(), f.end());
        }
        while (p.size() < size_t(kMinPayload)) p.push_back(0);
        raise_packet(p);
        pending.clear();
        seq++;
    };
    while (g_engine.running) {
        if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
        while (hdr[next].dwFlags & WHDR_DONE) {
            WAVEHDR& h = hdr[next];
            const short* s = (const short*)h.lpData;
            int got = int(h.dwBytesRecorded / 2);
            if (got == n) {
                double e = 0;
                for (int i = 0; i < n; i++) e += double(s[i]) * s[i];
                double rms = sqrt(e / n);
                // the noise floor follows quiet frames quickly and loud ones slowly
                floor_rms = rms < floor_rms ? floor_rms * 0.8 + rms * 0.2 : floor_rms * 0.998 + rms * 0.002;
                bool loud = rms > floor_rms * 3 && rms > 250;
                hang = loud ? 10 : hang > 0 ? hang - 1 : 0;  // 400 ms after the last loud frame
                if (hang > 0 && g_engine.mic) {
                    g_engine.mic_loud = GetTickCount();
                    unsigned char out[kMaxFrameBytes];
                    int len = opus_encode(enc, s, n, out, kMaxFrameBytes);
                    if (len > 0) pending.emplace_back(out, out + len);
                    if (int(pending.size()) >= kFramesPerPacket) flush();
                } else {
                    flush();
                }
            }
            h.dwFlags &= ~WHDR_DONE;
            g_wave.inAdd(in, &h, sizeof h);
            next = (next + 1) % kBlocks;
        }
    }
    g_wave.inReset(in);
    for (auto& h : hdr) g_wave.inUnprepare(in, &h, sizeof h);
    g_wave.inClose(in);
    opus_encoder_destroy(enc);
    CloseHandle(ev);
}

// One 40 ms block of everyone heard, through the 360 chain.
void mix_block(std::vector<float>& acc, int n) {
    std::fill(acc.begin(), acc.end(), 0.0f);
    std::vector<opus_int16> pcm((size_t)n);
    std::lock_guard<std::mutex> l(g_engine.mu);
    DWORD now = GetTickCount();
    for (auto it = g_engine.talkers.begin(); it != g_engine.talkers.end();) {
        Talker& t = it->second;
        if (now - t.heard > 10000 && t.frames.empty()) {
            logf("voice: console %#llx silent for 10 s: its decoder is let go (%llu packets heard, %llu lost)", (unsigned long long)it->first,
                 (unsigned long long)t.packets, (unsigned long long)t.lost);
            if (t.dec) opus_decoder_destroy(t.dec);
            it = g_engine.talkers.erase(it);
            continue;
        }
        if (!t.dec) {
            int err = 0;
            t.dec = opus_decoder_create(g_engine.out_rate, 1, &err);
            t.hp.set(true, 300, g_engine.out_rate);
            t.lp.set(false, 3400, g_engine.out_rate);
            if (!t.dec) {
                t.frames.clear();
                ++it;
                continue;
            }
        }
        if (!t.playing && int(t.frames.size()) >= kJitterStart) t.playing = true, t.concealed = 0;
        if (t.playing) {
            int got = 0;
            if (!t.frames.empty()) {
                std::vector<unsigned char> f = std::move(t.frames.front());
                t.frames.pop_front();
                got = f.empty() ? opus_decode(t.dec, nullptr, 0, pcm.data(), n, 0) : opus_decode(t.dec, f.data(), int(f.size()), pcm.data(), n, 0);
                t.concealed = 0;
            } else if (t.concealed < kConcealFrames) {
                got = opus_decode(t.dec, nullptr, 0, pcm.data(), n, 0);
                t.concealed++;
            } else {
                t.playing = false;
            }
            for (int i = 0; i < got && i < n; i++) {
                double x = t.lp.run(t.hp.run(pcm[size_t(i)] / 32768.0)) * 1.6;
                acc[size_t(i)] += float(mulaw_round_trip(tanh(1.5 * x) / tanh(1.5)));
            }
        }
        ++it;
    }
}

void playback_thread() {
    HWAVEOUT out = nullptr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int rate = 0;
    for (int r : {8000, 48000}) {
        WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, DWORD(r), DWORD(r * 2), 2, 16, 0};
        if (g_wave.outOpen(&out, WAVE_MAPPER, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
            rate = r;
            break;
        }
        out = nullptr;
    }
    if (!out) {
        logf("voice: no playback device could be opened (8 or 48 kHz mono): nobody is heard");
        CloseHandle(ev);
        return;
    }
    {
        std::lock_guard<std::mutex> l(g_engine.mu);
        g_engine.out_rate = rate;
    }
    logf("voice: playback open at %d Hz through the 360 headset chain (band-pass 300-3400 Hz, soft clip, 8-bit mu-law)", rate);
    const int n = rate * kFrameMs / 1000;
    const int kBlocks = 3;  // 120 ms queued
    std::vector<short> mem(size_t(n) * kBlocks, 0);
    std::vector<float> acc((size_t)n);
    WAVEHDR hdr[kBlocks] = {};
    for (int i = 0; i < kBlocks; i++) {
        hdr[i].lpData = (LPSTR)&mem[size_t(i) * n], hdr[i].dwBufferLength = DWORD(n * 2);
        g_wave.outPrepare(out, &hdr[i], sizeof hdr[i]);
        g_wave.outWrite(out, &hdr[i], sizeof hdr[i]);
    }
    int next = 0;
    while (g_engine.running) {
        if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
        while (hdr[next].dwFlags & WHDR_DONE) {
            mix_block(acc, n);
            short* s = (short*)hdr[next].lpData;
            for (int i = 0; i < n; i++) {
                float v = acc[size_t(i)];
                s[i] = short(v > 1 ? 32767 : v < -1 ? -32767 : v * 32767);
            }
            hdr[next].dwFlags &= ~WHDR_DONE;
            g_wave.outWrite(out, &hdr[next], sizeof hdr[next]);
            next = (next + 1) % kBlocks;
        }
    }
    g_wave.outReset(out);
    for (auto& h : hdr) g_wave.outUnprepare(out, &h, sizeof h);
    g_wave.outClose(out);
    CloseHandle(ev);
}

void start_engine() {
    if (g_engine.running.exchange(true)) return;
    if (!load_wave()) {
        logf("voice: the system winmm.dll has no wave functions: voice is off");
        g_engine.running = false;
        return;
    }
    const char* mic = getenv("FH2MP_VOICE_MIC");
    g_engine.mic = !(mic && mic[0] == '0');
    std::thread(playback_thread).detach();
    if (g_engine.mic)
        std::thread(capture_thread).detach();
    else
        logf("voice: FH2MP_VOICE_MIC=0: the microphone stays closed, this player only listens");
}

// A relayed packet from the game's voice loop (the game's thread): its frames go to that console's queue.
void take_packet(uint64_t from, const BYTE* p, UINT32 n) {
    if (n < 5 || p[0] != 'V' || p[1] != 1) return;
    int seq = p[2] | (p[3] << 8), count = p[4];
    std::vector<std::vector<unsigned char>> frames;
    UINT32 at = 5;
    for (int i = 0; i < count; i++) {
        if (at >= n || at + 1 + p[at] > n || p[at] == 0) return;
        frames.emplace_back(p + at + 1, p + at + 1 + p[at]);
        at += 1 + p[at];
    }
    std::lock_guard<std::mutex> l(g_engine.mu);
    auto found = g_engine.talkers.find(from);
    if (found == g_engine.talkers.end()) {
        found = g_engine.talkers.emplace(from, Talker()).first;
        logf("voice: hearing console %#llx (its first packet: %u bytes, %d frames)", (unsigned long long)from, n, count);
    }
    Talker& t = found->second;
    t.heard = GetTickCount(), t.packets++;
    if (t.expect >= 0) {
        int gap = (seq - t.expect) & 0xffff;
        if (gap >= 0x8000) return;  // late or a copy
        if (gap > 0 && gap < 4) {
            t.lost += gap;
            for (int i = 0; i < gap * count && i < kConcealFrames; i++) t.frames.emplace_back();
        }
    }
    t.expect = (seq + 1) & 0xffff;
    for (auto& f : frames) t.frames.push_back(std::move(f));
    while (int(t.frames.size()) > kJitterMax) t.frames.pop_front();
}

// ---- IChatManager ---------------------------------------------------------------------------------------------------------------------

struct IChatManagerAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_ChatSettings(IInspectable** out) = 0;                          // +0x30
    virtual HRESULT STDMETHODCALLTYPE add_OnDebugMessage(IUnknown* h, int64_t* token) = 0;               // +0x38
    virtual HRESULT STDMETHODCALLTYPE remove_OnDebugMessage(int64_t token) = 0;                          // +0x40
    virtual HRESULT STDMETHODCALLTYPE add_OnOutgoingChatPacketReady(IUnknown* h, int64_t* token) = 0;    // +0x48
    virtual HRESULT STDMETHODCALLTYPE remove_OnOutgoingChatPacketReady(int64_t token) = 0;               // +0x50
    virtual HRESULT STDMETHODCALLTYPE add_OnCompareUniqueConsoleIdentifiers(IUnknown* h, int64_t* token) = 0;  // +0x58
    virtual HRESULT STDMETHODCALLTYPE remove_OnCompareUniqueConsoleIdentifiers(int64_t token) = 0;       // +0x60
    virtual HRESULT STDMETHODCALLTYPE add_Event4(IUnknown* h, int64_t* token) = 0;                       // +0x68
    virtual HRESULT STDMETHODCALLTYPE remove_Event4(int64_t token) = 0;                                  // +0x70
    virtual HRESULT STDMETHODCALLTYPE add_Event5(IUnknown* h, int64_t* token) = 0;                       // +0x78
    virtual HRESULT STDMETHODCALLTYPE remove_Event5(int64_t token) = 0;                                  // +0x80
    virtual HRESULT STDMETHODCALLTYPE ProcessIncomingChatMessage(IUnknown* buffer, IInspectable* id) = 0;  // +0x88
    virtual HRESULT STDMETHODCALLTYPE HandleNewRemoteConsole(IInspectable* id) = 0;                      // +0x90
    virtual HRESULT STDMETHODCALLTYPE AddLocalUserToChatChannelAsync(UINT8 ch, IInspectable* user, IUnknown** op) = 0;  // +0x98
    virtual HRESULT STDMETHODCALLTYPE RemoveLocalUserFromChatChannelAsync(UINT8 ch, IInspectable* user, IUnknown** op) = 0;  // +0xa0
    virtual HRESULT STDMETHODCALLTYPE ChannelCall3Async(UINT8 ch, IInspectable* user, IUnknown** op) = 0;  // +0xa8
    virtual HRESULT STDMETHODCALLTYPE RemoveRemoteConsoleAsync(IInspectable* id, IUnknown** op) = 0;     // +0xb0
    virtual HRESULT STDMETHODCALLTYPE GetChatUsers(IInspectable** out) = 0;                              // +0xb8
    virtual HRESULT STDMETHODCALLTYPE MuteUserFromAllChannels(IInspectable* user) = 0;                   // +0xc0
    virtual HRESULT STDMETHODCALLTYPE UnmuteUserFromAllChannels(IInspectable* user) = 0;                 // +0xc8
    virtual HRESULT STDMETHODCALLTYPE MuteAll() = 0;                                                     // +0xd0
    virtual HRESULT STDMETHODCALLTYPE UnmuteAll() = 0;                                                   // +0xd8
    virtual HRESULT STDMETHODCALLTYPE get_Flag(boolean* out) = 0;                                        // +0xe0
    virtual HRESULT STDMETHODCALLTYPE get_Object(IInspectable** out) = 0;                                // +0xe8
};

class ChatManager final : public IChatManagerAbi {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_Inspectable) || IsEqualGUID(iid, IID_Agile) || IsEqualGUID(iid, IID_ChatManager)) {
            *out = static_cast<IChatManagerAbi*>(this);
            AddRef();
            return S_OK;
        }
        return *out = nullptr, E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override { return ULONG(--refs_); }  // one for the process
    INSPECTABLE_BODY
    HRESULT STDMETHODCALLTYPE get_ChatSettings(IInspectable** out) override {
        if (!out) return E_POINTER;
        ++g_settings.refs;
        *out = (IInspectable*)&g_settings;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE add_OnDebugMessage(IUnknown*, int64_t* token) override { return give_token(token); }
    HRESULT STDMETHODCALLTYPE remove_OnDebugMessage(int64_t) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE add_OnOutgoingChatPacketReady(IUnknown* h, int64_t* token) override {
        if (!h || !token) return E_POINTER;
        h->AddRef();
        IUnknown* old = nullptr;
        {
            std::lock_guard<std::mutex> l(g_engine.mu);
            old = g_engine.outgoing;
            g_engine.outgoing = h;
            *token = g_engine.outgoing_token = g_engine.next_token++;
        }
        if (old) old->Release();
        logf("voice: the game's chat layer listens for our packets: the engine starts");
        start_engine();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE remove_OnOutgoingChatPacketReady(int64_t token) override {
        IUnknown* old = nullptr;
        {
            std::lock_guard<std::mutex> l(g_engine.mu);
            if (token != g_engine.outgoing_token) return S_OK;
            old = g_engine.outgoing;
            g_engine.outgoing = nullptr;
        }
        if (old) old->Release();
        logf("voice: the game's chat layer stopped listening for our packets: nothing more is sent");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE add_OnCompareUniqueConsoleIdentifiers(IUnknown*, int64_t* token) override { return give_token(token); }
    HRESULT STDMETHODCALLTYPE remove_OnCompareUniqueConsoleIdentifiers(int64_t) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE add_Event4(IUnknown*, int64_t* token) override { return give_token(token); }
    HRESULT STDMETHODCALLTYPE remove_Event4(int64_t) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE add_Event5(IUnknown*, int64_t* token) override { return give_token(token); }
    HRESULT STDMETHODCALLTYPE remove_Event5(int64_t) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ProcessIncomingChatMessage(IUnknown* buffer, IInspectable* id) override {
        uint64_t from = 0;
        if (id) {
            IInspectable* ref = nullptr;
            if (SUCCEEDED(id->QueryInterface(IID_RefUInt64, (void**)&ref)) && ref) {
                typedef HRESULT(STDMETHODCALLTYPE * ValueFn)(IInspectable*, UINT64*);
                ((ValueFn)(*(void***)ref)[6])(ref, &from);  // IReference<UINT64>::get_Value, +0x30
                ref->Release();
            }
        }
        const BYTE* p = nullptr;
        UINT32 n = 0;
        if (g_engine.running && buffer_bytes(buffer, &p, &n)) take_packet(from, p, n);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HandleNewRemoteConsole(IInspectable*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE AddLocalUserToChatChannelAsync(UINT8, IInspectable*, IUnknown** op) override { return done(op); }
    HRESULT STDMETHODCALLTYPE RemoveLocalUserFromChatChannelAsync(UINT8, IInspectable*, IUnknown** op) override { return done(op); }
    HRESULT STDMETHODCALLTYPE ChannelCall3Async(UINT8, IInspectable*, IUnknown** op) override { return done(op); }
    HRESULT STDMETHODCALLTYPE RemoveRemoteConsoleAsync(IInspectable*, IUnknown** op) override { return done(op); }
    HRESULT STDMETHODCALLTYPE GetChatUsers(IInspectable** out) override { return out ? (*out = nullptr, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE MuteUserFromAllChannels(IInspectable*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UnmuteUserFromAllChannels(IInspectable*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE MuteAll() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UnmuteAll() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Flag(boolean* out) override { return out ? (*out = 0, S_OK) : E_POINTER; }
    HRESULT STDMETHODCALLTYPE get_Object(IInspectable** out) override { return out ? (*out = nullptr, S_OK) : E_POINTER; }

private:
    HRESULT give_token(int64_t* token) {
        if (!token) return E_POINTER;
        std::lock_guard<std::mutex> l(g_engine.mu);
        *token = g_engine.next_token++;
        return S_OK;
    }
    HRESULT done(IUnknown** op) {
        if (!op) return E_POINTER;
        *op = static_cast<IActionAbi*>(new DoneAction());
        return S_OK;
    }
    std::atomic<long> refs_{1};
};

ChatManager g_manager;
IInspectable* chat_manager_self() { return static_cast<IChatManagerAbi*>(&g_manager); }

// game+0x363a70: activates Microsoft.Xbox.GameChat.ChatManager and gives back its IChatManager (one count, the caller's).
void* (*o_activate)();
void* hk_activate() {
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) logf("voice: the game activates its ChatManager: ours answers (Opus narrowband, relayed by the FH2MP server)");
    g_manager.AddRef();
    return static_cast<IChatManagerAbi*>(&g_manager);
}

// game+0x36d830: a buffer with a copy of the bytes (the game's voice loop, a relayed payload); ours instead of the system's Buffer class.
void* (*o_make_buffer)(const void*, unsigned long long);
void* hk_make_buffer(const void* data, unsigned long long n) {
    if (n > 0x10000) n = 0x10000;  // its callers (game+0x379390, game+0x298190) copy at most 0x55c and 64 bytes
    return static_cast<IBufferAbi*>(new Buf(data, size_t(n)));
}

}  // namespace

bool voice_install(const FH2ModApi* api, FH2Mod* mod, unsigned char* base, void (*log)(const char*)) {
    g_log = log;
    const char* off = getenv("FH2MP_NO_VOICE");
    if (off && off[0] && off[0] != '0') {
        logf("voice: FH2MP_NO_VOICE is set: the game's ChatManager is left to Azimuth (no voice)");
        return false;
    }
    static const unsigned char activate_head[] = {0x40, 0x55, 0x53, 0x57, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x50, 0x48, 0xc7, 0x45, 0xd8, 0xfe};
    static const unsigned char buffer_head[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff};
    if (memcmp(base + 0x363a70, activate_head, sizeof activate_head) || memcmp(base + 0x36d830, buffer_head, sizeof buffer_head)) {
        logf("voice: the game's ChatManager activation (game+0x363a70) or chat buffer (game+0x36d830) is not the code this Guide knows: no voice");
        return false;
    }
    init_settings();
    FH2Hook* a = api->hook_function(mod, "guide: the game's ChatManager activation (game+0x363a70)", base + 0x363a70, (void*)hk_activate, (void**)&o_activate);
    FH2Hook* b = api->hook_function(mod, "guide: the game's chat buffer (game+0x36d830)", base + 0x36d830, (void*)hk_make_buffer, (void**)&o_make_buffer);
    if (!a || !b || !api->enable_hooks(mod, a) || !api->enable_hooks(mod, b)) {
        logf("voice: the game's ChatManager activation or chat buffer could not be hooked: no voice");
        return false;
    }
    logf("voice: the game's ChatManager is ours (game+0x363a70, game+0x36d830 hooked); the engine starts when a multiplayer session does");
    return true;
}

int voice_talking(uint64_t* ids, int max) {
    int k = 0;
    DWORD now = GetTickCount();
    if (k < max && g_engine.mic && now - g_engine.mic_loud < 500) ids[k++] = 0;
    std::lock_guard<std::mutex> l(g_engine.mu);
    for (auto& kv : g_engine.talkers)
        if (k < max && now - kv.second.heard < 500) ids[k++] = kv.first;
    return k;
}
