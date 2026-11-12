// The Xbox system UI calls of FH2 (build 1.0.0.64) and what answers each (audit 2026-11-11; Notion "FH2's Xbox system UI calls").
//
// The game reaches Windows.Xbox.UI.SystemUI through seven small wrappers, one per method. Each asks for the statics by interface id and
// calls one slot of it:
//   ISystemUIStatics {1a0dd80e-3f9d-4cfa-8e50-0e2ebd1aa72f}
//     game+0xe41d30  +0x40 ShowProfileCardAsync(user, xuid)        the PopupShowPlayerCard scene             -> the Guide's Profile (hooked)
//     game+0x17b9840 +0x48 ShowSendInvitesAsync(user)              the party manager's invite task; under Azimuth that task ends before
//                    it gets here, so its entry is hooked instead: game+0x17b9700 (called by the Game Party popup, game+0xdaefe0, and by
//                    the state-flow node NetworkShowSystemInviteToGameParty, game+0x8dd9c0)           -> the Guide's game invite (hooked)
//     game+0xd08bd0  +0x50 ShowAccountPickerAsync(controller, options)  Splash, ControllerPairing, a pause-menu lambda: left to Azimuth
//                    (it has the one signed-in user; the launcher signs players in)
//     game+0xed23a0  +0x58 ShowVirtualKeyboardAsync(default, title, description, scope)  the VirtualKeyboardPopup scene
//                                                                                                     -> the Guide's keyboard (hooked)
//     game+0xe36a80  +0x68 LaunchAchievementsAsync(user, titleId)  the PopupShowAchievements scene: left to Azimuth (the server keeps no
//                    achievements to list)
//   ISystemUIStatics5 {1fac25f1-2fa6-4a68-88d6-89d5454a6c0f}
//     game+0xb64690  +0x30 ShowPeoplePickerAsync(user, prompt, from, chosen, min, max)  the Car Club popup's Invite (PopupClubAction,
//                    game+0xde0b50)                                                                   -> the Guide's player list (hooked)
//     game+0xc5f200  +0x40 TrySnapOther(uri, user)                 the state-flow node LaunchURI with LaunchSnapped (Forza Hub beside the
//                    game): left to Azimuth (another app, not a Guide screen). It has the same code shape as the gamercard wrapper and
//                    the same slot number, in another interface: the interface id is part of the check below.
//
// The gamercard and keyboard scenes keep the returned async object and poll it every frame: QueryInterface(IAsyncInfo), get_Status; when
// it is no longer Started they close, and the keyboard scene first takes GetResults (slot +0x40 of what the wrapper returned) when the
// status is Completed, else keeps the default text. The people picker's caller (game+0xb63f50) makes a PPL task of the operation: it sets
// a Completed handler (game+0xafc4f0: Canceled cancels the task, Error reads IAsyncInfo's error code, else GetResults) and its
// continuation (game+0xafc220) reads the result with get_Size (+0x38) and GetAt (+0x30) and takes each string as a decimal xuid; a null
// result is no players. So each hooked wrapper returns an object of ours that stays Started while the Guide shows the screen and ends
// when the Guide closes; the people picker always ends Completed (dismissed = no players), since nothing in that continuation catches a
// cancelled task.
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
std::atomic<bool> g_can_show{false};

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
typedef HRESULT(WINAPI* DeleteStringFn)(HSTRING);
RawBufferFn g_raw;
CreateStringFn g_create;
DeleteStringFn g_delete;

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

// WinRT's HRESULTs for a call made at the wrong time and an index past the end (not in every mingw winerror.h).
const HRESULT ILLEGAL_METHOD_CALL = HRESULT(0x8000000EL), ILLEGAL_DELEGATE_ASSIGNMENT = HRESULT(0x80000018L), OUT_OF_BOUNDS = HRESULT(0x8000000BL);

const GUID IID_Inspectable = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
const GUID IID_AsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID IID_AsyncAction = {0x5a648006, 0x843a, 0x4da9, {0x86, 0x5b, 0x9d, 0x26, 0xe5, 0xdf, 0xad, 0x7b}};
const GUID IID_AsyncOperationString = {0x3e1fe603, 0xf897, 0x5263, {0xb3, 0x28, 0x08, 0x06, 0x42, 0x6b, 0x8a, 0x79}};
const GUID IID_VectorViewString = {0x2f13c006, 0xa03a, 0x5f69, {0xb0, 0x90, 0x75, 0xa4, 0x3e, 0x33, 0x42, 0x3e}};
const GUID IID_Agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

// The interface ids the wrappers ask the SystemUI statics for.
const GUID IID_SystemUIStatics = {0x1a0dd80e, 0x3f9d, 0x4cfa, {0x8e, 0x50, 0x0e, 0x2e, 0xbd, 0x1a, 0xa7, 0x2f}};
const GUID IID_SystemUIStatics5 = {0x1fac25f1, 0x2fa6, 0x4a68, {0x88, 0xd6, 0x89, 0xd5, 0x45, 0x4a, 0x6c, 0x0f}};

// IVectorView<HSTRING>: the chosen players, each xuid as decimal text.
struct IStringView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, HSTRING* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(HSTRING item, UINT32* index, boolean* found) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 capacity, HSTRING* items, UINT32* actual) = 0;
};

class StringView final : public IStringView {
public:
    explicit StringView(std::vector<std::wstring> items) : items_(std::move(items)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_Inspectable) || IsEqualGUID(iid, IID_Agile) || IsEqualGUID(iid, IID_VectorViewString)) {
            *out = static_cast<IStringView*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        long n = --refs_;
        if (n == 0) delete this;
        return ULONG(n);
    }
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
    HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, HSTRING* item) override {
        if (!item) return E_POINTER;
        *item = nullptr;
        if (index >= items_.size()) return OUT_OF_BOUNDS;
        return g_create(items_[index].c_str(), UINT32(items_[index].size()), item);
    }
    HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) override {
        if (!size) return E_POINTER;
        *size = UINT32(items_.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IndexOf(HSTRING item, UINT32* index, boolean* found) override {
        if (!index || !found) return E_POINTER;
        *index = 0, *found = false;
        UINT32 n = 0;
        PCWSTR w = item ? g_raw(item, &n) : L"";
        for (size_t i = 0; i < items_.size(); i++)
            if (items_[i].size() == n && !memcmp(items_[i].data(), w, n * sizeof(wchar_t))) {
                *index = UINT32(i), *found = true;
                break;
            }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 capacity, HSTRING* items, UINT32* actual) override {
        if (!actual || (capacity && !items)) return E_POINTER;
        *actual = 0;
        for (UINT32 i = start; i < items_.size() && *actual < capacity; i++) {
            HRESULT hr = g_create(items_[i].c_str(), UINT32(items_[i].size()), &items[*actual]);
            if (FAILED(hr)) return hr;
            ++*actual;
        }
        return S_OK;
    }

private:
    ~StringView() {}
    std::atomic<long> refs_{1};
    std::vector<std::wstring> items_;
};

// IAsyncAction, IAsyncOperation<HSTRING> and IAsyncOperation<IVectorView<HSTRING>*> have the same shape (GetResults differs in what it gives).
struct IAsyncWork : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void** result) = 0;
};
struct IAsyncState : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

enum class Gives { Nothing, Text, People };

// What a hooked wrapper returns: Started (0) until the Guide's screen is gone, then Completed (1) or Canceled (2).
class SysAsync final : public IAsyncWork, public IAsyncState {
public:
    explicit SysAsync(Gives gives) : gives_(gives) {}

    // IUnknown (both bases share the count)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_Inspectable) || IsEqualGUID(iid, IID_Agile) ||
            (gives_ == Gives::Nothing && IsEqualGUID(iid, IID_AsyncAction)) || (gives_ == Gives::Text && IsEqualGUID(iid, IID_AsyncOperationString))) {
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
    // IAsyncAction / IAsyncOperation<T>
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
    HRESULT STDMETHODCALLTYPE GetResults(void** result) override {
        // an action's GetResults has no argument: result is only looked at for an operation
        if (gives_ == Gives::Nothing) return status_ == 1 ? S_OK : ILLEGAL_METHOD_CALL;
        if (!result) return E_POINTER;
        *result = nullptr;
        std::lock_guard<std::mutex> l(mu_);
        if (status_ != 1) return ILLEGAL_METHOD_CALL;
        if (gives_ == Gives::People) {
            *result = static_cast<IStringView*>(new StringView(people_));  // the caller's count
            return S_OK;
        }
        if (!text_.empty() && g_create) return g_create(text_.c_str(), UINT32(text_.size()), (HSTRING*)result);
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

    void complete(bool done, const std::string& text, const std::vector<uint64_t>& people) {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (status_ != 0) return;
            int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
            text_.assign(size_t(n), L'\0');
            if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), &text_[0], n);
            if (done)
                for (uint64_t x : people) people_.push_back(std::to_wstring(x));
            // the people picker's caller has nothing that catches a cancelled task: dismissed is "no players", as a list
            status_ = done || gives_ == Gives::People ? 1 : 2;
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
    const Gives gives_;
    std::mutex mu_;
    std::atomic<int> status_{0};
    std::wstring text_;
    std::vector<std::wstring> people_;
    IUnknown* handler_ = nullptr;
    bool fired_ = false;
};

std::mutex g_mu;
std::deque<SysAsk> g_asks;

// Queues what the game asked for; the returned object is the game's (one count), the queue holds another until sysui_complete.
IAsyncWork* ask(SysAsk a, Gives gives) {
    SysAsync* op = new SysAsync(gives);
    op->AddRef();
    a.op = static_cast<IAsyncWork*>(op);
    std::lock_guard<std::mutex> l(g_mu);
    g_asks.push_back(std::move(a));
    return static_cast<IAsyncWork*>(op);
}

typedef void* (*CardFn)(void* user, HSTRING xuid);
typedef void* (*KeyboardFn)(HSTRING def, HSTRING title, HSTRING description, int scope);
typedef void* (*PeopleFn)(void* user, HSTRING prompt, void* from, void* chosen, UINT32 min, UINT32 max);
typedef void (*InviteUiFn)(void* party_manager, void* callback);
CardFn o_card_popup;
KeyboardFn o_keyboard;
PeopleFn o_people;
InviteUiFn o_invite_ui;

// ShowProfileCardAsync(user, xuid): the xuid comes as decimal text.
void* hk_profile_card(void* user, HSTRING xuid) {
    if (!g_can_show) return o_card_popup(user, xuid);
    SysAsk a;
    a.kind = SysAsk::ProfileCard;
    a.xuid = strtoull(utf8_of(xuid).c_str(), nullptr, 10);
    logf("guide: the game asked for a player's gamercard (xuid %llx): the Guide shows their profile", (unsigned long long)a.xuid);
    return ask(a, Gives::Nothing);
}

// ShowVirtualKeyboardAsync(defaultText, titleText, descriptionText, inputScope).
void* hk_virtual_keyboard(HSTRING def, HSTRING title, HSTRING description, int scope) {
    if (!g_can_show) return o_keyboard(def, title, description, scope);
    SysAsk a;
    a.kind = SysAsk::Keyboard;
    a.text = utf8_of(def), a.title = utf8_of(title), a.description = utf8_of(description);
    logf("guide: the game asked for the system keyboard (title '%s', %d characters given, input scope %d): the Guide shows its keyboard", a.title.c_str(),
         int(a.text.size()), scope);
    return ask(a, Gives::Text);
}

// The xuids of one of the game's IVectorView<HSTRING> lists (asked for by its interface id, read with the slots the game reads ours with).
std::vector<uint64_t> xuids_of(void* list) {
    std::vector<uint64_t> out;
    IStringView* view = nullptr;
    if (!list || FAILED(((IUnknown*)list)->QueryInterface(IID_VectorViewString, (void**)&view)) || !view) return out;
    UINT32 n = 0;
    if (SUCCEEDED(view->get_Size(&n)))
        for (UINT32 i = 0; i < n && i < 100; i++) {
            HSTRING h = nullptr;
            if (FAILED(view->GetAt(i, &h))) break;
            uint64_t x = strtoull(utf8_of(h).c_str(), nullptr, 10);
            if (h) g_delete(h);
            if (x) out.push_back(x);
        }
    view->Release();
    return out;
}

// ShowPeoplePickerAsync(user, promptText, selectFrom, preselected, minimum, maximum).
void* hk_people_picker(void* user, HSTRING prompt, void* from, void* chosen, UINT32 min, UINT32 max) {
    if (!g_can_show) return o_people(user, prompt, from, chosen, min, max);
    SysAsk a;
    a.kind = SysAsk::People;
    a.title = utf8_of(prompt), a.people = xuids_of(from), a.chosen = xuids_of(chosen), a.min = min, a.max = max;
    logf("guide: the game asked for the people picker ('%s', %d players to choose from, %d chosen, %u to %u wanted): the Guide shows its player list",
         a.title.c_str(), int(a.people.size()), int(a.chosen.size()), min, max);
    return ask(a, Gives::People);
}

// The party manager's "show the system invite UI": the Guide opens its game invite, and the game's own call goes on as before (its
// task ends without a user under Azimuth and tells the caller so).
void hk_invite_ui(void* party_manager, void* callback) {
    if (g_can_show) {
        logf("guide: the game asked for the invite UI: the Guide opens a game invite");
        if (g_on_invite) g_on_invite();
    }
    o_invite_ui(party_manager, callback);
}

// A wrapper is the known one when, as in build 1.0.0.64, it starts with the frame set-up, puts this interface id on its stack in its
// first 0x50 bytes (four `mov dword ptr [rax+d8], imm32`) and calls this slot of the statics before 0xc0.
bool wrapper_at(const unsigned char* at, const GUID& iid, unsigned char slot) {
    static const unsigned char head[] = {0x48, 0x8b, 0xc4, 0x57, 0x48};
    if (memcmp(at, head, sizeof head)) return false;
    unsigned char id[16];
    memcpy(id, &iid, 16);
    int words = 0;
    for (int i = 5; i < 0x50 && words < 4; i++)
        if (at[i] == 0xc7 && at[i + 1] == 0x40 && !memcmp(at + i + 3, id + words * 4, 4)) words++, i += 6;
    if (words != 4) return false;
    for (int i = 0x50; i < 0xc0; i++)
        if (at[i] == 0xff && (at[i + 1] == 0x50 || (at[i - 1] == 0x41 && at[i + 1] == 0x52)) && at[i + 2] == slot) return true;
    return false;
}

}  // namespace

bool sysui_install(const FH2ModApi* api, FH2Mod* mod, unsigned char* base, void (*log)(const char*), void (*on_invite)()) {
    g_log = log, g_on_invite = on_invite;
    if (HMODULE combase = LoadLibraryW(L"combase.dll")) {
        g_raw = (RawBufferFn)(void*)GetProcAddress(combase, "WindowsGetStringRawBuffer");
        g_create = (CreateStringFn)(void*)GetProcAddress(combase, "WindowsCreateString");
        g_delete = (DeleteStringFn)(void*)GetProcAddress(combase, "WindowsDeleteString");
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
    bool strings = g_raw && g_create && g_delete;
    if (!strings) logf("guide: combase has no HSTRING functions: the game's gamercard, keyboard and people picker calls are left to Azimuth");
    hook("guide: the game's gamercard popup (ShowProfileCardAsync)", 0xe41d30, (void*)hk_profile_card, (void**)&o_card_popup,
         strings && wrapper_at(base + 0xe41d30, IID_SystemUIStatics, 0x40));
    hook("guide: the game's system keyboard (ShowVirtualKeyboardAsync)", 0xed23a0, (void*)hk_virtual_keyboard, (void**)&o_keyboard,
         strings && wrapper_at(base + 0xed23a0, IID_SystemUIStatics, 0x58));
    hook("guide: the game's people picker (ShowPeoplePickerAsync)", 0xb64690, (void*)hk_people_picker, (void**)&o_people,
         strings && wrapper_at(base + 0xb64690, IID_SystemUIStatics5, 0x30));
    static const unsigned char invite_head[] = {0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x40, 0xe0, 0xfe};
    hook("guide: the game's invite UI (the party manager, game+0x17b9700)", 0x17b9700, (void*)hk_invite_ui, (void**)&o_invite_ui,
         memcmp(base + 0x17b9700, invite_head, sizeof invite_head) == 0);
    logf("guide: %d of 4 system UI calls of the game are answered by the Guide (gamercard, keyboard, people picker, invite UI)", on);
    return on > 0;
}

void sysui_showing(bool can) {
    if (g_can_show.exchange(can) == can) return;
    logf(can ? "guide: the game's system UI calls are answered by the Guide from now on (it can draw)"
             : "guide: the Guide cannot draw any more: the game's system UI calls go on to Azimuth");
}

bool sysui_take(SysAsk* out) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_asks.empty()) return false;
    *out = std::move(g_asks.front());
    g_asks.pop_front();
    return true;
}

void sysui_complete(void* op, bool done, const std::string& text, const std::vector<uint64_t>& people) {
    if (!op) return;
    SysAsync* a = static_cast<SysAsync*>(static_cast<IAsyncWork*>(op));
    a->complete(done, text, people);
    a->Release();
}
