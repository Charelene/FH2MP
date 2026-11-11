// The Xbox system UI calls of FH2 (build 1.0.0.64) and what answers each (audit 2026-11-11; Notion "FH2's Xbox system UI calls").
//
// The game reaches Windows.Xbox.UI.SystemUI through seven small wrappers, one per method (ISystemUIStatics slot in brackets):
//   game+0xe41d30  ShowProfileCardAsync(user, xuid) [+0x40]   from the PopupShowPlayerCard scene        -> the Guide's Profile  (hooked)
//   game+0xc5f200  ShowProfileCardAsync(user, xuid) [+0x40]   from the URI handler game+0xc579a0        -> the Guide's Profile  (hooked)
//   game+0xed23a0  ShowVirtualKeyboardAsync(default, title, description, scope) [+0x58]
//                                                             from the VirtualKeyboardPopup scene       -> the Guide's keyboard (hooked)
//   game+0x17b9840 ShowSendInvitesAsync(user) [+0x48]         from the party manager's invite task; under Azimuth that task ends before
//                  it gets here, so the entry is hooked instead: game+0x17b9700 (called by the Game Party popup, game+0xdaefe0, and by
//                  the state-flow node NetworkShowSystemInviteToGameParty, game+0x8dd9c0)              -> the Guide's game invite (hooked)
//   game+0xe36a80  LaunchAchievementsAsync(user, titleId) [+0x68] from PopupShowAchievements: left to Azimuth (the server keeps no
//                  achievements to show)
//   game+0xd08bd0  ShowAccountPickerAsync(controller, options) [+0x50] from Splash, ControllerPairing: left to Azimuth (one account)
//   game+0xb64690  ShowPeoplePickerAsync(...) [ISystemUIStatics5 +0x30]: left to Azimuth
//
// The scenes keep the returned async object and poll it every frame: QueryInterface(IAsyncInfo), get_Status; when it is no longer
// Started they close, and the keyboard scene first takes GetResults (slot +0x40 of what the wrapper returned) when the status is
// Completed, else keeps the default text. So each hooked wrapper returns an object of ours that stays Started while the Guide shows the
// screen and ends when the Guide closes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hstring.h>
#include <inspectable.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "fh2mod.h"
#include "sysui.h"

namespace {

void (*g_log)(const char*);
void (*g_on_invite)();

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) g_log(buf);
}

typedef PCWSTR(WINAPI* RawBufferFn)(HSTRING, UINT32*);
typedef HRESULT(WINAPI* CreateStringFn)(PCWSTR, UINT32, HSTRING*);
RawBufferFn g_raw;
CreateStringFn g_create;

std::string utf8_of(HSTRING h) {
    if (!h || !g_raw) return "";
    UINT32 n = 0;
    PCWSTR w = g_raw(h, &n);
    if (!w || !n) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w, int(n), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, int(n), &s[0], len, nullptr, nullptr);
    return s;
}

// WinRT's HRESULTs for a call made at the wrong time (not in every mingw winerror.h).
const HRESULT ILLEGAL_METHOD_CALL = HRESULT(0x8000000EL), ILLEGAL_DELEGATE_ASSIGNMENT = HRESULT(0x80000018L);

const GUID IID_Inspectable = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
const GUID IID_AsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID IID_AsyncAction = {0x5a648006, 0x843a, 0x4da9, {0x86, 0x5b, 0x9d, 0x26, 0xe5, 0xdf, 0xad, 0x7b}};
const GUID IID_AsyncOperationString = {0x3e1fe603, 0xf897, 0x5263, {0xb3, 0x28, 0x08, 0x06, 0x42, 0x6b, 0x8a, 0x79}};
const GUID IID_Agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

// IAsyncAction and IAsyncOperation<HSTRING> have the same shape (GetResults differs in what it gives).
struct IAsyncWork : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(HSTRING* text) = 0;
};
struct IAsyncState : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// What a hooked wrapper returns: Started (0) until the Guide's screen is gone, then Completed (1) or Canceled (2).
class SysAsync final : public IAsyncWork, public IAsyncState {
public:
    explicit SysAsync(bool gives_text) : gives_text_(gives_text) {}

    // IUnknown (both bases share the count)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_Inspectable) || IsEqualGUID(iid, IID_Agile) ||
            IsEqualGUID(iid, gives_text_ ? IID_AsyncOperationString : IID_AsyncAction)) {
            *out = static_cast<IAsyncWork*>(this);
        } else if (IsEqualGUID(iid, IID_AsyncInfo)) {
            *out = static_cast<IAsyncState*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        long n = --refs_;
        if (n == 0) delete this;
        return ULONG(n);
    }
    // IInspectable
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override {
        if (count) *count = 0;
        if (iids) *iids = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override {
        if (name) *name = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override {
        if (level) *level = BaseTrust;
        return S_OK;
    }
    // IAsyncAction / IAsyncOperation<HSTRING>
    HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) override {
        bool now = false;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (handler_) return ILLEGAL_DELEGATE_ASSIGNMENT;
            handler_ = handler;
            if (handler_) handler_->AddRef();
            now = status_ != 0;
        }
        if (now) fire();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) override {
        std::lock_guard<std::mutex> l(mu_);
        if (!handler) return E_POINTER;
        *handler = handler_;
        if (handler_) handler_->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetResults(HSTRING* text) override {
        // an action's GetResults has no argument: text is only looked at for the operation that gives a string
        if (!gives_text_) return status_ == 1 ? S_OK : ILLEGAL_METHOD_CALL;
        if (!text) return E_POINTER;
        *text = nullptr;
        std::lock_guard<std::mutex> l(mu_);
        if (status_ != 1) return ILLEGAL_METHOD_CALL;
        if (!result_.empty() && g_create) return g_create(result_.c_str(), UINT32(result_.size()), text);
        return S_OK;
    }
    // IAsyncInfo
    HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) override {
        if (id) *id = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Status(int* status) override {
        if (!status) return E_POINTER;
        *status = status_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) override {
        if (code) *code = S_OK;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Cancel() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

    void complete(bool done, const std::string& text) {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (status_ != 0) return;
            int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
            result_.assign(size_t(n), L'\0');
            if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), &result_[0], n);
            status_ = done ? 1 : 2;
        }
        fire();
    }

private:
    ~SysAsync() {
        if (handler_) handler_->Release();
    }
    // The completed handler, when one was given (the scenes poll instead): Invoke(asyncInfo, status) is the delegate's slot 3.
    void fire() {
        IUnknown* h = nullptr;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (fired_ || !handler_) return;
            fired_ = true, h = handler_;
            h->AddRef();
        }
        typedef HRESULT(STDMETHODCALLTYPE * InvokeFn)(IUnknown*, IAsyncWork*, int);
        ((InvokeFn)(*(void***)h)[3])(h, static_cast<IAsyncWork*>(this), status_);
        h->Release();
    }

    std::atomic<long> refs_{1};
    const bool gives_text_;
    std::mutex mu_;
    std::atomic<int> status_{0};
    std::wstring result_;
    IUnknown* handler_ = nullptr;
    bool fired_ = false;
};

std::mutex g_mu;
std::deque<SysAsk> g_asks;

// Queues what the game asked for; the returned object is the game's (one count), the queue holds another until sysui_complete.
IAsyncWork* ask(SysAsk a, bool gives_text) {
    SysAsync* op = new SysAsync(gives_text);
    op->AddRef();
    a.op = static_cast<IAsyncWork*>(op);
    std::lock_guard<std::mutex> l(g_mu);
    g_asks.push_back(std::move(a));
    return static_cast<IAsyncWork*>(op);
}

void* o_card_popup;
void* o_card_uri;
void* o_keyboard;
typedef void (*InviteUiFn)(void* party_manager, void* callback);
InviteUiFn o_invite_ui;

// ShowProfileCardAsync(user, xuid): the xuid comes as decimal text.
void* hk_profile_card(void* user, HSTRING xuid, void*, void*) {
    (void)user;
    SysAsk a;
    a.kind = SysAsk::ProfileCard;
    a.xuid = strtoull(utf8_of(xuid).c_str(), nullptr, 10);
    logf("guide: the game asked for a player's gamercard (xuid %llx): the Guide shows their profile", (unsigned long long)a.xuid);
    return ask(a, false);
}

// ShowVirtualKeyboardAsync(defaultText, titleText, descriptionText, inputScope).
void* hk_virtual_keyboard(HSTRING def, HSTRING title, HSTRING description, int scope) {
    SysAsk a;
    a.kind = SysAsk::Keyboard;
    a.text = utf8_of(def), a.title = utf8_of(title), a.description = utf8_of(description);
    logf("guide: the game asked for the system keyboard (title '%s', %d characters given, input scope %d): the Guide shows its keyboard", a.title.c_str(),
         int(a.text.size()), scope);
    return ask(a, true);
}

// The party manager's "show the system invite UI": the Guide opens its game invite, and the game's own call goes on as before (its
// task ends without a user under Azimuth and tells the caller so).
void hk_invite_ui(void* party_manager, void* callback) {
    logf("guide: the game asked for the invite UI: the Guide opens a game invite");
    if (g_on_invite) g_on_invite();
    o_invite_ui(party_manager, callback);
}

// A wrapper is the known one when it starts as in build 1.0.0.64 and calls the expected slot of the statics within its first 0xb0 bytes.
bool wrapper_at(const unsigned char* at, unsigned char frame, unsigned char slot) {
    static const unsigned char head[] = {0x48, 0x8b, 0xc4, 0x57, 0x48, 0x83, 0xec};
    if (memcmp(at, head, sizeof head) || at[7] != frame) return false;
    for (int i = 8; i < 0xb0; i++)
        if (at[i] == 0xff && at[i + 1] == 0x50 && at[i + 2] == slot) return true;
    return false;
}

}  // namespace

bool sysui_install(const FH2ModApi* api, FH2Mod* mod, unsigned char* base, void (*log)(const char*), void (*on_invite)()) {
    g_log = log, g_on_invite = on_invite;
    if (HMODULE combase = LoadLibraryW(L"combase.dll")) {
        g_raw = (RawBufferFn)(void*)GetProcAddress(combase, "WindowsGetStringRawBuffer");
        g_create = (CreateStringFn)(void*)GetProcAddress(combase, "WindowsCreateString");
    }
    int on = 0;
    auto hook = [&](const char* name, unsigned rva, void* detour, void** orig, bool known) {
        if (!known) {
            logf("guide: %s (game+0x%x) is not the code this Guide knows: left to Azimuth", name, rva);
            return;
        }
        FH2Hook* h = api->hook_function(mod, name, base + rva, detour, orig);
        if (!h || !api->enable_hooks(mod, h)) {
            logf("guide: %s (game+0x%x) could not be hooked: left to Azimuth", name, rva);
            return;
        }
        on++;
    };
    bool strings = g_raw && g_create;
    if (!strings) logf("guide: combase has no HSTRING functions: the game's gamercard and keyboard calls are left to Azimuth");
    hook("guide: the game's gamercard popup (ShowProfileCardAsync)", 0xe41d30, (void*)hk_profile_card, &o_card_popup, strings && wrapper_at(base + 0xe41d30, 0x50, 0x40));
    hook("guide: the game's gamercard link (ShowProfileCardAsync)", 0xc5f200, (void*)hk_profile_card, &o_card_uri, strings && wrapper_at(base + 0xc5f200, 0x50, 0x40));
    hook("guide: the game's system keyboard (ShowVirtualKeyboardAsync)", 0xed23a0, (void*)hk_virtual_keyboard, &o_keyboard, strings && wrapper_at(base + 0xed23a0, 0x70, 0x58));
    static const unsigned char invite_head[] = {0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x40, 0xe0, 0xfe};
    hook("guide: the game's invite UI (the party manager, game+0x17b9700)", 0x17b9700, (void*)hk_invite_ui, (void**)&o_invite_ui,
         memcmp(base + 0x17b9700, invite_head, sizeof invite_head) == 0);
    logf("guide: %d of 4 system UI calls of the game are answered by the Guide (gamercard popup, gamercard link, keyboard, invite UI)", on);
    return on > 0;
}

bool sysui_take(SysAsk* out) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_asks.empty()) return false;
    *out = std::move(g_asks.front());
    g_asks.pop_front();
    return true;
}

void sysui_complete(void* op, bool done, const std::string& text) {
    if (!op) return;
    SysAsync* a = static_cast<SysAsync*>(static_cast<IAsyncWork*>(op));
    a->complete(done, text);
    a->Release();
}
