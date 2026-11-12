// Loopback test of the voice engine (guide/src/voice.cpp) without a game: the ABI the game uses (vtable offsets read from the game's
// code, see voice.cpp's header), our packet format, the jitter queue and the 360 chain.
//   build.sh voicetest  ->  guide/build/voice-test.exe
#include "../src/voice.cpp"

#include <cstdio>

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), g_fail++; \
    } while (0)

typedef HRESULT(STDMETHODCALLTYPE* Slot1)(void*, void*);
typedef HRESULT(STDMETHODCALLTYPE* Slot2)(void*, void*, void*);
static void* slot(void* obj, int off) { return (*(void***)obj)[off / 8]; }

// What the game's handler (game+0x374010) reads from each packet, through raw offsets.
static std::vector<std::vector<unsigned char>> g_sent;
static int g_reliable = -1, g_to_all = -1;
struct FakeDelegate {
    void** vt;
};
static HRESULT STDMETHODCALLTYPE fd_qi(void*, REFIID, void** out) { return *out = nullptr, E_NOINTERFACE; }
static ULONG STDMETHODCALLTYPE fd_ref(void*) { return 1; }
static HRESULT STDMETHODCALLTYPE fd_invoke(void*, void* sender, void* args) {
    if (!sender || !args) return E_POINTER;
    void* buf = nullptr;
    ((Slot1)slot(args, 0x30))(args, &buf);  // get_PacketBuffer
    UINT32 len = 0;
    ((Slot1)slot(buf, 0x38))(buf, &len);  // IBuffer::get_Length
    void* access = nullptr;
    ((Slot2)slot(buf, 0))(buf, (void*)&IID_BufferByteAccess, &access);
    BYTE* bytes = nullptr;
    ((Slot1)slot(access, 0x18))(access, &bytes);  // IBufferByteAccess::Buffer
    g_sent.emplace_back(bytes, bytes + len);
    boolean v = 9;
    ((Slot1)slot(args, 0x38))(args, &v);
    g_reliable = v;
    ((Slot1)slot(args, 0x48))(args, &v);
    g_to_all = v;
    ((IUnknown*)access)->Release();
    ((IUnknown*)buf)->Release();
    return S_OK;
}
static void* g_fd_vt[4] = {(void*)fd_qi, (void*)fd_ref, (void*)fd_ref, (void*)fd_invoke};
static FakeDelegate g_fd = {g_fd_vt};

// A boxed u64 as the game makes one (Platform::Details::CreateValue): IReference<UINT64>, get_Value at +0x30.
struct Box {
    void** vt;
    UINT64 v;
};
static HRESULT STDMETHODCALLTYPE box_qi(Box* b, REFIID iid, void** out) {
    if (IsEqualGUID(iid, IID_RefUInt64)) return *out = b, S_OK;
    return *out = nullptr, E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE box_ref(Box*) { return 1; }
static HRESULT STDMETHODCALLTYPE box_value(Box* b, UINT64* v) { return *v = b->v, S_OK; }
static void* g_box_vt[7] = {(void*)box_qi, (void*)box_ref, (void*)box_ref, nullptr, nullptr, nullptr, (void*)box_value};

// A Windows.Xbox.System.IUser as Azimuth hands one to AddLocalUserToChatChannelAsync: get_XboxUserId at +0x78 (slot 15).
static HRESULT STDMETHODCALLTYPE fu_xuid(void*, HSTRING* out) { return g_create(L"2533277673447315", 16, out); }
static HRESULT STDMETHODCALLTYPE fu_ok() { return S_OK; }
static void* g_fu_vt[40];
static struct {
    void** vt;
} g_fake_user = {g_fu_vt};

int main() {
    init_settings();
    HMODULE combase = LoadLibraryW(L"combase.dll");
    g_raw = (RawBufferFn)(void*)GetProcAddress(combase, "WindowsGetStringRawBuffer");
    g_create = (CreateStringFn)(void*)GetProcAddress(combase, "WindowsCreateString");
    g_delete = (DeleteStringFn)(void*)GetProcAddress(combase, "WindowsDeleteString");
    for (auto& f : g_fu_vt) f = (void*)fu_ok;
    g_fu_vt[15] = (void*)fu_xuid;
    IChatManagerAbi* cm = (IChatManagerAbi*)hk_activate();
    // the slots the game calls
    CHECK(slot(cm, 0x88) == (*(void***)cm)[17]);
    void* settings = nullptr;
    ((Slot1)slot(cm, 0x30))(cm, &settings);
    CHECK(settings != nullptr);
    typedef HRESULT(STDMETHODCALLTYPE * SetFn)(void*, int);
    for (int off : {0x38, 0x48, 0x68, 0x88, 0xb8, 0xc8}) CHECK(((SetFn)slot(settings, off))(settings, 1) == S_OK);
    void* users = nullptr;
    ((Slot1)slot(cm, 0xb8))(cm, &users);
    UINT32 nusers = 9;
    if (users) ((Slot1)slot(users, 0x38))(users, &nusers);
    CHECK(users && nusers == 0);  // nobody yet: an empty list, as the real engine gives
    if (users) ((IUnknown*)users)->Release();
    void* op = nullptr;
    typedef HRESULT(STDMETHODCALLTYPE * ChanFn)(void*, UINT8, void*, void**);
    CHECK(((ChanFn)slot(cm, 0x98))(cm, 0, nullptr, &op) == S_OK && op);
    int status = -1;
    void* info = nullptr;
    ((Slot2)slot(op, 0))(op, (void*)&IID_AsyncInfo, &info);
    ((Slot1)slot(info, 0x38))(info, &status);  // IAsyncInfo::get_Status
    CHECK(status == 1);
    ((IUnknown*)info)->Release();
    ((IUnknown*)op)->Release();

    // a 440 Hz tone, encoded like the capture thread and handed to the game's handler
    int64_t token = 0;
    g_engine.outgoing = (IUnknown*)&g_fd;
    g_engine.running = true;
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(8000, 1, OPUS_APPLICATION_VOIP, &err);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND));
    const int n = 320;
    short pcm[n];
    uint16_t seq = 0;
    size_t total = 0;
    for (int pkt = 0; pkt < 10; pkt++) {
        std::vector<unsigned char> p = {'V', 1, (unsigned char)(seq & 0xff), (unsigned char)(seq >> 8), 2};
        for (int f = 0; f < 2; f++) {
            for (int i = 0; i < n; i++) pcm[i] = short(8000 * sin(2 * 3.14159265 * 440 * ((pkt * 2 + f) * n + i) / 8000.0));
            unsigned char out[kMaxFrameBytes];
            int len = opus_encode(enc, pcm, n, out, kMaxFrameBytes);
            CHECK(len > 0);
            p.push_back((unsigned char)len);
            p.insert(p.end(), out, out + len);
        }
        while (p.size() < size_t(kMinPayload)) p.push_back(0);
        total += p.size();
        raise_packet(p);
        seq++;
    }
    CHECK(g_sent.size() == 10);
    CHECK(g_reliable == 0 && g_to_all == 1);
    printf("10 packets of 80 ms: %zu bytes in all, %.1f kbit/s of payload\n", total, total * 8 / 0.8 / 1000);
    (void)token;

    // back in through ProcessIncomingChatMessage, as the game's voice loop does (its buffer from game+0x36d830, the console id boxed)
    Box id = {g_box_vt, 0x0009000012345678ull};
    for (auto& s : g_sent) {
        void* b = hk_make_buffer(s.data(), s.size());
        CHECK(((Slot2)slot(cm, 0x88))(cm, b, &id) == S_OK);
        ((IUnknown*)b)->Release();
    }
    CHECK(g_engine.talkers.count(id.v) == 1);
    CHECK(g_engine.talkers[id.v].frames.size() == 8);  // 20 frames, the queue keeps the newest kJitterMax
    std::vector<float> acc((size_t)n);
    double energy = 0;
    for (int b = 0; b < 8; b++) {
        mix_block(acc, n);
        for (float v : acc) energy += double(v) * v;
    }
    CHECK(energy > 1.0);
    printf("8 blocks played: energy %.1f\n", energy);
    uint64_t talking[4];
    CHECK(voice_talking(talking, 4) == 1 && talking[0] == id.v);
    // a short, foreign or broken payload is dropped
    unsigned char junk[20] = {'V', 1, 0, 0, 5, 200};
    take_packet(7, junk, sizeof junk);
    CHECK(g_engine.talkers.count(7) == 0);
    opus_encoder_destroy(enc);
    // the local player: its ChatUser is there when AddLocalUserToChatChannelAsync returns, found by xuid as game+0x370bc0 does
    {
        void* op2 = nullptr;
        CHECK(((ChanFn)slot(cm, 0x98))(cm, 0, &g_fake_user, &op2) == S_OK && op2);
        ((IUnknown*)op2)->Release();
        void* users2 = nullptr;
        ((Slot1)slot(cm, 0xb8))(cm, &users2);
        UINT32 count = 0;
        ((Slot1)slot(users2, 0x38))(users2, &count);
        CHECK(count == 2);  // the local player and the console heard above
        void* local = nullptr;
        typedef HRESULT(STDMETHODCALLTYPE * GetAtFn)(void*, UINT32, void**);
        for (UINT32 i = 0; i < count; i++) {
            void* u = nullptr;
            ((GetAtFn)slot(users2, 0x30))(users2, i, &u);
            HSTRING h = nullptr;
            ((Slot1)slot(u, 0x30))(u, &h);
            if (wcscmp(g_raw(h, nullptr), L"2533277673447315") == 0)
                local = u;
            else
                ((IUnknown*)u)->Release();
            g_delete(h);
        }
        CHECK(local != nullptr);
        if (local) {
            INT32 mode = -1;
            g_engine.mic_loud = GetTickCount() - 1000;
            ((Slot1)slot(local, 0x40))(local, &mode);
            CHECK(mode == 0);
            g_engine.mic_loud = GetTickCount();
            ((Slot1)slot(local, 0x40))(local, &mode);
            CHECK(mode == 1);
            // get_User -> AudioDevices (+0x38): our capture device when there is a microphone; other slots reach the real user
            void* user = nullptr;
            ((Slot1)slot(local, 0x60))(local, &user);
            CHECK(user != nullptr);
            void* devices = nullptr;
            ((Slot1)slot(user, 0x38))(user, &devices);
            UINT32 nd = 9;
            ((Slot1)slot(devices, 0x38))(devices, &nd);
            CHECK(nd == (have_microphone() ? 1u : 0u));
            if (nd == 1) {
                void* d = nullptr;
                ((GetAtFn)slot(devices, 0x30))(devices, 0, &d);
                INT32 type = -1;
                ((Slot1)slot(d, 0x38))(d, &type);
                HSTRING hid = nullptr;
                ((Slot1)slot(d, 0x30))(d, &hid);
                CHECK(type == 0 && wcsstr(g_raw(hid, nullptr), L"xboxcontrollercapture"));
                g_delete(hid);
                ((IUnknown*)d)->Release();
            }
            HSTRING hx = nullptr;
            ((Slot1)slot(user, 0x78))(user, &hx);  // forwarded to the real user
            CHECK(hx && wcscmp(g_raw(hx, nullptr), L"2533277673447315") == 0);
            g_delete(hx);
            ((IUnknown*)devices)->Release();
            ((IUnknown*)user)->Release();
            ((IUnknown*)local)->Release();
        }
        ((IUnknown*)users2)->Release();
        printf("local ChatUser: talking mode, %s capture device, forwarded user calls\n", have_microphone() ? "a" : "no");
    }
    // the game's Mute with the remote player's ChatUser: their packets are not played any more; Unmute brings them back
    {
        ChatUser* remote = chat_user(id.v, nullptr);
        CHECK(((Slot1)slot(cm, 0xc0))(cm, static_cast<IChatUserAbi*>(remote)) == S_OK);
        CHECK(g_engine.talkers.count(id.v) == 0);
        void* b = hk_make_buffer(g_sent[0].data(), g_sent[0].size());
        ((Slot2)slot(cm, 0x88))(cm, b, &id);
        ((IUnknown*)b)->Release();
        CHECK(g_engine.talkers.count(id.v) == 0);
        CHECK(((Slot1)slot(cm, 0xc8))(cm, static_cast<IChatUserAbi*>(remote)) == S_OK);
        b = hk_make_buffer(g_sent[1].data(), g_sent[1].size());
        ((Slot2)slot(cm, 0x88))(cm, b, &id);
        ((IUnknown*)b)->Release();
        CHECK(g_engine.talkers.count(id.v) == 1);
        printf("mute and unmute through the game's ChatManager calls\n");
    }
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
