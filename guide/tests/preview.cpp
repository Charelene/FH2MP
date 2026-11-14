// guide-preview.exe: draws the Guide off screen with the same code the overlay uses (guide.cpp, assets.cpp, Dear ImGui's DX12 backend) and
// writes a PNG, so the look can be checked against the design canvas without a game.
//   guide-preview.exe <out.png> <scene> [seconds] [gamerpic.png] [background.png]
// GUIDE_THEME in the environment is the theme the scene is drawn in (a theme's key: xbox360, fh2mp, blue-plastic; Xbox without it).
// GUIDE_ACH_STYLE draws the achtoast scenes as the Xbox 360's notification (1) or the Xbox One's toast (2); notifications is
// Settings > Notifications.
// Scenes: home (the Guide, the player's blade), games, settings, down; friends, friends-nogame, party, players, messages; message,
// message-invite, message-request; profile, profile-nogame, profile-personal, profile-request, profile-sent, profile-here; create,
// exit-session; create-nogame; pick, pick-many, compose, compose-invite, discard, keyboard, keyboard-caps, add-friend; box-accept, box-gone;
// own-profile, edit-profile, edit-motto; toast-sent, toast-invite, toast-request, toast-message, toast-online. Sample players, not real ones.
// seconds: time since the Guide opened or the toast started (default 1.5 and 3.0).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <string>
#include <vector>

#include "backends/imgui_impl_dx12.h"
#include "gfx.h"
#include "guide.h"
#include "imgui.h"

namespace {

const UINT W = 1920, H = 1080, SRVS = 64;
ID3D12Device* g_dev;
ID3D12DescriptorHeap* g_srv;
UINT g_srv_size, g_srv_next;

void srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    UINT i = g_srv_next++;
    cpu->ptr = g_srv->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * g_srv_size;
    gpu->ptr = g_srv->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(i) * g_srv_size;
}
void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

std::vector<unsigned char> read_all(const char* path) {
    std::vector<unsigned char> out;
    FILE* f = fopen(path, "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    out.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    out.resize(fread(out.data(), 1, out.size(), f));
    fclose(f);
    return out;
}

ImTextureID picture(ID3D12CommandQueue* q, const char* path) {
    std::vector<unsigned char> png = read_all(path), rgba;
    int w, h;
    if (png.empty() || !decode_png(png.data(), png.size(), &rgba, &w, &h)) return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    srv_alloc(nullptr, &cpu, &gpu);
    return upload_texture(g_dev, q, rgba.data(), w, h, cpu) ? (ImTextureID)gpu.ptr : 0;
}

bool save_png(const char* path, const unsigned char* bgra, UINT stride) {
    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateStream(&st)) &&
              SUCCEEDED(st->InitializeFromFilename(wpath, GENERIC_WRITE)) && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
              SUCCEEDED(fr->SetSize(W, H)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
              SUCCEEDED(fr->WritePixels(H, stride, stride * H, (BYTE*)bgra)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: guide-preview <out.png> <scene> [seconds | from:to:fps] [gamerpic.png] [background.png]\n"
                        "  from:to:fps writes one picture per frame, <out>-000.png and so on\n"
                        "  achtoast-normal | achtoast-rare | achtoast-mythic (add -noart to draw without the Xbox app's art): the app's folders are\n"
                        "  given in GUIDE_XBOX_NOTIFICATIONS and GUIDE_GAMEBAR_ASSETS\n");
        return 2;
    }
    std::string scene = argv[2];
    bool is_toast = scene.rfind("toast", 0) == 0;
    double secs = argc > 3 ? atof(argv[3]) : (is_toast ? 3.0 : 1.5);
    double seq_from = 0, seq_to = 0, seq_fps = 0;  // a frame sequence instead of one picture
    if (argc > 3 && sscanf(argv[3], "%lf:%lf:%lf", &seq_from, &seq_to, &seq_fps) == 3 && seq_fps > 0) secs = seq_from;
    else seq_fps = 0;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev))) || FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) {
        fprintf(stderr, "no Direct3D 12 device\n");
        return 1;
    }
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, rb{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, td.Width = W, td.Height = H, td.DepthOrArraySize = 1, td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM, td.SampleDesc.Count = 1, td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{DXGI_FORMAT_B8G8R8A8_UNORM, {0.16f, 0.2f, 0.22f, 1}};
    ID3D12Resource *rt = nullptr, *readback = nullptr;
    UINT pitch = (W * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, bd.Width = UINT64(pitch) * H, bd.Height = 1, bd.DepthOrArraySize = 1, bd.MipLevels = 1;
    bd.SampleDesc.Count = 1, bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt));
    g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC rd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRVS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    g_dev->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtv_heap));
    g_dev->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_srv));
    g_srv_size = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_dev->CreateRenderTargetView(rt, nullptr, rtv);
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    list->Close();
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    auto wait = [&] {
        q->Signal(fence, ++fv);
        if (fence->GetCompletedValue() < fv) fence->SetEventOnCompletion(fv, ev), WaitForSingleObject(ev, 5000);
    };

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    GuideAssets assets;
    load_fonts(assets);
    ImGui_ImplDX12_InitInfo info;
    info.Device = g_dev, info.CommandQueue = q, info.NumFramesInFlight = 1, info.RTVFormat = DXGI_FORMAT_B8G8R8A8_UNORM, info.SrvDescriptorHeap = g_srv;
    info.SrvDescriptorAllocFn = srv_alloc, info.SrvDescriptorFreeFn = srv_free;
    if (!ImGui_ImplDX12_Init(&info)) return 1;
    if (argc > 4) assets.gamerpic = picture(q, argv[4]);
    if (scene.rfind("achtoast", 0) == 0 && scene.find("noart") == std::string::npos) {
        // the achievement toast's art, from the Xbox app's own folders on this PC
        const char* dirs[2] = {getenv("GUIDE_XBOX_NOTIFICATIONS"), getenv("GUIDE_GAMEBAR_ASSETS")};
        for (int where = 0; where < 2; where++)
            for (const char* const* f = ach_art_files(where); dirs[where] && *f; f++) {
                ImTextureID tex = picture(q, (std::string(dirs[where]) + "\\" + *f).c_str());
                if (!tex) fprintf(stderr, "no %s in %s\n", *f, dirs[where]);
                ach_art_set(&assets.ach, *f, tex);
            }
    }
    ImTextureID bg = argc > 5 ? picture(q, argv[5]) : 0;

    if (const char* theme = getenv("GUIDE_THEME")) theme_apply(theme);
    Guide g;
    if (const char* style = getenv("GUIDE_STYLE")) g.set_guide_style(atoi(style));  // 1: the Xbox One guide's layout
    g.gamertag = "DebugFox";
    const double t0 = 100.0;
    // sample players for the pages (not real ones)
    Social so;
    so.session = so.loaded = true;
    so.me.xuid = 1, so.me.tag = "DebugFox", so.me.state = Friend::Playing, so.me.lobby_id = "L1", so.me.lobby_kind = "freeroam";
    so.me.level = 27, so.me.has_profile = true, so.me.motto = "S1 or nothing", so.me.location = "Kyiv";
    auto card = [&](uint64_t x, const char* tag, Friend::State st, const char* lobby, const char* status, const char* club, int level) {
        Friend f;
        f.xuid = x, f.tag = tag, f.state = st, f.lobby_id = lobby, f.lobby_kind = *lobby ? "freeroam" : "", f.lobby_status = status, f.club = club, f.level = level;
        f.lobby_players = *lobby ? 3 : 0, f.lobby_capacity = *lobby ? 12 : 0;
        return f;
    };
    so.friends.push_back(card(2, "DebugWolf", Friend::Playing, "L1", "Free Roam", "", 12));
    so.friends.push_back(card(3, "NightRunR", Friend::Playing, "L2", "Free Roam", "APEX", 41));
    so.friends.push_back(card(4, "Kestrel 88", Friend::Online, "", "", "", 7));
    so.friends.push_back(card(5, "torque.tuner", Friend::Playing, "L3", "Road Trip: Nice", "", 29));
    so.friends.push_back(card(6, "Vellum", Friend::Offline, "", "", "", 3));
    so.friends.push_back(card(7, "SaltLakeSam", Friend::Offline, "", "", "APEX", 55));
    so.friends.push_back(card(8, "drift_mum", Friend::Online, "", "", "", 18));
    so.friends[4].last_seen = 2 * 86400 + 600;
    so.friends[1].motto = "meet at the nice docks after 9", so.friends[1].location = "Nice, France", so.friends[1].bio = "Drifts mostly. Runs the APEX Thursday bridge meet.";
    so.requests.push_back(card(9, "PaperLantern", Friend::Online, "", "", "", 22));
    so.outgoing.push_back(card(10, "Ugandan Hobo", Friend::Offline, "", "", "", 31));
    if (scene.find("nogame") != std::string::npos) so.me.lobby_id = "", so.me.state = Friend::Online;
    if (scene == "party-private") {
        // DebugFox's private game: DebugWolf leads it, apex hunter 7 is talking
        so.me.lobby_private = true, so.me.lobby_leader = 2;
        so.players.push_back(so.friends[0]);
        so.players.push_back(card(11, "apex hunter 7", Friend::Playing, "L1", "Free Roam", "APEX", 44));
        const uint64_t talking[] = {11};
        g.set_talking(talking, 1);
    }
    if (scene == "players") {
        // who is in DebugFox's game: a friend, a stranger and someone who asked to be a friend; the stranger is talking
        so.players.push_back(so.friends[0]);
        so.players.push_back(card(11, "apex hunter 7", Friend::Playing, "L1", "Free Roam", "APEX", 44));
        so.players.push_back(card(9, "PaperLantern", Friend::Playing, "L1", "Free Roam", "", 22));
        const uint64_t talking[] = {11};
        g.set_talking(talking, 1);
    }
    ReceivedInvite iv;
    iv.id = 9, iv.from = so.friends[1], iv.kind = "freeroam", iv.age = 75, iv.expires_in = 525, iv.text = "meet at the docks";
    so.invites.push_back(iv);
    TextMessage tm;
    tm.id = 17, tm.from = 4, tm.tag = "Kestrel 88", tm.body = "you up for the Castello sprint later? bring the M3", tm.chat = "4", tm.created = double(time(nullptr)) - 3 * 3600, tm.unread = true;
    so.texts.push_back(tm);
    tm.id = 9, tm.from = 8, tm.tag = "drift_mum", tm.body = "gg", tm.chat = "8", tm.created = double(time(nullptr)) - 30 * 3600, tm.unread = false;
    so.texts.push_back(tm);
    // NightRunR's achievements as the server has them (their Guide reported them)
    so.friends[1].gamerscore = 160;
    if (scene != "compare-loading")
        ach_parse("[unlocked]\nwelcome_festival=2026-09-01 10:00:00 UTC\nwelcome_sisteron=2026-09-01 11:00:00 UTC\nwelcome_castelletto=2026-09-02 11:00:00 UTC\n"
                  "horizon_leader=2026-09-02 12:00:00 UTC\nwristband_yellow=2026-09-01 10:30:00 UTC\nwristband_green=2026-09-03 10:30:00 UTC\n"
                  "wristband_pink=2026-09-05 10:30:00 UTC\nfirst_of_many=2026-09-01 10:40:00 UTC\nnew_car_smell=2026-09-01 10:10:00 UTC\ncar_lover=2026-10-01 10:10:00 UTC\n",
                  &so.ach_of[3]),
            so.ach_of[3].known = true;
    g.set_social(so);
    if (scene.find("noach") == std::string::npos) {
        // the Achievements mod's file, as it writes it
        AchUnlocks au;
        ach_parse("; Achievements unlocked offline (Achievements mod). Delete this file to start over.\n[unlocked]\n"
                  "welcome_festival=2026-10-20 18:02:11 UTC\nwelcome_sisteron=2026-10-20 18:40:03 UTC\nwelcome_nice=2026-10-21 19:12:45 UTC\n"
                  "wanderlust=2026-10-21 19:13:02 UTC\nwristband_yellow=2026-10-20 18:25:30 UTC\nwristband_green=2026-10-22 20:01:00 UTC\n"
                  "first_of_many=2026-10-20 18:31:09 UTC\nnumber_one=2026-10-20 19:05:44 UTC\nnew_car_smell=2026-10-20 18:10:00 UTC\n"
                  "license_to_skill=2026-10-21 20:20:20 UTC\nsummer_sale=2026-10-23 21:00:00 UTC\nrain_master=2026-10-22 20:30:00 UTC\n"
                  "social_caterpillar=2026-11-08 22:15:00 UTC\non_the_road_again=2026-11-09 21:40:00 UTC\nwelcome_storm_island=2026-11-01 17:00:00 UTC\n"
                  "porsche_owners_club=2026-11-02 17:30:00 UTC\n\n[counters]\nskill.Drift.2293=7\n",
                  &au);
        au.known = true;
        g.set_achievements(au);
    }
    if (assets.gamerpic) assets.pics[2] = assets.gamerpic;
    auto press = [&](std::initializer_list<PadEvent> evs, double at = 99.0) {
        for (PadEvent e : evs) g.handle(e, at);
    };
    const PadEvent D = PadEvent::Down, A = PadEvent::A, RB = PadEvent::Rb;
    // to the Settings tab from where the Guide opens: one to the right in the 360's layout, three in the Xbox One's
    const int to_settings = getenv("GUIDE_STYLE") && atoi(getenv("GUIDE_STYLE")) == 1 ? 3 : 1;
    auto settings = [&](double at) {
        for (int i = 0; i < to_settings; i++) g.handle(RB, at);
    };
    NetResult nr;
    if (scene == "friends" || scene == "friends-nogame") {
        g.open_community(TabFriends, t0 - 5), press({D, D});
    } else if (scene == "theme" || scene == "theme-plastic" || scene == "theme-fh2mp" || scene == "theme-360") {
        // Settings > Theme with the focus moved: Orange on the first tab, Blue Plastic on the second, FH2MP, Xbox 360
        // Settings > Theme as the player gets there: the Guide, RB to Settings, down to Theme, A
        g.toggle(t0 - 10), settings(t0 - 9), g.handle(D, t0 - 8), g.handle(A, t0 - 7);
        if (scene == "theme") press({D, D, D, D});
        if (scene == "theme-plastic") g.handle(RB, t0 - 6), press({D, D, D, D});
        if (scene == "theme-fh2mp") press({D, D});
        if (scene == "theme-360") press({D});

    } else if (scene == "notifications" || scene == "notifications-one" || scene == "notifications-360") {
        // Settings > Notifications, the focus on Guide or moved down to Xbox One or Xbox 360
        g.toggle(t0 - 10), settings(t0 - 9), press({D, D, D}, t0 - 8), g.handle(A, t0 - 7);
        if (scene == "notifications-one") press({D});
        if (scene == "notifications-360") press({D, D});
    } else if (scene == "system") {
        // Settings > System: the Guide's layout
        g.toggle(t0 - 10), settings(t0 - 9), press({D, D, D, D, D, D}, t0 - 8), g.handle(A, t0 - 7);
    } else if (scene == "voice") {
        // Settings > Voice with the microphone muted and the focus on Volume
        g.set_voice(false, true, 3);
        g.toggle(t0 - 10), settings(t0 - 9), g.handle(D, t0 - 8), g.handle(D, t0 - 7.5), g.handle(A, t0 - 7), press({D, D});
    } else if (scene == "party" || scene == "party-private") {
        g.open_community(TabParty, t0 - 5);
    } else if (scene == "players") {
        g.open_community(TabPlayers, t0 - 5);
    } else if (scene == "messages") {
        g.open_community(TabMessages, t0 - 5), press({D, D});
    } else if (scene == "message" || scene == "message-invite" || scene == "message-request" || scene == "exit-session") {
        g.open_community(TabMessages, t0 - 5);
        int downs = scene == "message-request" ? 1 : scene == "message" ? 3 : 2;
        for (int i = 0; i < downs; i++) g.handle(D, 99.0);
        g.handle(A, scene == "exit-session" ? t0 - 3 : t0);
        if (scene == "exit-session") g.handle(A, t0);  // Accept Game Invite while in a game
    } else if (scene == "compare" || scene == "compare-loading" || scene == "compare-down" || scene == "ach-detail") {
        // Friends > NightRunR > Compare Games (after Invite to Game, Join Session In Progress, Send Message)
        g.open_community(TabFriends, t0 - 5);
        for (int i = 0; i < 3; i++) g.handle(D, 99.0);
        g.handle(A, t0 - 4), press({D, D, D});
        g.handle(A, scene == "ach-detail" ? t0 - 3 : t0);
        if (scene == "compare-down") press({D, D, D, D, D, D, D, D, D});
        if (scene == "ach-detail") press({D, D, D, D, D, D}), g.handle(A, t0);
    } else if (scene.rfind("profile", 0) == 0) {
        // friends tab rows: Add Friend, PaperLantern (request), DebugWolf, NightRunR, torque.tuner, drift_mum, Kestrel 88, Ugandan Hobo (sent), ...
        g.open_community(TabFriends, t0 - 5);
        int downs = scene == "profile-request" ? 1 : scene == "profile-here" ? 2 : scene == "profile-sent" ? 7 : 3;
        for (int i = 0; i < downs; i++) g.handle(D, 99.0);
        g.handle(A, t0);
        if (scene == "profile-personal") g.handle(RB, t0);
    } else if (scene == "create" || scene == "create-nogame") {
        g.open_community(TabMessages, t0 - 5), g.handle(A, t0);
    } else if (scene == "pick") {
        g.open_invite(t0);
    } else if (scene == "invite-nogame") {
        // the game's Invite Friends while the Guide's picture says "no game": the invite is written all the same
        g.open_invite(t0 - 5), g.handle(A, t0);
    } else if (scene == "pick-many") {
        g.open_invite(t0 - 5), press({A, D});  // NightRunR, then Edit Recipients
        g.handle(A, t0), press({D, D, A, D});  // tick torque.tuner as well
    } else if (scene == "compose-invite") {
        g.open_invite(t0 - 5), g.handle(A, t0);
    } else if (scene == "compose" || scene == "discard" || scene == "keyboard" || scene == "keyboard-caps") {
        g.open_community(TabMessages, t0 - 5), press({A, A, D, D, D}), g.handle(A, t0 - 3);  // Create New, Message, a friend
        g.handle(D, t0 - 3), g.handle(D, t0 - 3), g.handle(A, t0 - 2);                        // Add Text
        if (scene == "keyboard-caps") g.handle(PadEvent::L3, t0 - 2), g.handle(PadEvent::Rt, t0 - 2);
        for (const char* c = "On my way, 5 min"; *c; c++) g.type_char(uint32_t(*c), t0 - 2);
        if (scene == "compose" || scene == "discard") g.handle(PadEvent::Start, t0);
        if (scene == "discard") g.handle(PadEvent::B, t0);
    } else if (scene == "add-friend") {
        g.open_community(TabFriends, t0 - 5), g.handle(A, t0);
        for (const char* c = "Charelene"; *c; c++) g.type_char(uint32_t(*c), t0);
    } else if (scene == "own-profile" || scene == "edit-profile" || scene == "edit-motto") {
        g.toggle(t0 - 9), settings(t0 - 8);
        g.handle(A, scene == "own-profile" ? t0 : t0 - 5);
        if (scene != "own-profile") g.handle(A, scene == "edit-profile" ? t0 : t0 - 3);
        if (scene == "edit-motto") g.handle(A, t0);
    } else if (scene == "box-accept") {
        g.toggle(t0 - 5);
        nr.op = NetOp::Accept, nr.ok = true, nr.xuid = 3, nr.name = "NightRunR", nr.kind = "freeroam";
        g.on_result(nr, assets, t0);
    } else if (scene == "box-gone") {
        g.toggle(t0 - 5);
        nr.op = NetOp::Accept, nr.ok = false, nr.xuid = 3, nr.name = "NightRunR", nr.code = "lobby_gone";
        g.on_result(nr, assets, t0);
    } else if (scene == "game-card" || scene == "game-card-unknown") {
        g.open_profile_for_game(scene == "game-card" ? 3 : 77, t0);
    } else if (scene == "game-people" || scene == "game-people-full" || scene == "game-people-one") {
        // the game's Car Club invite: three friends to choose from, two at most (or one)
        bool one = scene == "game-people-one";
        g.open_people_for_game("Invite friends to your club", {3, 2, 4}, scene == "game-people-full" ? std::vector<uint64_t>{3, 4} : std::vector<uint64_t>{3}, 1,
                               one ? 1 : 2, t0);
        if (scene == "game-people-full") g.handle(PadEvent::Down, t0), g.handle(PadEvent::Down, t0);
    } else if (scene == "game-keyboard") {
        g.open_keyboard_for_game("APEX", "Club Tag", "Enter a tag for your club.", t0);
    } else if (scene == "achievements" || scene == "achievements-locked" || scene == "achievements-end") {
        g.toggle(t0 - 10), g.handle(PadEvent::Lb, t0 - 9), g.handle(D, t0 - 8), g.handle(A, t0);  // Games > Achievements
        if (scene == "achievements-locked") press({PadEvent::Right, PadEvent::Right, PadEvent::Right, D});
        if (scene == "achievements-end") press({D, D, D, D, D, D, D, D});
    } else if (scene.rfind("achtoast", 0) == 0) {
        if (const char* style = getenv("GUIDE_ACH_STYLE")) g.set_notify(atoi(style));
        if (scene.find("mythic") != std::string::npos) g.ach_toast(AchTier::Mythic, -1, "Forza Horizon 2", "50 of 50 Achievements", -1, -1, t0);
        else if (scene.find("rare") != std::string::npos)
            g.ach_toast(AchTier::Rare, 50, "All Your Race Are Belong To Us 2.0", "Complete all 168 Championships.", 3.12f, 18, t0);
        else g.ach_toast(AchTier::Normal, 10, "Rain Master", "Complete your first race in the rain.", -1, 34, t0);
    } else if (scene == "open-on-toast") {
        nr.op = NetOp::Received, nr.ok = true, nr.xuid = 3, nr.name = "NightRunR", nr.id = 9;
        g.on_result(nr, assets, t0 - 2);
        g.toggle(t0);  // the Guide button while the invite's toast plays
    } else if (is_toast) {
        nr.ok = true, nr.xuid = 2, nr.name = "DebugWolf";
        nr.op = scene == "toast-sent" ? NetOp::Invite : scene == "toast-invite" ? NetOp::Received : scene == "toast-request" ? NetOp::GotRequest
                : scene == "toast-message" ? NetOp::GotText : NetOp::Online;
        g.on_result(nr, assets, t0);
    } else {
        g.toggle(t0 - 10);
        if (scene == "games") g.handle(PadEvent::Lb, t0 - 5);
        if (scene == "settings") settings(t0 - 5);
        if (scene == "down") g.handle(PadEvent::Down, t0 - 5);
        g.toggle(t0 - 1), g.toggle(t0);  // closed and opened again: the open animation starts at t0
    }

    int shots = seq_fps > 0 ? int((seq_to - seq_from) * seq_fps) + 1 : 1;
    for (int shot = 0; shot < shots; shot++) {
    if (seq_fps > 0) secs = seq_from + shot / seq_fps;
    for (int frame = shot ? 1 : 0; frame < 2; frame++) {
        io.DisplaySize = ImVec2(float(W), float(H));
        io.DeltaTime = 1.0f / 60;
        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        if (bg) ImGui::GetBackgroundDrawList()->AddImage(bg, ImVec2(0, 0), ImVec2(float(W), float(H)));
        g.draw(ImGui::GetForegroundDrawList(), assets, float(W), float(H), t0 + secs);
        ImGui::Render();
        alloc->Reset();
        list->Reset(alloc, nullptr);
        list->ClearRenderTargetView(rtv, cv.Color, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->SetDescriptorHeaps(1, &g_srv);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
        if (frame == 1) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b.Transition.pResource = rt, b.Transition.Subresource = 0;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET, b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}, src{rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            dst.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, W, H, 1, pitch};
            src.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        list->Close();
        ID3D12CommandList* lists[] = {list};
        q->ExecuteCommandLists(1, lists);
        wait();
    }
    void* map = nullptr;
    D3D12_RANGE all{0, SIZE_T(pitch) * H};
    readback->Map(0, &all, &map);
    std::vector<unsigned char> px((size_t)pitch * H);
    memcpy(px.data(), map, px.size());
    readback->Unmap(0, nullptr);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    std::string out_path = argv[1];
    if (seq_fps > 0) {
        char n[16];
        snprintf(n, sizeof n, "-%03d.png", shot);
        size_t dot = out_path.rfind(".png");
        out_path = (dot == std::string::npos ? out_path : out_path.substr(0, dot)) + n;
    }
    if (!save_png(out_path.c_str(), px.data(), pitch)) {
        fprintf(stderr, "could not write %s\n", out_path.c_str());
        return 1;
    }
    if (!seq_fps || shot == shots - 1) printf("wrote %s (%s at %.2f s%s)\n", out_path.c_str(), scene.c_str(), secs, seq_fps > 0 ? ", the last of the sequence" : "");
    // back to a render target for the next picture
    if (shot + 1 < shots) {
        alloc->Reset();
        list->Reset(alloc, nullptr);
        D3D12_RESOURCE_BARRIER back{};
        back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, back.Transition.pResource = rt, back.Transition.Subresource = 0;
        back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE, back.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &back);
        list->Close();
        ID3D12CommandList* ls[] = {list};
        q->ExecuteCommandLists(1, ls);
        wait();
    }
    }
    ImGui_ImplDX12_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
