// ================================================================ in-game editor (F2)
// Two windows: the editor itself (a normal, movable window with all the tabs) and a transparent
// click-through layer over the game that only draws the move/rotate/scale axis and handles click-to-select.
// F2 (pressed while the game or the editor has focus) shows/hides both. Edits are sent to the LauncherBridge
// mod, which applies them in-game. Included from app.cpp so it can share the launcher's theme, fonts and widgets.

static HWND g_ovHwnd = nullptr;
static IDXGISwapChain* g_ovSwap = nullptr;
static ID3D11RenderTargetView* g_ovRtv = nullptr;
static ImGuiContext* g_ovCtx = nullptr;
static ImGuiContext* g_mainCtx = nullptr;
struct FontSet { ImFont *body, *med, *small, *h2, *title; };
static FontSet g_ovFonts{}, g_edFonts{};
static HWND g_gameHwnd = nullptr;
static std::atomic<bool> g_f2Toggle{ false };
static std::atomic<int> g_moveReq{ 0 };            // walk/fly/ghost hotkey pressed while playing (1/2/3)
static bool g_snapPick = false;                    // next click on the game: put the selection there
static std::atomic<bool> g_capture{ false };      // editor is grabbing mouse + keyboard
static std::atomic<bool> g_dialogOpen{ false };   // a Windows file dialog is open: let the mouse and keyboard reach it
static std::atomic<ULONGLONG> g_lastToggle{ 0 };  // F2 debounce (one press = one toggle)
static void RequestToggle() {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastToggle.load() < 350) return;
    g_lastToggle = now;
    g_f2Toggle = true;
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
}
static UINT g_ovResizeW = 0, g_ovResizeH = 0;
// the editor window
static HWND g_edWnd = nullptr;
static IDXGISwapChain* g_edSwap = nullptr;
static ID3D11RenderTargetView* g_edRtv = nullptr;
static ImGuiContext* g_edCtx = nullptr;
static FontSet* g_edFontsP = nullptr;
static UINT g_edResizeW = 0, g_edResizeH = 0;
static bool g_edPlaced = false;
// small always-on-top window that shows the editor's cursor while the game keeps the real one
static HWND g_curWnd = nullptr;
static int g_curShape = -1;

// editor data from the game
static EdState g_ed;
static std::vector<EdProp> g_props;
static std::string g_propsFor;
static std::vector<EdActor> g_actors;
static int g_actorTotal = 0, g_lastMsgId = 0;
// models tab
static std::vector<EdModel> g_models;               // loaded in the game right now
static int g_modelsTotal = 0, g_modelSrc = 0;        // 0 = loaded now, 1 = all game files
static bool g_modelsAsked = false;
static char g_modelFilter[128] = "", g_modelPath[260] = "";
static std::vector<std::string> g_pakModels; static std::string g_pakModelsNote;
static std::vector<std::string> g_pakLower;          // same list, lower case (search)
static std::vector<uint8_t> g_pakFlags;              // bit 0 character (skeletal), bit 1 can be exported
static std::atomic<int> g_pakModelsState{ 0 };       // 0 not read, 1 reading, 2 ready
static std::atomic<int> g_pakDone{ 0 }, g_pakTotal{ 0 };
static std::mutex g_pakModelsMx;
static std::wstring g_pakFor;                        // the game the list is for
static int g_pakKind = 0;                            // 0 everything, 1 objects, 2 characters
static std::set<std::string> g_pakTicked;            // model paths ticked for exporting together
static std::string g_placePath, g_placeName;         // model waiting to be placed with a click
struct Thumb { int state = 0; ID3D11ShaderResourceView* srv = nullptr; ULONGLONG askedAt = 0, checkedAt = 0; };   // 0 new, 1 asked, 2 ready, 3 failed
static std::unordered_map<std::string, Thumb> g_thumbs;
static std::vector<std::pair<std::string, std::string>> g_thumbAsk;   // id, path waiting to be sent
static ULONGLONG g_thumbSentAt = 0;
static bool g_thumbDirSent = false;
static std::wstring g_thumbDirFor;

struct Toast { std::string text; bool err; ULONGLONG t; };
static std::vector<Toast> g_toasts;
static char g_actorFilter[128] = "", g_propFilter[128] = "", g_consoleBuf[256] = "";
static std::vector<std::string> g_consoleHist;
static int g_edSelectTab = -1;             // switch tab next frame
static ImVec2 g_clickPos; static ULONGLONG g_clickAt = 0;   // click ripple

static FontSet SaveFonts() { return { F_body, F_med, F_small, F_h2, F_title }; }
static void UseFonts(const FontSet& f) { F_body = f.body; F_med = f.med; F_small = f.small; F_h2 = f.h2; F_title = f.title; }
static void Ed(const std::string& verb, const std::string& arg = "") { BridgeSend(verb, arg); }
static std::string F3s(const float* v) { char b[96]; snprintf(b, sizeof b, "%.3f,%.3f,%.3f", v[0], v[1], v[2]); return b; }

// ---- game window
struct FindWin { DWORD pid; HWND best; long area; };
static void UeSpawnAt(const std::string& path, float nx, float ny);   // (ueui.inc)
static bool UeWantEdWindow();
static ImVec2 g_ueGamePos(0, 0), g_ueGameSize(0, 0);   // the game's picture inside the layer
static ImVec2 g_ueVpMinF(0, 0), g_ueVpMaxF(0, 0);   // (the viewport: the part of the game not under a panel)
static std::string g_dragPath;                       // dragged out of a panel onto the game
static ULONGLONG g_olRefreshAt = 0;                  // the Outliner asks for the actor list again then
static BOOL CALLBACK FindWinProc(HWND h, LPARAM lp) {
    auto* f = (FindWin*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    RECT r; GetClientRect(h, &r);
    long a = (r.right - r.left) * (r.bottom - r.top);
    if (a > f->area) { f->area = a; f->best = h; }
    return TRUE;
}
static HWND FindGameWindow(DWORD pid) {
    if (!pid) return nullptr;
    FindWin f{ pid, nullptr, 0 };
    EnumWindows(FindWinProc, (LPARAM)&f);
    return f.best;
}
static bool GameClientRect(RECT& out) {
    if (!g_gameHwnd || !IsWindow(g_gameHwnd) || IsIconic(g_gameHwnd)) return false;
    RECT r; if (!GetClientRect(g_gameHwnd, &r)) return false;
    POINT tl{ 0, 0 }; ClientToScreen(g_gameHwnd, &tl);
    out = { tl.x, tl.y, tl.x + (r.right - r.left), tl.y + (r.bottom - r.top) };
    return out.right > out.left && out.bottom > out.top;
}

static bool CaptureActive();
ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);   // from imgui_impl_win32.cpp
// ---- F2 watcher (runs on its own thread so it reacts instantly)
static int VkFromImGuiKey(int key) {
    if (key <= 0) return 0;
    for (int vk = 1; vk < 255; vk++) if ((int)ImGui_ImplWin32_KeyEventToImGuiKey(vk, 0) == key) return vk;
    return 0;
}
static DWORD WINAPI F2Thread(LPVOID) {
    bool was = false;
    bool mwas[3] = { false, false, false };
    for (;;) {
        // walk / fly / ghost hotkeys while you're playing (the editor handles them itself while it's open)
        if (!g_ovVisible && g_bridge.connected) {
            HWND fg = GetForegroundWindow(); DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
            bool inGame = pid && pid == g_gamePid.load();
            int keys[3] = { g_edKeys.walk, g_edKeys.fly, g_edKeys.ghost };
            for (int i = 0; i < 3; i++) {
                int vk = VkFromImGuiKey(keys[i]);
                bool d = vk && inGame && (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (d && !mwas[i]) { g_moveReq = i + 1; PostMessageW(g_hwnd, WM_NULL, 0, 0); }
                mwas[i] = d;
            }
        }
        bool down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        if (down && !was) {
            HWND fg = GetForegroundWindow();
            DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
            if (!CaptureActive() && fg && (fg == g_ovHwnd || fg == g_edWnd || (pid && pid == g_gamePid.load()))) RequestToggle();
        }
        was = down;
        Sleep(15);
    }
    return 0;
}

// ---- input capture while the editor is open
// The game keeps keyboard focus (so it doesn't pause). Low-level hooks catch the mouse and keyboard, hand them to
// the editor and hold them back from the game. Only active while the game is the foreground window, and
// Alt+Tab / Windows key / Ctrl+Alt+Del always work.

struct InEvt { int type; float x, y; int a; bool down; };   // 0 pos, 1 button, 2 wheel, 3 key, 4 char
static std::mutex g_inMx;
static std::vector<InEvt> g_inQ;
static HANDLE g_hookThread = nullptr;
static DWORD g_hookThreadId = 0;
static POINT g_virt{ 0, 0 };                       // our own cursor position (screen pixels)
static POINT g_lastPt{ 0, 0 };                     // last raw position reported by the mouse hook
static RECT g_capRect{ 0, 0, 0, 0 };               // game client area (screen), guarded by g_inMx
static RECT g_deskRect{ 0, 0, 0, 0 };              // whole desktop (all monitors), guarded by g_inMx
static BYTE g_keys[256];                           // key states as we see them (the system never sees blocked keys)

static bool GameIsForeground() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid && pid == g_gamePid.load();
}
static bool CaptureActive() { return g_capture && !g_dialogOpen && GameIsForeground(); }
static void PushIn(const InEvt& e) { std::lock_guard<std::mutex> lk(g_inMx); g_inQ.push_back(e); }

static LRESULT CALLBACK LLMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_capture && !g_dialogOpen && GameIsForeground()) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        switch (wp) {
        case WM_MOUSEMOVE: {
            // Windows: the blocked cursor stays put, and pt is where it would have gone, so pt - cursor = this move.
            // Wine reports the real (unblockable) pointer position instead, so there we just follow pt.
            static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
            if (wine) {
                std::lock_guard<std::mutex> lk(g_inMx);
                g_virt.x = std::clamp(m->pt.x, g_deskRect.left, g_deskRect.right - 1);
                g_virt.y = std::clamp(m->pt.y, g_deskRect.top, g_deskRect.bottom - 1);
                g_inQ.push_back({ 0, (float)g_virt.x, (float)g_virt.y, 0, false });
                break;
            }
            POINT cur; GetCursorPos(&cur);
            long dx = m->pt.x - cur.x, dy = m->pt.y - cur.y;
            g_lastPt = m->pt;
            std::lock_guard<std::mutex> lk(g_inMx);
            g_virt.x = std::clamp(g_virt.x + dx, g_deskRect.left, g_deskRect.right - 1);
            g_virt.y = std::clamp(g_virt.y + dy, g_deskRect.top, g_deskRect.bottom - 1);
            g_inQ.push_back({ 0, (float)g_virt.x, (float)g_virt.y, 0, false });
            break;
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: PushIn({ 1, 0, 0, 0, wp == WM_LBUTTONDOWN }); break;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: PushIn({ 1, 0, 0, 1, wp == WM_RBUTTONDOWN }); break;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: PushIn({ 1, 0, 0, 2, wp == WM_MBUTTONDOWN }); break;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: PushIn({ 1, 0, 0, HIWORD(m->mouseData) == XBUTTON2 ? 4 : 3, wp == WM_XBUTTONDOWN }); break;
        case WM_MOUSEWHEEL: PushIn({ 2, 0, (float)(short)HIWORD(m->mouseData) / WHEEL_DELTA, 0, false }); break;
        case WM_MOUSEHWHEEL: PushIn({ 2, -(float)(short)HIWORD(m->mouseData) / WHEEL_DELTA, 0, 0, false }); break;
        }
        PostMessageW(g_hwnd, WM_NULL, 0, 0);
        return 1;                                   // the game doesn't see it
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK LLKeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_capture && !g_dialogOpen && GameIsForeground()) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        UINT vk = k->vkCode;
        bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        bool alt = (k->flags & LLKHF_ALTDOWN) != 0;
        // always let these through: Windows key, Alt+Tab, Alt+Esc, Alt+F4
        if (vk == VK_LWIN || vk == VK_RWIN || (alt && (vk == VK_TAB || vk == VK_ESCAPE || vk == VK_F4)))
            return CallNextHookEx(nullptr, code, wp, lp);
        if (vk == VK_F2) {                          // our own toggle
            if (down && !(g_keys[VK_F2] & 0x80)) RequestToggle();
            g_keys[VK_F2] = down ? 0x80 : 0;
            return 1;
        }
        g_keys[vk] = down ? 0x80 : 0;
        auto pair = [&](int a, int b, int both) { g_keys[both] = (g_keys[a] | g_keys[b]) & 0x80; };
        pair(VK_LSHIFT, VK_RSHIFT, VK_SHIFT); pair(VK_LCONTROL, VK_RCONTROL, VK_CONTROL); pair(VK_LMENU, VK_RMENU, VK_MENU);
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) g_keys[vk] = down ? 0x80 : 0;
        LPARAM fake = ((LPARAM)k->scanCode << 16) | ((k->flags & LLKHF_EXTENDED) ? (1 << 24) : 0);
        PushIn({ 3, 0, 0, (int)ImGui_ImplWin32_KeyEventToImGuiKey(vk, fake), down });
        if (down && !(g_keys[VK_CONTROL] & 0x80) && !(g_keys[VK_MENU] & 0x80)) {
            BYTE st[256]; memcpy(st, g_keys, sizeof st);
            if (GetKeyState(VK_CAPITAL) & 1) st[VK_CAPITAL] = 1;
            wchar_t buf[8] = {};
            HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
            int n = ToUnicodeEx(vk, k->scanCode, st, buf, 8, 0x4 /* don't change keyboard state */, layout);
            for (int i = 0; i < n; i++) if (buf[i] >= 32) PushIn({ 4, 0, 0, (int)buf[i], true });
        }
        PostMessageW(g_hwnd, WM_NULL, 0, 0);
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static DWORD WINAPI HookThread(LPVOID) {
    HHOOK km = SetWindowsHookExW(WH_MOUSE_LL, LLMouseProc, GetModuleHandleW(nullptr), 0);
    HHOOK kk = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyProc, GetModuleHandleW(nullptr), 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (km) UnhookWindowsHookEx(km);
    if (kk) UnhookWindowsHookEx(kk);
    return 0;
}
static void StartCapture(const RECT& r) {
    {
        std::lock_guard<std::mutex> lk(g_inMx);
        g_capRect = r;
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        g_deskRect = { vx, vy, vx + GetSystemMetrics(SM_CXVIRTUALSCREEN), vy + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        g_virt = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        GetCursorPos(&g_lastPt);
        g_inQ.clear();
        g_inQ.push_back({ 0, (float)g_virt.x, (float)g_virt.y, 0, false });
    }
    memset(g_keys, 0, sizeof g_keys);
    g_capture = true;
    if (!g_hookThread) g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);
}
static void StopCapture() {
    g_capture = false;
    if (g_hookThread) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 1000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr; g_hookThreadId = 0;
    }
    std::lock_guard<std::mutex> lk(g_inMx);
    g_inQ.clear();
}
// Captured input goes to whichever window the editor's cursor is over: the editor window or the game layer.
// A press is followed to its release in the same window (so drags work), and the keyboard goes to the window
// that was clicked last.
enum { RT_NONE, RT_GAME, RT_EDITOR };
// Unreal-style flying: hold the right mouse button on the game, look with the mouse, move with WASD / Q / E
static bool g_flyRMB = false;
static POINT g_flyAnchor{ 0, 0 }, g_flyLast{ 0, 0 };
static double g_flyYawTot = 0, g_flyPitchTot = 0;
static std::mutex g_flyMx;   // guards g_flyYawTot / g_flyPitchTot (mouse on the main thread, sticks on the fly thread)
static std::vector<InEvt> g_evGame, g_evEd;
static int g_route = RT_NONE, g_kbRoute = RT_GAME, g_btnOwner[3] = { RT_NONE, RT_NONE, RT_NONE };
static bool EdClientRect(RECT& r) {
    if (!g_edWnd || !IsWindowVisible(g_edWnd) || IsIconic(g_edWnd)) return false;
    RECT c; GetClientRect(g_edWnd, &c);
    POINT tl{ 0, 0 }; ClientToScreen(g_edWnd, &tl);
    r = { tl.x, tl.y, tl.x + c.right, tl.y + c.bottom };
    return true;
}
static bool InRect(const RECT& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static void DispatchCapturedInput() {
    std::vector<InEvt> q;
    { std::lock_guard<std::mutex> lk(g_inMx); q.swap(g_inQ); }
    RECT game = g_capRect, ed{ 0, 0, 0, 0 };
    bool haveEd = EdClientRect(ed);
    auto to = [&](int rt) -> std::vector<InEvt>* { return rt == RT_GAME ? &g_evGame : rt == RT_EDITOR ? &g_evEd : nullptr; };
    auto local = [&](int rt, InEvt e) { const RECT& o = rt == RT_EDITOR ? ed : game; e.x -= o.left; e.y -= o.top; return e; };
    static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    for (auto& e : q) {
        if (g_flyRMB) {                                   // flying: the mouse turns the camera, nothing else sees it
            if (e.type == 0) {
                { std::lock_guard<std::mutex> lk(g_flyMx); g_flyYawTot += (e.x - g_flyLast.x) * 0.15; g_flyPitchTot -= (e.y - g_flyLast.y) * 0.15; }
                g_flyLast = { (LONG)e.x, (LONG)e.y };
                continue;
            }
            if (e.type == 1 && e.a == 1 && !e.down) { g_flyRMB = false; g_btnOwner[1] = RT_NONE; continue; }
            if (e.type == 2) { g_flySpeed = std::clamp(g_flySpeed * (e.y > 0 ? 1.25f : 0.8f), 0.05f, 30.f); continue; }   // wheel = speed, like Unreal
            if (e.type == 1) continue;
            if (e.type == 3 || e.type == 4) continue;     // WASD etc. are read from the key states
        }
        if (e.type == 1 && e.a == 1 && e.down && g_route == RT_GAME && g_edFreecam) {
            g_flyRMB = true; g_btnOwner[1] = RT_GAME;
            { std::lock_guard<std::mutex> lk(g_inMx); g_flyAnchor = g_virt; }
            g_flyLast = g_flyAnchor;
            continue;
        }
        if (e.type == 0) {
            int held = RT_NONE; for (int b = 0; b < 3; b++) if (g_btnOwner[b] != RT_NONE) held = g_btnOwner[b];
            int over = held != RT_NONE ? held : (haveEd && InRect(ed, e.x, e.y)) ? RT_EDITOR : InRect(game, e.x, e.y) ? RT_GAME : RT_NONE;
            if (over != g_route && g_route != RT_NONE) to(g_route)->push_back({ 0, -FLT_MAX, -FLT_MAX, 0, false });   // left that window
            g_route = over;
            if (auto* v = to(over)) v->push_back(local(over, e));
        } else if (e.type == 1) {
            int b = std::clamp(e.a, 0, 2);
            int rt = e.down ? g_route : g_btnOwner[b];
            if (e.down) { g_btnOwner[b] = rt; if (rt != RT_NONE) g_kbRoute = rt; }
            else g_btnOwner[b] = RT_NONE;
            if (auto* v = to(rt)) v->push_back(e);
        } else if (e.type == 2) { if (auto* v = to(g_route)) v->push_back(e); }
        else { if (auto* v = to(g_kbRoute)) v->push_back(e); }
    }
    // keep the editor's cursor where it was while flying (Windows; Wine can't hold the pointer still)
    if (g_flyRMB && !wine) { std::lock_guard<std::mutex> lk(g_inMx); g_virt = g_flyAnchor; g_flyLast = g_flyAnchor; }
}

// ---- flying the editing camera: controller (XInput) + right mouse / WASD, sent to the game every frame
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } PadState;
typedef struct { DWORD dwPacketNumber; PadState Gamepad; } PadPacket;
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, PadPacket*);
static XInputGetStateFn g_xget = nullptr;
static bool g_padSeen = false;
static WORD g_padPrev = 0;
// stick value -1..1 past the dead zone, with a gentle curve so small pushes give fine, slow control
static float Stick(SHORT v, SHORT dz) {
    if (v > -dz && v < dz) return 0;
    float f = std::clamp((v > 0 ? v - dz : v + dz) / (32767.f - dz), -1.f, 1.f);
    return f * (0.35f + 0.65f * std::fabs(f));
}
static void LoadXInput() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    for (auto n : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
        if (HMODULE h = LoadLibraryW(n)) { g_xget = (XInputGetStateFn)GetProcAddress(h, "XInputGetState"); if (g_xget) break; }
}
static bool ReadPad(PadPacket& st) {
    if (!g_xget) return false;
    for (DWORD i = 0; i < 4; i++) if (g_xget(i, &st) == 0) return true;
    return false;
}

// The flying input is sent from its own thread ~120 times a second, so it's smooth no matter how fast
// the editor windows happen to draw. (Buttons are handled on the main thread in FlyTick.)
static DWORD WINAPI FlyThread(LPVOID) {
    timeBeginPeriod(1);   // 1 ms timer precision so the 8 ms pace is steady
    LoadXInput();
    LARGE_INTEGER freq, last{}; QueryPerformanceFrequency(&freq);
    unsigned seq = 0;
    std::wstring path;
    for (;;) {
        if (!g_ovVisible || !g_edFreecam) { last.QuadPart = 0; Sleep(30); continue; }
        if (path.empty()) path = BridgeDir() + L"\\fly.txt";
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        float dt = last.QuadPart ? std::min(0.1f, (float)((q.QuadPart - last.QuadPart) / (double)freq.QuadPart)) : 0.008f; last = q;
        float f = 0, r = 0, u = 0, boost = 1;
        PadPacket st{};
        if (ReadPad(st)) {
            const PadState& g = st.Gamepad;
            f += Stick(g.sThumbLY, 7849); r += Stick(g.sThumbLX, 7849);
            u += (g.bRightTrigger - g.bLeftTrigger) / 255.f;
            std::lock_guard<std::mutex> lk(g_flyMx);
            g_flyYawTot += Stick(g.sThumbRX, 8689) * 150.0 * dt;
            g_flyPitchTot += Stick(g.sThumbRY, 8689) * 110.0 * dt;
            if (g.wButtons & 0x0040) boost = 3;                                   // left stick click: faster
        }
        if (g_flyRMB) {                                                           // right mouse + WASD / Q / E
            auto k = [](int vk) { return (g_keys[vk] & 0x80) != 0; };
            f += (k('W') ? 1.f : 0.f) - (k('S') ? 1.f : 0.f);
            r += (k('D') ? 1.f : 0.f) - (k('A') ? 1.f : 0.f);
            u += (k('E') ? 1.f : 0.f) - (k('Q') ? 1.f : 0.f);
            if (k(VK_SHIFT)) boost = 3;
        }
        f = std::clamp(f, -1.f, 1.f); r = std::clamp(r, -1.f, 1.f); u = std::clamp(u, -1.f, 1.f);
        double yaw, pitch; { std::lock_guard<std::mutex> lk(g_flyMx); yaw = g_flyYawTot; pitch = g_flyPitchTot; }
        char line[160];
        snprintf(line, sizeof line, "%u %.4f %.4f %.3f %.3f %.3f %.3f\n", ++seq, yaw, pitch, f, r, u, g_flySpeed * boost);
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(h, line, (DWORD)strlen(line), &wr, nullptr); CloseHandle(h); }
        Sleep(8);
    }
    return 0;
}

// controller buttons (main thread: they change editor state)
static void FlyTick() {
    if (!g_edFreecam) return;
    LoadXInput();
    static HANDLE th = nullptr;
    if (!th) th = CreateThread(nullptr, 0, FlyThread, nullptr, 0, nullptr);
    ULONGLONG now = GetTickCount64();
    PadPacket st{};
    if (!ReadPad(st)) return;
    const PadState& g = st.Gamepad;
    if (!g_padSeen) { g_padSeen = true; g_toasts.push_back({ "Controller found: left stick moves, right stick looks, triggers go down / up", false, now }); }
    WORD pressed = g.wButtons & ~g_padPrev;
    if (pressed & 0x0200) { g_flySpeed = std::min(30.f, g_flySpeed * 1.5f); char b[48]; snprintf(b, sizeof b, "Camera speed %.2fx", g_flySpeed); g_toasts.push_back({ b, false, now }); SaveSettings(); }   // RB
    if (pressed & 0x0100) { g_flySpeed = std::max(0.05f, g_flySpeed / 1.5f); char b[48]; snprintf(b, sizeof b, "Camera speed %.2fx", g_flySpeed); g_toasts.push_back({ b, false, now }); SaveSettings(); } // LB
    if (pressed & 0x1000) { Ed("ed_pick"); g_edSelectTab = 2; }                                        // A: select what's in the middle
    if (pressed & 0x2000) Ed("ed_deselect");                                                           // B: deselect
    if (pressed & 0x8000) { g_gzMode = g_gzMode >= GZ_SCALE ? GZ_MOVE : g_gzMode + 1; SaveSettings(); }  // Y: next tool
    g_padPrev = g.wButtons;
}
// Feed one window's share of the captured input into the current ImGui context.
static void FeedEvents(std::vector<InEvt>& q) {
    ImGuiIO& io = ImGui::GetIO();
    for (auto& e : q) {
        switch (e.type) {
        case 0: io.AddMousePosEvent(e.x, e.y); break;
        case 1: io.AddMouseButtonEvent(e.a, e.down); break;
        case 2: io.AddMouseWheelEvent(e.x, e.y); break;
        case 3:
            io.AddKeyEvent(ImGuiMod_Ctrl, (g_keys[VK_CONTROL] & 0x80) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (g_keys[VK_SHIFT] & 0x80) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (g_keys[VK_MENU] & 0x80) != 0);
            if (e.a != ImGuiKey_None) io.AddKeyEvent((ImGuiKey)e.a, e.down);
            break;
        case 4: io.AddInputCharacterUTF16((ImWchar16)e.a); break;
        }
    }
    q.clear();
}

// ---- the editor's cursor (a tiny click-through window that follows it across the game, the editor and the desktop)
static LRESULT WINAPI CurWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}
// Draw a system cursor into the window with smooth edges: render it on black and on white, and work out
// each pixel's transparency from the difference.
static void PaintCursor(HCURSOR c) {
    const int N = 32;
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), N, -N, 1, 32, BI_RGB };
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    void *pb = nullptr, *pw = nullptr, *po = nullptr;
    HBITMAP bb = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pb, nullptr, 0);
    HBITMAP bw = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pw, nullptr, 0);
    HBITMAP bo = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &po, nullptr, 0);
    if (bb && bw && bo) {
        memset(pb, 0, N * N * 4); memset(pw, 0xFF, N * N * 4);
        HGDIOBJ old = SelectObject(dc, bb); DrawIconEx(dc, 0, 0, c, N, N, 0, nullptr, DI_NORMAL);
        SelectObject(dc, bw); DrawIconEx(dc, 0, 0, c, N, N, 0, nullptr, DI_NORMAL);
        auto* B = (unsigned char*)pb; auto* W = (unsigned char*)pw; auto* O = (unsigned char*)po;
        for (int i = 0; i < N * N * 4; i += 4) {
            int a = 255 - std::clamp(((W[i] - B[i]) + (W[i + 1] - B[i + 1]) + (W[i + 2] - B[i + 2])) / 3, 0, 255);
            O[i] = std::min<int>(B[i], a); O[i + 1] = std::min<int>(B[i + 1], a); O[i + 2] = std::min<int>(B[i + 2], a); O[i + 3] = (unsigned char)a;   // premultiplied
        }
        SelectObject(dc, bo);
        POINT src{ 0, 0 }; SIZE sz{ N, N }; BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_curWnd, screen, nullptr, &sz, dc, &src, 0, &bf, ULW_ALPHA);
        SelectObject(dc, old);
    }
    if (bb) DeleteObject(bb); if (bw) DeleteObject(bw); if (bo) DeleteObject(bo);
    DeleteDC(dc); ReleaseDC(nullptr, screen);
}
static void UpdateCursorWindow(bool show, int imguiCursor) {
    if (!g_curWnd) return;
    if (!show) { if (IsWindowVisible(g_curWnd)) ShowWindow(g_curWnd, SW_HIDE); return; }
    int shape = 0;
    switch (imguiCursor) {
    case ImGuiMouseCursor_TextInput: shape = 1; break; case ImGuiMouseCursor_ResizeAll: shape = 2; break;
    case ImGuiMouseCursor_ResizeNS: shape = 3; break; case ImGuiMouseCursor_ResizeEW: shape = 4; break;
    case ImGuiMouseCursor_ResizeNESW: shape = 5; break; case ImGuiMouseCursor_ResizeNWSE: shape = 6; break;
    case ImGuiMouseCursor_Hand: shape = 7; break; case ImGuiMouseCursor_NotAllowed: shape = 8; break;
    }
    static const LPCWSTR ids[] = { IDC_ARROW, IDC_IBEAM, IDC_SIZEALL, IDC_SIZENS, IDC_SIZEWE, IDC_SIZENESW, IDC_SIZENWSE, IDC_HAND, IDC_NO };
    ICONINFO ii{}; POINT hot{ 0, 0 };
    if (GetIconInfo(LoadCursorW(nullptr, ids[shape]), &ii)) { hot = { (LONG)ii.xHotspot, (LONG)ii.yHotspot }; if (ii.hbmMask) DeleteObject(ii.hbmMask); if (ii.hbmColor) DeleteObject(ii.hbmColor); }
    POINT v; { std::lock_guard<std::mutex> lk(g_inMx); v = g_virt; }
    if (shape != g_curShape) { g_curShape = shape; PaintCursor(LoadCursorW(nullptr, ids[shape])); }
    SetWindowPos(g_curWnd, HWND_TOPMOST, v.x - hot.x, v.y - hot.y, 32, 32, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// ---- overlay window & rendering
static void OvCreateRT() {
    ID3D11Texture2D* back = nullptr;
    g_ovSwap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_ovRtv); back->Release(); }
}
static void OvCleanupRT() { if (g_ovRtv) { g_ovRtv->Release(); g_ovRtv = nullptr; } }

static LRESULT WINAPI OvWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // While capturing, mouse/keyboard/focus come only from the input hooks - ignore what the system sends this window.
    bool ignore = g_capture && ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE ||
                                (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_SETCURSOR);
    if (g_ovCtx && !ignore) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_ovCtx);
        LRESULT r = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        ImGui::SetCurrentContext(prev);
        if (r) return r;
    }
    switch (msg) {
    case WM_SIZE: if (wp != SIZE_MINIMIZED) { g_ovResizeW = LOWORD(lp); g_ovResizeH = HIWORD(lp); } return 0;
    case WM_CLOSE: return 0;                                   // only F2 / the close button hide it
    case WM_SYSCOMMAND: if ((wp & 0xfff0) == SC_KEYMENU) return 0; break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void SaveEdWindowRect();
static bool CaptureActive();
static LRESULT WINAPI EdWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // while the game has focus, this window's mouse and keyboard come from the input hooks instead
    bool ignore = CaptureActive() && ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE ||
                                      (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_SETCURSOR);
    if (g_edCtx && !ignore) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_edCtx);
        LRESULT r = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        ImGui::SetCurrentContext(prev);
        if (r) return r;
    }
    switch (msg) {
    case WM_SIZE: if (wp != SIZE_MINIMIZED) { g_edResizeW = LOWORD(lp); g_edResizeH = HIWORD(lp); } return 0;
    case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize = { 420, 400 }; return 0; }
    case WM_EXITSIZEMOVE: SaveEdWindowRect(); return 0;
    case WM_CLOSE: OvHide(); return 0;                         // closing the window closes the editor (F2 opens it again)
    case WM_SYSCOMMAND: if ((wp & 0xfff0) == SC_KEYMENU) return 0; break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ApplyOverlayStyle() {
    if (!g_ovCtx) return;
    ImGuiStyle st = ImGui::GetStyle();                         // main context's style (current)
    float s = S();
    st.WindowPadding = ImVec2(16 * s, 14 * s);
    st.WindowRounding = (g_rounding + 4) * s;
    st.WindowBorderSize = 1;
    st.WindowTitleAlign = ImVec2(0, 0.5f);
    st.Colors[ImGuiCol_WindowBg] = Alpha(P.card, 0.97f);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_TabHovered] = Alpha(P.accent, 0.20f);
    st.Colors[ImGuiCol_TabSelected] = Alpha(P.accent, 0.28f);
    st.Colors[ImGuiCol_TabSelectedOverline] = P.accent;
    st.Colors[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_TabDimmedSelected] = Alpha(P.accent, 0.18f);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ovCtx);
    ImGui::GetStyle() = st;
    if (g_edCtx) {
        st.WindowRounding = 0; st.WindowBorderSize = 0;
        st.Colors[ImGuiCol_WindowBg] = P.card;
        ImGui::SetCurrentContext(g_edCtx);
        ImGui::GetStyle() = st;
        if (g_edWnd) {   // dark or light title bar to match the theme
            BOOL dark = P.dark; DwmSetWindowAttribute(g_edWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
        }
    }
    ImGui::SetCurrentContext(prev);
}

static void OvBuildFonts() {
    if (!g_ovCtx) return;
    FontSet keep = SaveFonts();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ovCtx);
    BuildFonts();
    g_ovFonts = SaveFonts();
    if (g_edCtx) { ImGui::SetCurrentContext(g_edCtx); BuildFonts(); g_edFonts = SaveFonts(); }
    ImGui::SetCurrentContext(prev);
    UseFonts(keep);
}

static bool MakeSwapChain(HWND hwnd, IDXGISwapChain** out) {
    IDXGIDevice* dxgiDev = nullptr; IDXGIAdapter* adapter = nullptr; IDXGIFactory* factory = nullptr;
    if (FAILED(g_dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) return false;
    dxgiDev->GetAdapter(&adapter);
    if (adapter) adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    HRESULT hr = factory ? factory->CreateSwapChain(g_dev, &sd, out) : E_FAIL;
    if (factory) { factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER); factory->Release(); }
    if (adapter) adapter->Release();
    dxgiDev->Release();
    return SUCCEEDED(hr);
}
static ImGuiContext* MakeImGuiContext(HWND hwnd) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* c = ImGui::CreateContext();
    ImGui::SetCurrentContext(c);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    ImGui::SetCurrentContext(prev);
    return c;
}
static void EdCreateRT() {
    ID3D11Texture2D* back = nullptr;
    g_edSwap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_edRtv); back->Release(); }
}
static void EdCleanupRT() { if (g_edRtv) { g_edRtv->Release(); g_edRtv = nullptr; } }

static bool OvCreate(HINSTANCE inst) {
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, OvWndProc, 0, 0, inst, nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"UE4SSEditorOverlay", nullptr };
    RegisterClassExW(&wc);
    g_ovHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, wc.lpszClassName, L"UE4SS Editor", WS_POPUP,
                               0, 0, 800, 600, nullptr, nullptr, inst, nullptr);
    if (!g_ovHwnd) return false;
    SetLayeredWindowAttributes(g_ovHwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS mg{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_ovHwnd, &mg);               // lets transparent pixels show the game underneath

    if (!MakeSwapChain(g_ovHwnd, &g_ovSwap)) return false;
    OvCreateRT();
    g_ovCtx = MakeImGuiContext(g_ovHwnd);

    // the editor window: a normal window with a title bar and a taskbar button
    WNDCLASSEXW we{ sizeof(we), CS_CLASSDC, EdWndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"UE4SSEditorWindow", LoadIconW(inst, MAKEINTRESOURCEW(1)) };
    RegisterClassExW(&we);
    g_edWnd = CreateWindowExW(WS_EX_APPWINDOW, we.lpszClassName, L"UE4SS Editor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 660, 800, nullptr, nullptr, inst, nullptr);
    if (g_edWnd && MakeSwapChain(g_edWnd, &g_edSwap)) {
        EdCreateRT();
        g_edCtx = MakeImGuiContext(g_edWnd);
    }
    // the editor's cursor
    WNDCLASSEXW wcur{ sizeof(wcur), 0, CurWndProc, 0, 0, inst, nullptr, nullptr, nullptr, nullptr, L"UE4SSEditorCursor", nullptr };
    RegisterClassExW(&wcur);
    g_curWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wcur.lpszClassName, L"", WS_POPUP, 0, 0, 32, 32, nullptr, nullptr, inst, nullptr);
    OvBuildFonts();
    ApplyOverlayStyle();
    CreateThread(nullptr, 0, F2Thread, nullptr, 0, nullptr);
    return true;
}

static ULONGLONG g_ovShownAt = 0, g_ovLostFgAt = 0;
// ---- mouse-look block: a tiny helper DLL in the game drops raw mouse input while this event is set
static HANDLE g_editEv = nullptr; static DWORD g_editEvPid = 0, g_inputDllPid = 0; static bool g_editHeld = false;
static std::wstring ExtractInputDll() {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(805), (LPCWSTR)RT_RCDATA);
    if (!r) return L"";
    const void* data = LockResource(LoadResource(nullptr, r)); DWORD sz = SizeofResource(nullptr, r);
    std::wstring dir = BridgeDir(); CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\LauncherInput-" + std::to_wstring(sz) + L".dll";   // size in the name: a loaded old copy is never overwritten
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow == sz) return path;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    DWORD wr = 0; WriteFile(f, data, sz, &wr, nullptr); CloseHandle(f);
    return wr == sz ? path : L"";
}
static void LoadInputDll(DWORD pid) {
    if (!pid || g_inputDllPid == pid) return;
    g_inputDllPid = pid;
    std::thread([pid] {
        std::wstring dll = ExtractInputDll();
        if (dll.empty()) { Log(L"Editor: couldn't prepare the mouse-look blocker.", LOG_WARN); return; }
        HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
        if (!h) { Log(L"Editor: couldn't reach the game to block mouse-look (try running the launcher as administrator).", LOG_WARN); return; }
        size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
        LPVOID mem = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        bool ok = false;
        if (mem && WriteProcessMemory(h, mem, dll.c_str(), bytes, nullptr)) {
            auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
            HANDLE t = CreateRemoteThread(h, nullptr, 0, ll, mem, 0, nullptr);
            if (t) { if (WaitForSingleObject(t, 5000) == WAIT_OBJECT_0) { DWORD c = 0; GetExitCodeThread(t, &c); ok = c != 0; VirtualFreeEx(h, mem, 0, MEM_RELEASE); } CloseHandle(t); }
        }
        CloseHandle(h);
        if (!ok) Log(L"Editor: the game wouldn't load the mouse-look blocker, so the mouse may still turn the camera.", LOG_WARN);
    }).detach();
}
static void SetEditingFlag(bool on) {
    DWORD pid = g_gamePid.load();
    if (pid != g_editEvPid) {
        if (g_editEv) { if (g_editHeld) ReleaseMutex(g_editEv); CloseHandle(g_editEv); g_editEv = nullptr; g_editHeld = false; }
        g_editEvPid = pid;
        if (pid) g_editEv = CreateMutexW(nullptr, FALSE, (L"Local\\UE4SSLauncherEditing_" + std::to_wstring(pid)).c_str());
    }
    if (!g_editEv || on == g_editHeld) return;
    if (on) { DWORD r = WaitForSingleObject(g_editEv, 200); g_editHeld = (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED); }
    else { ReleaseMutex(g_editEv); g_editHeld = false; }
}

static void SetOverlayMode(bool capture) {
    // keep-running mode: the layer over the game never takes focus or clicks (the hooks feed it);
    // classic mode: it takes clicks normally
    LONG ex = GetWindowLongW(g_ovHwnd, GWL_EXSTYLE);
    if (capture) ex |= WS_EX_NOACTIVATE | WS_EX_TRANSPARENT; else ex &= ~(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT);
    SetWindowLongW(g_ovHwnd, GWL_EXSTYLE, ex);
}
// Put the editor window next to the game: where you last left it, else beside the game, else on
// another monitor, else over the right side of the game.
static void SaveEdWindowRect() {
    if (!g_edWnd || !IsWindowVisible(g_edWnd) || IsIconic(g_edWnd)) return;
    RECT r; GetWindowRect(g_edWnd, &r);
    g_edWinRect[0] = r.left; g_edWinRect[1] = r.top; g_edWinRect[2] = r.right - r.left; g_edWinRect[3] = r.bottom - r.top;
    SaveSettings();
}
static BOOL CALLBACK MonEnum(HMONITOR m, HDC, LPRECT, LPARAM lp) { ((std::vector<HMONITOR>*)lp)->push_back(m); return TRUE; }
static void PlaceEditorWindow() {
    if (g_edPlaced) return;
    g_edPlaced = true;
    RECT saved{ g_edWinRect[0], g_edWinRect[1], g_edWinRect[0] + g_edWinRect[2], g_edWinRect[1] + g_edWinRect[3] };
    if (g_edWinRect[2] > 200 && g_edWinRect[3] > 200 && MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        SetWindowPos(g_edWnd, nullptr, saved.left, saved.top, g_edWinRect[2], g_edWinRect[3], SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    RECT gw{ 0, 0, 1280, 720 }; if (g_gameHwnd) GetWindowRect(g_gameHwnd, &gw);
    HMONITOR gm = MonitorFromWindow(g_gameHwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(gm, &mi);
    RECT work = mi.rcWork;
    RECT want{ 0, 0, (LONG)Px(660), (LONG)Px(780) };
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
    int w = want.right - want.left, h = std::min<int>(want.bottom - want.top, work.bottom - work.top);
    int x, y = std::clamp<int>(gw.top, work.top, std::max<int>(work.top, work.bottom - h));
    if (gw.right + w <= work.right) x = gw.right;                         // right of the game
    else if (gw.left - w >= work.left) x = gw.left - w;                   // left of the game
    else {
        std::vector<HMONITOR> mons; EnumDisplayMonitors(nullptr, nullptr, MonEnum, (LPARAM)&mons);
        HMONITOR other = nullptr; for (auto m : mons) if (m != gm) { other = m; break; }
        if (other) {                                                      // a second monitor
            MONITORINFO o{ sizeof(o) }; GetMonitorInfoW(other, &o);
            x = o.rcWork.left + 40; y = o.rcWork.top + 40;
            h = std::min<int>(h, o.rcWork.bottom - o.rcWork.top - 80);
        } else { x = work.right - w - 24; y = work.top + 40; h = std::min<int>(h, work.bottom - work.top - 80); }   // over the game's right side
    }
    SetWindowPos(g_edWnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}
static int g_projMode = -1;          // -1 not checked yet, 0 = horizontal FOV, 1 = vertical FOV, 2 = doesn't match (use the game's numbers),
                                     // 3 = FOV given for a fixed screen shape (g_projRef, e.g. 4:3) and widened to the real one (High on Life)
static float g_projRef = 4.f / 3.f;
static int g_projFails = 0;
// ---- the game fitted into the viewport (Unreal's layout): the layer and its panels cover the game's area as it was
// when the editor opened (the "host"); the game window itself is moved and resized to the space between the panels,
// and put back when the editor closes. Works with windowed and borderless games (not exclusive fullscreen).
static RECT g_ueHost{ 0, 0, 0, 0 }, g_ueGameWin{ 0, 0, 0, 0 };
static bool g_ueFitted = false, g_ueFitBroken = false;
static ULONGLONG g_ueFitAt = 0, g_ueStableAt = 0;
static RECT g_ueFitWant{ 0, 0, 0, 0 }, g_ueLastTarget{ 0, 0, 0, 0 };
static int g_ueFitFails = 0;
static ImVec2 g_ueOvOrigin(0, 0);
static bool OvHostRect(RECT& out) {
    if (g_ueHost.right > g_ueHost.left && g_gameHwnd && IsWindow(g_gameHwnd) && !IsIconic(g_gameHwnd)) { out = g_ueHost; return true; }
    return GameClientRect(out);
}
static void UeUnfit() {
    if (g_ueFitted && g_gameHwnd && IsWindow(g_gameHwnd))
        SetWindowPos(g_gameHwnd, nullptr, g_ueGameWin.left, g_ueGameWin.top, g_ueGameWin.right - g_ueGameWin.left, g_ueGameWin.bottom - g_ueGameWin.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    g_ueFitted = false;
}
// once a frame: move / resize the game window to the viewport (after the panels stop changing)
static void UeFitStep(ImVec2 vpMin, ImVec2 vpMax) {
    if (!g_ueFit || g_ueFitBroken || !g_gameHwnd || !IsWindow(g_gameHwnd) || g_ueHost.right <= g_ueHost.left) { if (!g_ueFit && g_ueFitted) UeUnfit(); return; }
    RECT t{ g_ueHost.left + (LONG)(vpMin.x - g_ueOvOrigin.x), g_ueHost.top + (LONG)(vpMin.y - g_ueOvOrigin.y),
            g_ueHost.left + (LONG)(vpMax.x - g_ueOvOrigin.x), g_ueHost.top + (LONG)(vpMax.y - g_ueOvOrigin.y) };
    if (t.right - t.left < 160 || t.bottom - t.top < 120) return;
    ULONGLONG now = GetTickCount64();
    if (memcmp(&t, &g_ueLastTarget, sizeof t) != 0) { g_ueLastTarget = t; g_ueStableAt = now; return; }
    if (now - g_ueStableAt < 120 || (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;    // (while a panel is being dragged)
    RECT c; if (!GameClientRect(c)) return;
    if (abs(c.left - t.left) <= 1 && abs(c.top - t.top) <= 1 && abs(c.right - t.right) <= 1 && abs(c.bottom - t.bottom) <= 1) { g_ueFitFails = 0; return; }
    if (g_ueFitAt && now - g_ueFitAt < 1200) return;                                    // (give the game time to follow)
    if (g_ueFitAt && memcmp(&g_ueFitWant, &t, sizeof t) == 0 && ++g_ueFitFails >= 3) {
        g_ueFitBroken = true; UeUnfit();
        g_toasts.push_back({ "This game won't let its window be resized. Set it to Windowed or Borderless in its settings to fit it between the panels.", true, now });
        return;
    }
    RECT w = t;
    LONG st = GetWindowLongW(g_gameHwnd, GWL_STYLE), ex = GetWindowLongW(g_gameHwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&w, (DWORD)st, FALSE, (DWORD)ex);
    SetWindowPos(g_gameHwnd, nullptr, w.left, w.top, w.right - w.left, w.bottom - w.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    ClipCursor(nullptr);
    g_ueFitted = true; g_ueFitAt = now; g_ueFitWant = t;
}
static void OvShow() {
    g_projMode = -1; g_projFails = 0; g_modelsAsked = false; g_thumbDirSent = false;
    for (auto& kv : g_thumbs) if (kv.second.state == 1) kv.second.state = 0;   // ask again for any the game didn't finish
    if (!g_bridge.connected) { Log(L"The editor needs a game running with mods. Launch one from the Play page.", LOG_WARN); return; }
    g_gameHwnd = FindGameWindow(g_gamePid.load());
    RECT r;
    if (!GameClientRect(r)) { Log(L"Couldn't find the game's window. If the game is in exclusive fullscreen, switch it to borderless or windowed.", LOG_WARN); return; }
    g_ueHost = r; GetWindowRect(g_gameHwnd, &g_ueGameWin);
    g_ueFitted = false; g_ueFitBroken = false; g_ueFitFails = 0; g_ueFitAt = 0; g_ueLastTarget = RECT{ 0, 0, 0, 0 };
    SetOverlayMode(g_edNoPause);
    PlaceEditorWindow();
    g_route = RT_NONE; g_kbRoute = RT_GAME; g_btnOwner[0] = g_btnOwner[1] = g_btnOwner[2] = RT_NONE;
    if (g_edNoPause) {
        // the game keeps focus the whole time (so it doesn't pause); if the launcher had focus, give it back to the game
        if (!GameIsForeground()) SetForegroundWindow(g_gameHwnd);
        SetWindowPos(g_ovHwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        if (g_edWnd && UeWantEdWindow()) SetWindowPos(g_edWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        StartCapture(r);
    } else {
        // classic mode: the editor window takes focus like any window (some games pause when they lose focus)
        SetWindowPos(g_ovHwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        if (g_edWnd && UeWantEdWindow()) {
            SetWindowPos(g_edWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), me = GetCurrentThreadId();
            if (fgThread != me) AttachThreadInput(fgThread, me, TRUE);
            SetForegroundWindow(g_edWnd); SetFocus(g_edWnd);
            if (fgThread != me) AttachThreadInput(fgThread, me, FALSE);
        }
    }
    g_ovVisible = true;
    g_ovShownAt = GetTickCount64(); g_ovLostFgAt = 0;
    LoadInputDll(g_gamePid.load());
    SetEditingFlag(true);
    Ed("ed_open", g_edFreecam ? "fc=1" : "");
    if (!g_actorsRequested) { g_actorsRequested = true; Ed("ed_actors", ""); }
}
static void MatEdCloseFwd();
static void OvHide(bool refocusGame) {
    if (!g_ovVisible) return;
    MatEdCloseFwd();             // (puts the editor window back to its own size first)
    SaveEdWindowRect();
    g_ovVisible = false;
    UeUnfit(); g_ueHost = RECT{ 0, 0, 0, 0 };    // the game window back to how it was
    SetEditingFlag(false);
    StopCapture();
    ShowWindow(g_ovHwnd, SW_HIDE);
    if (g_edWnd) ShowWindow(g_edWnd, SW_HIDE);
    UpdateCursorWindow(false, 0);
    Ed("ed_close");
    if (refocusGame && g_gameHwnd && IsWindow(g_gameHwnd) && !GameIsForeground()) SetForegroundWindow(g_gameHwnd);
}

// keep the overlay glued to the game window
static void OvTrack() {
    RECT r;
    if (!OvHostRect(r) || !g_bridge.connected) { OvHide(false); return; }
    { std::lock_guard<std::mutex> lk(g_inMx); g_capRect = r; }
    // The axis layer belongs on top of the game: hide it while another app is in front. The editor window
    // stays above the game while you play/edit, and behaves like a normal window otherwise.
    HWND fg = GetForegroundWindow();
    bool front = GameIsForeground() || fg == g_ovHwnd || (g_edWnd && fg == g_edWnd);
    if (front != (IsWindowVisible(g_ovHwnd) != FALSE)) ShowWindow(g_ovHwnd, front ? SW_SHOWNOACTIVATE : SW_HIDE);
    static int edTop = -1;
    if (g_edWnd && edTop != (int)front) {
        SetWindowPos(g_edWnd, front ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        edTop = front;
    }
    if (!front) return;
    RECT cur; GetWindowRect(g_ovHwnd, &cur);
    if (cur.left != r.left || cur.top != r.top || cur.right != r.right || cur.bottom != r.bottom)
        SetWindowPos(g_ovHwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
}

// ---- our own projection: the game sends its camera every frame and we place the gizmo ourselves,
// so it stays glued to the object while the camera moves (instead of trailing behind it)
static EdCam g_cam, g_camPrev;
static ULONGLONG g_camArrive = 0, g_camPrevArrive = 0;
struct CamPose { float loc[3], rot[3], fov, aspect; };

// half the view's width and height at distance 1, for each way games measure their FOV
static void ProjHalf(const CamPose& c, int mode, float& hx, float& hy) {
    float t = tanf(std::clamp(c.fov, 1.f, 170.f) * 0.5f * 0.017453293f);
    if (mode == 1) { hy = t; hx = t * c.aspect; }
    else if (mode == 3) { hy = t / g_projRef; hx = hy * c.aspect; }
    else { hx = t; hy = t / c.aspect; }
}
static bool ProjectPose(const CamPose& c, int mode, const float p[3], float& sx, float& sy) {
    const float d = 0.017453293f;
    float cp = cosf(c.rot[0] * d), sp = sinf(c.rot[0] * d), cy = cosf(c.rot[1] * d), sy_ = sinf(c.rot[1] * d), cr = cosf(c.rot[2] * d), sr = sinf(c.rot[2] * d);
    float F[3] = { cp * cy, cp * sy_, sp };                                         // Unreal: X forward, Y right, Z up
    float R[3] = { sr * sp * cy - cr * sy_, sr * sp * sy_ + cr * cy, -sr * cp };
    float U[3] = { -(cr * sp * cy + sr * sy_), cy * sr - cr * sp * sy_, cr * cp };
    float r[3] = { p[0] - c.loc[0], p[1] - c.loc[1], p[2] - c.loc[2] };
    float x = r[0] * F[0] + r[1] * F[1] + r[2] * F[2];
    float y = r[0] * R[0] + r[1] * R[1] + r[2] * R[2];
    float z = r[0] * U[0] + r[1] * U[1] + r[2] * U[2];
    if (x < 1.f) return false;                                                       // behind the camera
    float hx, hy; ProjHalf(c, mode, hx, hy);
    sx = 0.5f + 0.5f * y / (x * hx); sy = 0.5f - 0.5f * z / (x * hy);
    return true;
}

// the camera now: the latest sample, carried forward by its current motion for the time since it arrived
static bool CurrentPose(CamPose& o) {
    ULONGLONG now = GetTickCount64();
    if (!g_cam.seq || now - g_camArrive > 500) return false;
    for (int i = 0; i < 3; i++) { o.loc[i] = g_cam.loc[i]; o.rot[i] = g_cam.rot[i]; }
    o.fov = g_cam.fov; o.aspect = (float)g_cam.w / (float)g_cam.h;
    if (g_camPrev.seq && g_camPrev.seq < g_cam.seq) {
        double dt = g_cam.t - g_camPrev.t;
        if (!(dt > 0.002 && dt < 0.25)) dt = (g_camArrive - g_camPrevArrive) / 1000.0;   // game clock unusable: use arrival times
        if (dt > 0.002 && dt < 0.25) {
            float ahead = std::min((now - g_camArrive) / 1000.f, 0.05f);
            for (int i = 0; i < 3; i++) {
                o.loc[i] += (float)((g_cam.loc[i] - g_camPrev.loc[i]) / dt) * ahead;
                float dr = g_cam.rot[i] - g_camPrev.rot[i];
                while (dr > 180) dr -= 360;
                while (dr < -180) dr += 360;
                o.rot[i] += (float)(dr / dt) * ahead;
            }
        }
    }
    return true;
}

// check our maths against where the game itself put the object, and learn which way it measures FOV
static void CheckProjection() {
    if (!g_ed.hasSelCam || !g_ed.hasScreen || !g_ed.hasCenter || g_ed.selcam[8] <= 0) return;
    CamPose c; for (int i = 0; i < 3; i++) { c.loc[i] = g_ed.selcam[i]; c.rot[i] = g_ed.selcam[3 + i]; }
    c.fov = g_ed.selcam[6]; c.aspect = g_ed.selcam[7] / g_ed.selcam[8];
    int match = -1;
    for (int mode = 0; mode < 2 && match < 0; mode++) {
        float x, y;
        if (ProjectPose(c, mode, g_ed.scenter, x, y) && fabsf(x - g_ed.sscreen[0]) < 0.006f && fabsf(y - g_ed.sscreen[1]) < 0.006f) match = mode;
    }
    if (match < 0) {
        // the FOV may be for a fixed screen shape (4:3, 16:9...) and widened to the real one: work out which shape
        // from where the game put the object, using whichever direction it's furthest off the middle
        const float d = 0.017453293f;
        float cp = cosf(c.rot[0] * d), sp = sinf(c.rot[0] * d), cy = cosf(c.rot[1] * d), sy_ = sinf(c.rot[1] * d), cr = cosf(c.rot[2] * d), sr = sinf(c.rot[2] * d);
        float F[3] = { cp * cy, cp * sy_, sp }, R[3] = { sr * sp * cy - cr * sy_, sr * sp * sy_ + cr * cy, -sr * cp }, U[3] = { -(cr * sp * cy + sr * sy_), cy * sr - cr * sp * sy_, cr * cp };
        float r[3] = { g_ed.scenter[0] - c.loc[0], g_ed.scenter[1] - c.loc[1], g_ed.scenter[2] - c.loc[2] };
        float x = r[0] * F[0] + r[1] * F[1] + r[2] * F[2], y = r[0] * R[0] + r[1] * R[1] + r[2] * R[2], z = r[0] * U[0] + r[1] * U[1] + r[2] * U[2];
        float ox = g_ed.sscreen[0] - 0.5f, oy = g_ed.sscreen[1] - 0.5f, hy = 0;
        if (x > 1.f) {
            if (fabsf(ox) * c.aspect >= fabsf(oy) && fabsf(ox) > 0.03f) hy = (y / x) / (2 * ox) / c.aspect;
            else if (fabsf(oy) > 0.03f) hy = -(z / x) / (2 * oy);
        }
        if (hy > 0.01f) {
            float t = tanf(std::clamp(c.fov, 1.f, 170.f) * 0.5f * d), ref = t / hy;
            for (float k : { 4.f / 3.f, 16.f / 9.f, 16.f / 10.f, 21.f / 9.f, 1.f }) if (fabsf(ref / k - 1) < 0.015f) ref = k;
            float keep = g_projRef; g_projRef = ref;
            float px, py;
            if (ref > 0.5f && ref < 4.f && ProjectPose(c, 3, g_ed.scenter, px, py) && fabsf(px - g_ed.sscreen[0]) < 0.006f && fabsf(py - g_ed.sscreen[1]) < 0.006f) {
                if (g_projMode != 3 || fabsf(keep - ref) > 0.01f) Log(L"Editor: this game sets its view angle for a " + std::to_wstring(ref).substr(0, 4) + L":1 screen and widens it - the gizmo and outline allow for that.", LOG_INFO);
                match = 3;
            } else g_projRef = keep;
        }
    }
    if (match >= 0) { g_projMode = match; g_projFails = 0; }
    else if (++g_projFails >= 4 && g_projMode != 2) { g_projMode = 2; Log(L"Editor: this game's camera doesn't match the standard projection, so the gizmo follows the game's own updates instead.", LOG_INFO); }
}

// ---- incoming data
static void OvPoll() {
    {
        EdCam n = g_cam;
        if (ReadEdCam(n)) { g_camPrev = g_cam; g_camPrevArrive = g_camArrive; g_cam = n; g_camArrive = GetTickCount64(); }
    }
    if (ReadEdState(g_ed)) {
        CheckProjection();
        if (g_ed.hasMove && g_defWalk < 0) { g_defWalk = g_ed.walk; g_defJump = g_ed.jump; g_defGravity = g_ed.gravity; }
        if (g_defFov < 0 && g_ed.fov > 1) g_defFov = g_ed.fov;
        for (auto& m : g_ed.msgs) if (m.id > g_lastMsgId) {
            g_lastMsgId = m.id;
            g_toasts.push_back({ m.text, m.err, GetTickCount64() });
            Log(U2W("Editor: " + m.text), m.err ? LOG_WARN : LOG_INFO);
        }
    }
    ReadEdProps(g_props, g_propsFor);
    ReadEdActors(g_actors, g_actorTotal);
    ReadEdModels(g_models, g_modelsTotal);
}

// ---- UI pieces
static void EdLabel(const char* t) { ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted(t); ImGui::PopStyleColor(); }
static ID3D11ShaderResourceView* MatThumbFor(const std::wstring& png);
#include "filebrowser.inc"
static bool SegButton(const char* label, bool active, float w) {
    return active ? AccentButton(label, ImVec2(w, 0)) : ImGui::Button(label, ImVec2(w, 0));
}

// A slider that shows the game's live value, but keeps the user's value while dragging.
static bool LiveSlider(const char* id, float live, float& out, float mn, float mx, const char* fmt, ImGuiSliderFlags fl = 0) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID key = ImGui::GetID(id);
    bool wasActive = st->GetBool(key, false);
    float v = wasActive ? st->GetFloat(key + 1, live) : live;
    bool changed = ImGui::SliderFloat(id, &v, mn, mx, fmt, fl);
    st->SetBool(key, ImGui::IsItemActive());
    st->SetFloat(key + 1, v);
    out = v;
    return changed;
}

static void TabPlayer() {
    if (g_ed.pawn.empty()) { EdLabel("No player character right now (menus or loading screens)."); return; }
    ImGui::PushFont(F_med); ImGui::Text(ICON_USER "  %s", g_ed.pawn.c_str()); ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, Px(2)));

    SectionLabel("Position");
    static float tp[3]; static bool tpEditing = false;
    if (!tpEditing) memcpy(tp, g_ed.loc, sizeof tp);
    ImGui::SetNextItemWidth(-Px(110));
    ImGui::DragFloat3("##tp", tp, 5.0f, 0, 0, "%.0f");
    tpEditing = ImGui::IsItemActive() || (ImGui::IsItemDeactivated() ? false : tpEditing);
    if (ImGui::IsItemDeactivatedAfterEdit()) tpEditing = true;      // keep the typed value until Teleport
    ImGui::SameLine();
    if (AccentButton("Teleport", ImVec2(-1, 0))) { Ed("ed_tp", F3s(tp)); tpEditing = false; }
    if (tpEditing && !ImGui::IsAnyItemActive()) { ImGui::SameLine(); }
    if (ImGui::SmallButton(ICON_COPY " Copy position")) {
        char b[96]; snprintf(b, sizeof b, "%.1f, %.1f, %.1f", g_ed.loc[0], g_ed.loc[1], g_ed.loc[2]); ImGui::SetClipboardText(b);
    }
    ImGui::SameLine();
    if (tpEditing && ImGui::SmallButton("Reset to current")) tpEditing = false;

    ImGui::Dummy(ImVec2(0, Px(4)));
    SectionLabel("Cheats");
    bool god = g_ed.god;
    if (ToggleSwitch("##god", &god)) Ed("ed_god", god ? "1" : "0");
    ImGui::SameLine(0, Px(10)); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("God mode (can't be damaged)");
    float w3 = (ImGui::GetContentRegionAvail().x - Px(16)) / 3;
    int mode = g_ed.ghost ? 2 : (g_ed.mode == 5 ? 1 : 0);
    ImGui::BeginDisabled(!g_ed.hasMove);
    if (SegButton(ICON_PERSON " Walk", mode == 0, w3)) Ed("ed_move", "walk");
    ImGui::SameLine(0, Px(8));
    if (SegButton(ICON_DOVE " Fly", mode == 1, w3)) Ed("ed_move", "fly");
    ImGui::SameLine(0, Px(8));
    if (SegButton(ICON_GHOST " Ghost", mode == 2, w3)) Ed("ed_move", "ghost");
    ImGui::EndDisabled();
    if (!g_ed.hasMove) EdLabel("This game's player doesn't use Unreal's character movement.");

    ImGui::Dummy(ImVec2(0, Px(4)));
    SectionLabel("Movement & world");
    auto row = [&](const char* label, const char* id, float live, float def, float mn, float mx, const char* fmt, ImGuiSliderFlags fl, const char* verb, const char* key) {
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label);
        ImGui::SameLine(Px(120));
        ImGui::SetNextItemWidth(-Px(40));
        float v;
        if (LiveSlider(id, live, v, mn, mx, fmt, fl)) {
            char b[64]; snprintf(b, sizeof b, "%.3f", v);
            Ed(verb, key ? std::string(key) + "\t" + b : std::string(b));
        }
        ImGui::SameLine();
        ImGui::PushID(id);
        ImGui::BeginDisabled(def < 0 || std::fabs(live - def) < 0.001f);
        if (ImGui::Button(ICON_ROTATE, ImVec2(-1, 0))) { char b[64]; snprintf(b, sizeof b, "%.3f", def); Ed(verb, key ? std::string(key) + "\t" + b : std::string(b)); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Reset");
        ImGui::PopID();
    };
    ImGui::BeginDisabled(!g_ed.hasMove);
    row("Walk speed", "##walk", g_ed.walk, g_defWalk, 10, 10000, "%.0f", ImGuiSliderFlags_Logarithmic, "ed_pset", "walk");
    row("Jump height", "##jump", g_ed.jump, g_defJump, 0, 5000, "%.0f", 0, "ed_pset", "jump");
    row("Gravity", "##grav", g_ed.gravity, g_defGravity, 0, 4, "%.2fx", 0, "ed_pset", "gravity");
    ImGui::EndDisabled();
    row("Game speed", "##time", g_ed.timescale, 1.0f, 0.05f, 5, "%.2fx", ImGuiSliderFlags_Logarithmic, "ed_timescale", nullptr);
}

// ---- sharing saved layouts: a .uelayout file holds what you changed in one level (or every level) of a game,
// so someone else can load it into their copy of the game.
static std::string LaySafe(const std::string& s) {
    std::string o = s;
    for (auto& ch : o) if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '_')) ch = '_';
    return o;
}
static std::wstring LayDir() { return ModsDir() + L"\\LauncherBridge\\"; }
static bool LayRead(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear(); char b[65536]; DWORD n; while (ReadFile(h, b, sizeof b, &n, nullptr) && n) out.append(b, n);
    CloseHandle(h); return true;
}
static bool LayWrite(const std::wstring& p, const std::string& d) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = WriteFile(h, d.data(), (DWORD)d.size(), &w, nullptr) && w == d.size(); CloseHandle(h); return ok;
}
static std::vector<std::string> LayLines(const std::string& t) {
    std::vector<std::string> out; size_t s = 0;
    while (s < t.size()) { size_t e = t.find('\n', s); std::string l = t.substr(s, e == std::string::npos ? std::string::npos : e - s);
        if (!l.empty() && l.back() == '\r') l.pop_back(); out.push_back(l); if (e == std::string::npos) break; s = e + 1; }
    return out;
}
static void LayToast(const std::string& t, bool err) { g_toasts.push_back({ t, err, GetTickCount64() }); Log(U2W(t), err ? LOG_WARN : LOG_OK); }
// all saved levels for a game: (level path, change lines)
static std::vector<std::pair<std::string, std::vector<std::string>>> LayCollect(const std::string& game, const std::string& onlyMap) {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    std::wstring prefix = L"layout_" + U2W(game) + L"__";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((LayDir() + prefix + L"*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string text; if (!LayRead(LayDir() + fd.cFileName, text)) continue;
        auto lines = LayLines(text);
        std::string map;
        if (!lines.empty() && lines[0].rfind("# Saved editor layout - ", 0) == 0) { size_t sp = lines[0].find_last_of(' '); if (sp != std::string::npos) map = lines[0].substr(sp + 1); }
        if (map.empty() || map[0] != '/') continue;
        if (!onlyMap.empty() && LaySafe(map) != LaySafe(onlyMap)) continue;
        std::vector<std::string> ch; for (auto& l : lines) if (!l.empty() && l[0] != '#') ch.push_back(l);
        if (!ch.empty()) out.push_back({ map, ch });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
static void LayoutShare(bool allLevels) {
    std::string game = g_ed.layoutGame, map = g_ed.layoutMap;
    if (game.empty() || map.empty()) { LayToast("Start the game with mods and go into a level first.", true); return; }
    Ed("ed_layout", "save");    // make sure the latest changes are on disk (the save dialog gives it time)
    std::string gameName = W2U(GameDisplayName()); if (gameName.empty()) gameName = game;
    std::string lvl = map.substr(map.find_last_of('/') + 1);
    std::wstring def = U2W(gameName + (allLevels ? " - all levels" : " - " + lvl));
    for (auto& ch : def) if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'_';
    OvBrowse(L"Shared editor layout (*.uelayout)\0*.uelayout\0", L"Save your edits to share", L"", true, def, L"uelayout", [allLevels, game, map, gameName, def](std::wstring f) {
        if (f.empty()) return;
        auto maps = LayCollect(game, allLevels ? "" : map);
        if (maps.empty()) { LayToast(allLevels ? "Nothing saved for this game yet." : "Nothing saved for this level yet: move, hide, delete or add something first.", true); return; }
        std::string out = "UE4SS-LAUNCHER-LAYOUT 1\ngame=" + game + "\ngamename=" + gameName + "\n";
        int n = 0;
        for (auto& m : maps) { out += "map=" + m.first + "\n"; for (auto& l : m.second) { out += l + "\n"; n++; } }
        if (!LayWrite(f, out)) { LayToast("Couldn't save the file.", true); return; }
        char b[256]; snprintf(b, sizeof b, "Saved %d change%s in %d level%s to %s. Send that file to anyone with this launcher and the same game.",
                              n, n == 1 ? "" : "s", (int)maps.size(), maps.size() == 1 ? "" : "s", W2U(NameOf(f)).c_str());
        LayToast(b, false);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + f + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    });
}
struct LayPack { std::string game, gameName; std::vector<std::pair<std::string, std::vector<std::string>>> maps; std::wstring file; };
static LayPack g_layPending;          // waiting for "load anyway" (made for another game)
// Which levels got a shared layout (so "Unload shared" can put back what was there before): imported_<game>.txt
static std::wstring LayManifest(const std::string& game) { return LayDir() + L"imported_" + U2W(game) + L".txt"; }
static bool LayHasImport(const std::string& game) { return !game.empty() && FileExists(LayManifest(game)); }
static void LayoutApply(const LayPack& p) {
    std::string game = !g_ed.layoutGame.empty() ? g_ed.layoutGame : p.game;   // load it into the game you're playing
    int levels = 0, changes = 0; bool here = false;
    std::string manifest; LayRead(LayManifest(game), manifest);
    for (auto& m : p.maps) {
        std::string body = "# Saved editor layout - " + game + " " + m.first + "\n";
        for (auto& l : m.second) body += l + "\n";
        changes += (int)m.second.size(); levels++;
        if (g_bridge.connected && !g_ed.layoutMap.empty() && LaySafe(m.first) == LaySafe(g_ed.layoutMap)) {
            // the level you're in: the game loads it straight away
            std::wstring tmp = BridgeDir() + L"\\import_layout.txt";
            if (LayWrite(tmp, body)) { Ed("ed_layout", "import\t" + W2U(tmp)); here = true; }   // (the game keeps a copy of what was there)
        } else {
            std::wstring dest = LayDir() + L"layout_" + U2W(game) + L"__" + U2W(LaySafe(m.first)) + L".txt";
            std::wstring bak = dest + L".before-import";
            if (!FileExists(bak)) { std::string old; if (FileExists(dest)) LayRead(dest, old); LayWrite(bak, old); }   // empty = nothing was saved there
            LayWrite(dest, body);
        }
        if (manifest.find("\n" + LaySafe(m.first) + "\n") == std::string::npos && manifest.rfind(LaySafe(m.first) + "\n", 0) != 0) manifest += LaySafe(m.first) + "\n";
    }
    LayWrite(LayManifest(game), manifest);
    char b[256]; snprintf(b, sizeof b, "Loaded %s: %d change%s in %d level%s.%s", W2U(NameOf(p.file)).c_str(), changes, changes == 1 ? "" : "s", levels, levels == 1 ? "" : "s",
                          here ? "" : " They're put in place when you go into those levels.");
    LayToast(b, false);
}
static void LayoutUnload() {
    std::string game = g_ed.layoutGame;
    std::string manifest;
    if (!LayRead(LayManifest(game), manifest)) { LayToast("No shared layout is loaded for this game.", true); return; }
    int levels = 0; bool here = false;
    for (auto& key : LayLines(manifest)) {
        if (key.empty()) continue;
        levels++;
        if (g_bridge.connected && !g_ed.layoutMap.empty() && key == LaySafe(g_ed.layoutMap)) { Ed("ed_layout", "unimport"); here = true; continue; }
        std::wstring dest = LayDir() + L"layout_" + U2W(game) + L"__" + U2W(key) + L".txt", bak = dest + L".before-import";
        std::string old;
        if (LayRead(bak, old)) {
            if (!old.empty()) LayWrite(dest, old); else DeleteFileW(dest.c_str());
            DeleteFileW(bak.c_str());
        } else DeleteFileW(dest.c_str());
    }
    DeleteFileW(LayManifest(game).c_str());
    char b[200]; snprintf(b, sizeof b, "Unloaded the shared layout from %d level%s.%s", levels, levels == 1 ? "" : "s",
                          here ? "" : " Your own saved changes (if any) are back; reload a level you're in to see it.");
    LayToast(b, false);
}
static void LayoutLoad() {
    OvBrowse(L"Shared editor layout (*.uelayout)\0*.uelayout\0All files\0*.*\0", L"Load someone's shared edits", L"", false, L"", nullptr, [](std::wstring f) {
        if (f.empty()) return;
        std::string text; if (!LayRead(f, text)) { LayToast("Couldn't read that file.", true); return; }
        auto lines = LayLines(text);
        if (lines.empty() || lines[0].rfind("UE4SS-LAUNCHER-LAYOUT", 0) != 0) { LayToast("That isn't a shared editor layout (.uelayout) file.", true); return; }
        LayPack p; p.file = f;
        for (size_t i = 1; i < lines.size(); i++) {
            const std::string& l = lines[i];
            if (l.rfind("game=", 0) == 0) p.game = l.substr(5);
            else if (l.rfind("gamename=", 0) == 0) p.gameName = l.substr(9);
            else if (l.rfind("map=", 0) == 0) p.maps.push_back({ l.substr(4), {} });
            else if (!l.empty() && l[0] != '#' && !p.maps.empty() && l.size() > 2 && l[1] == '\t' && strchr("AHDNM", l[0])) p.maps.back().second.push_back(l);
        }
        p.maps.erase(std::remove_if(p.maps.begin(), p.maps.end(), [](auto& m) { return m.second.empty(); }), p.maps.end());
        if (p.maps.empty() || p.game.empty()) { LayToast("That layout file is empty.", true); return; }
        if (!g_ed.layoutGame.empty() && LaySafe(p.game) != g_ed.layoutGame) { g_layPending = p; return; }   // ask first
        LayoutApply(p);
    });
}
static void LayoutShareButtons(bool small) {
    bool can = !g_ed.layoutGame.empty() && !g_ed.layoutMap.empty();
    auto btn = [&](const char* l) { return small ? ImGui::SmallButton(l) : ImGui::Button(l); };
    ImGui::BeginDisabled(!can);
    if (btn(ICON_FILE_IMPORT " Share this level")) LayoutShare(false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(can ? "Save your edits to this level as a .uelayout file you can send to people" : "Start the game with mods and go into a level first");
    ImGui::SameLine();
    if (btn(ICON_FILE_IMPORT " Share all levels")) LayoutShare(true);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Save your edits to every level of this game in one file");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (btn(ICON_DOWNLOAD " Load shared")) LayoutLoad();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Load a .uelayout file someone sent you");
    ImGui::SameLine();
    bool loaded = LayHasImport(g_ed.layoutGame);
    ImGui::BeginDisabled(!loaded);
    if (btn(ICON_ERASER " Unload shared")) LayoutUnload();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(loaded ? "Take the shared layout off again: objects go back how they were,\nand your own saved changes come back" : "No shared layout is loaded for this game");
    if (!g_layPending.maps.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.warn); ImGui::PushTextWrapPos(0);
        ImGui::Text(ICON_WARN "  %s was made for %s, but you're playing %s. Objects may not match.", W2U(NameOf(g_layPending.file)).c_str(),
                    (g_layPending.gameName.empty() ? g_layPending.game : g_layPending.gameName).c_str(), g_ed.layoutGame.c_str());
        ImGui::PopTextWrapPos(); ImGui::PopStyleColor();
        if (btn("Load anyway")) { LayoutApply(g_layPending); g_layPending = LayPack(); }
        ImGui::SameLine(); if (btn("Cancel")) g_layPending = LayPack();
    }
}


// ---- imported models (.psk/.psa, glTF, OBJ -> .uemodel), built in the game out of procedural mesh parts
#include "models.h"
static std::vector<ModelInfo> g_imp; static bool g_impLoaded = false;
static std::mutex g_impMx; static std::vector<std::pair<std::string, bool>> g_impDone; static std::atomic<int> g_impBusy{ 0 };
static std::map<std::wstring, int> g_impAnim;
static void ImpWork(std::function<bool(std::string&)> job) {
    g_impBusy++;
    std::thread([job] {
        std::string msg; bool ok = false;
        try { ok = job(msg); } catch (...) { msg = "That file couldn't be read."; }
        if (ok) { std::string sm = ModelLastSimplified(); if (!sm.empty()) msg += " (It had too many triangles for the game, so it was " + sm + ".)"; }
        { std::lock_guard<std::mutex> lk(g_impMx); g_impDone.push_back({ msg, ok }); }
        g_impBusy--; g_framesLeft = 3;
    }).detach();
}
static void ImpPoll(); static std::wstring ImpStartDir();
// ---- materials library (colour + normal map from every exported model), with small pictures for the UI
static std::vector<MaterialInfo> g_mats;
struct MatThumb { int state = 0; ID3D11ShaderResourceView* srv = nullptr; std::vector<unsigned char> px; int w = 0, h = 0; };   // 0 new 1 loading 2 loaded 3 ready 4 failed
static std::map<std::wstring, MatThumb> g_matThumbs; static std::mutex g_matThumbMx;
static void MatReload() { g_mats = MaterialList(); }   // (pictures stay cached: they may still be drawn this frame)
static ID3D11ShaderResourceView* MatThumbFor(const std::wstring& png) {
    WIN32_FILE_ATTRIBUTE_DATA fa{}; GetFileAttributesExW(png.c_str(), GetFileExInfoStandard, &fa);
    std::wstring key = png + L"|" + std::to_wstring(((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);   // a new picture gets a new entry
    std::lock_guard<std::mutex> lk(g_matThumbMx);
    MatThumb& t = g_matThumbs[key];
    if (t.state == 0) {
        t.state = 1; MatThumb* tp = &t;
        std::thread([png, tp] {
            std::vector<unsigned char> px; int w = 0, h = 0;
            bool ok = MaterialPreview(png, px, w, h);
            std::lock_guard<std::mutex> lk2(g_matThumbMx);
            if (ok) { tp->px.swap(px); tp->w = w; tp->h = h; tp->state = 2; } else tp->state = 4;
            g_framesLeft = 3;
        }).detach();
    }
    if (t.state == 2) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = t.w; d.Height = t.h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{ t.px.data(), (UINT)t.w * 4, 0 };
        ID3D11Texture2D* tex = nullptr;
        if (g_dev && SUCCEEDED(g_dev->CreateTexture2D(&d, &sd, &tex))) { g_dev->CreateShaderResourceView(tex, nullptr, &t.srv); tex->Release(); }
        t.state = t.srv ? 3 : 4; t.px.clear(); t.px.shrink_to_fit();
    }
    return t.state == 3 ? t.srv : nullptr;
}
static void MatPicture(const std::wstring& png, float size) {
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size, a.y + size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ID3D11ShaderResourceView* srv = MatThumbFor(png)) dl->AddImageRounded((ImTextureID)srv, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, Px(4));
    else dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(P.dim.x, P.dim.y, P.dim.z, 0.25f)), Px(4));
    ImGui::Dummy(ImVec2(size, size));
}
static const MaterialInfo* MatByFolder(const std::string& folder) {
    for (auto& m : g_mats) if (W2U(m.dir.substr(m.dir.find_last_of(L'\\') + 1)) == folder) return &m;
    return nullptr;
}
static void MatSetSlot(const std::wstring& model, int slot, const std::wstring& dir) {
    std::string msg; bool ok = ModelSetSlotMaterial(model, slot, dir, msg);
    g_toasts.push_back({ msg, !ok, GetTickCount64() });
    if (ok && g_bridge.connected) Ed("ed_modelmats", W2U(model));
    g_impLoaded = false;
}
// one model's material slots, each with a picker of the library's materials
static void ModelSlotsUI(const ModelInfo& m) {
    if (m.slots.empty()) return;
    char hdr[96]; snprintf(hdr, sizeof hdr, ICON_PALETTE "  Materials (%d)##slots", (int)m.slots.size());
    ImGui::PushFont(F_small);
    if (ImGui::TreeNode(hdr)) {
        for (int i = 0; i < (int)m.slots.size(); i++) {
            ImGui::PushID(i);
            const std::string& use = i < (int)m.slotUse.size() ? m.slotUse[i] : std::string();
            const MaterialInfo* cur = use.empty() ? nullptr : MatByFolder(use);
            if (cur) { MatPicture(cur->colour, Px(22)); ImGui::SameLine(); }
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(m.slots[i].c_str()); ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(220));
            std::string preview = use.empty() ? "Its own material" : (cur ? cur->name : use + " (missing)");
            if (ImGui::BeginCombo("##use", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("Its own material", use.empty())) MatSetSlot(m.file, i, L"");
                for (auto& lm : g_mats) {
                    ImGui::PushID(W2U(lm.dir).c_str());
                    std::string folder = W2U(lm.dir.substr(lm.dir.find_last_of(L'\\') + 1));
                    MatPicture(lm.colour, Px(26)); ImGui::SameLine();
                    if (ImGui::Selectable(lm.name.c_str(), folder == use, 0, ImVec2(0, Px(26)))) MatSetSlot(m.file, i, lm.dir);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("From %s%s", lm.from.c_str(), lm.normal.empty() ? "" : "  \xc2\xb7  has a normal map");
                    ImGui::PopID();
                }
                if (g_mats.empty()) ImGui::TextColored(P.dim, "No materials yet: export a model, or add a picture in Materials.");
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::PopFont();
}
static void MaterialsUI(bool inGame) {
    ImpPoll();
    if (AccentButton(ICON_PLUS "  Add a picture as a material...")) OvBrowse(L"Images (*.png, *.tga, *.jpg)\0*.png;*.tga;*.jpg;*.jpeg\0", L"Pick the material's colour picture", ImpStartDir(), false, L"", nullptr, [](std::wstring f) {
        if (!f.empty()) ImpWork([f](std::string& msg) { return MaterialAdd(f, msg); });
    });
    ImGui::SameLine();
    if (ImGui::Button(ICON_FOLDER_OPEN "  Materials folder")) { MakeDirs(MaterialsDir()); OpenFolder(MaterialsDir()); }
    ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0); ImGui::PushFont(F_small);
    ImGui::TextUnformatted(g_mats.empty()
        ? "No materials yet. Every model you export puts its original textures (colour and normal map) here, ready to use on any imported model."
        : "Exported models put their original textures here. Pick one for any imported model under its Materials (Imported view).");
    ImGui::PopFont(); ImGui::PopTextWrapPos(); ImGui::PopStyleColor();
    if (g_mats.empty()) return;
    float cell = Px(118), avail = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)(avail / (cell + ImGui::GetStyle().ItemSpacing.x)));
    std::wstring toDelete;
    for (int i = 0; i < (int)g_mats.size(); i++) {
        auto& m = g_mats[i];
        if (i % cols) ImGui::SameLine();
        ImGui::PushID(W2U(m.dir).c_str());
        ImGui::BeginGroup();
        MatPicture(m.colour, cell);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nFrom %s%s", m.name.c_str(), m.from.c_str(), m.normal.empty() ? "\nNo normal map" : "\nWith normal map");
        if (ImGui::BeginPopupContextItem("matmenu")) {
            std::wstring dir = m.dir;
            if (ImGui::MenuItem(m.normal.empty() ? "Add a normal map..." : "Replace the normal map...")) OvBrowse(L"Images (*.png, *.tga, *.jpg)\0*.png;*.tga;*.jpg;*.jpeg\0", L"Pick the normal map", ImpStartDir(), false, L"", nullptr, [dir](std::wstring f) {
                if (!f.empty()) ImpWork([dir, f](std::string& msg) { return MaterialSetNormal(dir, f, msg); });
            });
            if (ImGui::MenuItem("Show in folder")) OpenFolder(m.dir);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_TRASH " Delete")) toDelete = m.dir;
            ImGui::EndPopup();
        }
        ImGui::PushFont(F_small);
        std::string label = m.name;
        while (label.size() > 1 && ImGui::CalcTextSize(label.c_str()).x > cell) label.pop_back();
        if (label != m.name && label.size() > 3) label = label.substr(0, label.size() - 3) + "...";
        ImGui::TextUnformatted(label.c_str());
        ImGui::TextColored(P.dim, "%s", m.normal.empty() ? "colour" : "colour + normal");
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::PushFont(F_small); ImGui::TextColored(P.dim, "Right-click a material to add a normal map or delete it."); ImGui::PopFont();
    if (!toDelete.empty()) { MaterialDelete(toDelete); g_impLoaded = false; }
    (void)inGame;
}
static void ImpPoll() {
    std::vector<std::pair<std::string, bool>> done;
    { std::lock_guard<std::mutex> lk(g_impMx); done.swap(g_impDone); }
    for (auto& d : done) { g_toasts.push_back({ d.first, !d.second, GetTickCount64() }); Log(U2W(d.first), d.second ? LOG_OK : LOG_WARN); g_impLoaded = false; }
    if (!g_impLoaded) { g_imp = ModelList(); g_impLoaded = true; MatReload(); }
}
static std::wstring ImpStartDir() { std::wstring e = FModelExports(); return DirExists(e) ? e + L"\\" : L""; }
static void ImpImportPick() {
    OvBrowse(L"3D models (*.fbx, *.psk, *.gltf, *.glb, *.obj)\0*.fbx;*.psk;*.pskx;*.gltf;*.glb;*.obj\0All files\0*.*\0", L"Import a model", ImpStartDir(), false, L"", nullptr, [](std::wstring f) {
        if (f.empty()) return;
        ImpWork([f](std::string& msg) { std::wstring out; return ModelImport(f, out, msg); });
    });
}
static void ExportSelectedButtonFwd(bool small);
static void ImportedModelsUI(bool inGame) {
    ImpPoll();
    bool conn = g_bridge.connected;
    if (AccentButton(ICON_FILE_IMPORT "  Import a model...")) ImpImportPick();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(".fbx (with its animations), .psk (with its .psa animations), .gltf / .glb or .obj\n(FModel saves models as .psk or .gltf, animations as .psa)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FOLDER_OPEN "  Models folder")) { MakeDirs(ModelsDir()); OpenFolder(ModelsDir()); }
    if (g_impBusy > 0) { ImGui::SameLine(); ImGui::TextColored(P.dim, ICON_HOURGLASS " Converting..."); }
    if (inGame) {
        ImGui::TextColored(P.dim, "Or take one out of this game:"); ImGui::SameLine();
        if (g_edSelMesh.empty()) ImGui::TextColored(P.dim, "select an object, then Export model (Inspector tab)");
        else { ImGui::TextUnformatted(g_edSelMesh.substr(g_edSelMesh.find_last_of('/') + 1).c_str()); ImGui::SameLine(); ExportSelectedButtonFwd(true); }
    }
    if (g_imp.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("No imported models yet. Save a model from any Unreal game with FModel (launcher Models page), then import it here.");
        ImGui::PopTextWrapPos(); ImGui::PopStyleColor();
        return;
    }
    if (!conn) ImGui::TextColored(P.dim, "Start a game with mods to add these to it.");
    std::wstring toDelete;
    for (auto& m : g_imp) {
        ImGui::PushID(W2U(m.file).c_str());
        ImGui::Separator();
        ImGui::PushFont(F_med); ImGui::TextUnformatted(m.name.c_str()); ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(P.dim, "%d part%s  \xc2\xb7  %d triangles  \xc2\xb7  %s%s", m.parts, m.parts == 1 ? "" : "s", m.tris,
                           m.anims.empty() ? "no animations" : (std::to_string(m.anims.size()) + " animation" + (m.anims.size() == 1 ? "" : "s")).c_str(),
                           m.textured ? "" : "  \xc2\xb7  plain colour");
        ImGui::BeginDisabled(!conn);
        if (ImGui::Button(ICON_PLUS " Add to level")) { Ed("ed_modelspawn", W2U(m.file)); if (inGame) g_edSelectTab = 2; }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(conn ? "Puts it where you're looking (then move it with the editor)" : "Start the game with mods first");
        if (!m.anims.empty()) {
            int& sel = g_impAnim[m.file];
            if (sel < 0 || sel >= (int)m.anims.size()) sel = 0;
            ImGui::SameLine(); ImGui::SetNextItemWidth(Px(180));
            if (ImGui::BeginCombo("##anim", m.anims[sel].c_str())) {
                for (int i = 0; i < (int)m.anims.size(); i++) if (ImGui::Selectable(m.anims[i].c_str(), i == sel)) sel = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine(); if (ImGui::Button(ICON_PLAY)) Ed("ed_modelanim", W2U(m.file) + "\t" + std::to_string(sel + 1));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Play on the selected copy (or the newest one)");
            ImGui::SameLine(); if (ImGui::Button("Stop")) Ed("ed_modelanim", W2U(m.file) + "\tstop");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("...")) ImGui::OpenPopup("more");
        if (ImGui::BeginPopup("more")) {
            std::wstring file = m.file;
            if (ImGui::MenuItem("Add an animation (.psa)...")) OvBrowse(L"Animation (*.psa)\0*.psa\0", L"Add an animation", ImpStartDir(), false, L"", nullptr, [file](std::wstring f) {
                if (!f.empty()) ImpWork([file, f](std::string& msg) { return ModelAddAnims(file, { f }, msg); });
            });
            if (ImGui::MenuItem("Use a different texture...")) OvBrowse(L"Images (*.png, *.tga, *.jpg)\0*.png;*.tga;*.jpg;*.jpeg\0", L"Pick the model's colour texture", ImpStartDir(), false, L"", nullptr, [file](std::wstring f) {
                if (!f.empty()) ImpWork([file, f](std::string& msg) { return ModelSetTexture(file, f, msg); });
            });
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_TRASH " Delete")) toDelete = file;
            ImGui::EndPopup();
        }
        ModelSlotsUI(m);
        ImGui::PopID();
    }
    if (!toDelete.empty()) { ModelDelete(toDelete); g_impLoaded = false; }
}

static void LayoutBar() {
    // saved layout for this level: what you change here comes back next time the level loads
    ImGui::PushFont(F_small);
    bool on = g_ed.layoutOn;
    if (ImGui::Checkbox("Save my changes in this level", &on)) Ed("ed_layout", on ? "on" : "off");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Objects you move, rotate, scale, hide, delete or add are saved\nand put back the next time this level loads.");
    ImGui::SameLine();
    ImGui::TextColored(P.dim, g_ed.layoutCount == 1 ? "(1 saved change)" : "(%d saved changes)", g_ed.layoutCount);
    if (g_ed.layoutCount > 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_ROTATE " Put back")) Ed("ed_layout", "reapply");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Apply the saved changes again (e.g. after part of the level reloaded)");
        ImGui::SameLine();
        static double armed = 0;
        bool sure = ImGui::GetTime() - armed < 3;
        if (ImGui::SmallButton(sure ? "Click again to clear" : ICON_TRASH " Clear")) { if (sure) { Ed("ed_layout", "reset"); armed = 0; } else armed = ImGui::GetTime(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget all saved changes for this level");
    }
    LayoutShareButtons(true);
    ImGui::PopFont();
}

static void TabWorld() {
    LayoutBar();
    ImGui::SetNextItemWidth(-Px(190));
    bool enter = ImGui::InputTextWithHint("##af", ICON_SEARCH "  Filter by name or class", g_actorFilter, sizeof g_actorFilter, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(ICON_ROTATE " Refresh", ImVec2(Px(84), 0)) || enter) Ed("ed_actors", g_actorFilter);
    ImGui::SameLine();
    if (ImGui::Button(ICON_CROSSHAIRS, ImVec2(-1, 0))) { Ed("ed_pick"); g_edSelectTab = 2; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select whatever is in the middle of the screen\n(or just click it in the game with the mouse)");
    ImGui::PushFont(F_small);
    EdLabel(g_actorTotal > (int)g_actors.size() ? "Nearest 500 shown - use the filter to narrow it down." :
            g_actors.empty() ? "Press Refresh to list the actors in the level." : "Sorted by distance from the camera. Click one to inspect it.");
    ImGui::PopFont();
    if (ImGui::BeginTable("actors", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Dist (m)", ImGuiTableColumnFlags_WidthFixed, Px(64));
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushFont(F_small); ImGui::TableHeadersRow(); ImGui::PopFont(); ImGui::PopStyleColor();
        ImGuiListClipper clip; clip.Begin((int)g_actors.size());
        while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            auto& a = g_actors[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::Selectable(a.name.c_str(), a.path == g_ed.sel, ImGuiSelectableFlags_SpanAllColumns)) { Ed("ed_select", a.path); g_edSelectTab = 2; }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted(a.cls.c_str());
            ImGui::TableNextColumn();
            if (a.dist >= 100000) ImGui::Text("%.0fk", a.dist / 1000); else ImGui::Text("%.0f", a.dist / 100);
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
}


static std::string ShortName(const std::string& path) {
    std::string n = path.substr(path.find_last_of('/') + 1);
    size_t d = n.find('.'); return d == std::string::npos ? n : n.substr(0, d);
}
static std::string FolderOf(const std::string& path) {
    size_t s = path.find_last_of('/'); return s == std::string::npos ? "" : path.substr(0, s);
}
static void StartPlacing(const std::string& path) {
    g_placePath = path; g_placeName = ShortName(path);
    g_toasts.push_back({ "Click in the game to place " + g_placeName + "  (Esc to cancel)", false, GetTickCount64() });
}
static void ExportModelsFromGame(const std::vector<std::pair<std::string, int>>& list);   // (path, 0 unknown / 1 object / 2 character)
static void ModelRow(int i, const std::string& path, const std::string& name, int flags = -1) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(i);
    {   // tick, for exporting several at once
        bool t = g_pakTicked.count(path) != 0;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(2), Px(1)));
        if (ImGui::Checkbox("##tick", &t)) { if (t) g_pakTicked.insert(path); else g_pakTicked.erase(path); }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tick models to export them together");
        ImGui::SameLine();
    }
    bool placing = g_placePath == path;
    if (ImGui::Selectable(name.c_str(), placing, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { Ed("ed_spawn", path); g_placePath.clear(); }
        else StartPlacing(path);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s\nClick, then click in the game to place it.\nDouble-click to drop it where you're looking.", path.c_str(), flags >= 0 && (flags & 1) ? "\n(character model)" : "");
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushFont(F_small);
    std::string f = FolderOf(path); ImGui::TextUnformatted(f.c_str());
    ImGui::PopFont(); ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    if (ImGui::SmallButton(ICON_PLUS)) { Ed("ed_spawn", path); g_placePath.clear(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add it where you're looking");
    ImGui::SameLine(0, Px(4));
    bool canEx = flags < 0 || (flags & 2);
    ImGui::BeginDisabled(!canEx || g_impBusy > 0);
    if (ImGui::SmallButton(ICON_DOWNLOAD)) ExportModelsFromGame({ { path, flags < 0 ? 0 : (flags & 1) ? 2 : 1 } });
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", !canEx ? "This is an Unreal 5 game: its models can be placed but not exported yet" :
                                g_impBusy > 0 ? "Exporting..." : "Export this model (with its textures and animations) into Models > Imported");
    ImGui::PopID();
}

// ---- model pictures: the game photographs each model (see editor.lua); we cache the PNGs per game

static std::wstring ThumbDir() {
    std::wstring g = GameDisplayName(), k;
    for (wchar_t c : g) k += (iswalnum(c) ? c : L'_');
    if (k.empty()) k = L"game";
    return BridgeDir() + L"\\thumbs2\\" + k + L"\\";   // thumbs2: Unreal-style pictures (older ones are ignored)
}
static std::string ThumbId(const std::string& path) {
    std::string p = path; size_t d = p.find('.', p.find_last_of('/') + 1); if (d != std::string::npos) p = p.substr(0, d);   // same id with or without .Name
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : p) { h ^= (unsigned char)tolower(c); h *= 1099511628211ull; }
    char b[20]; snprintf(b, sizeof b, "%016llx", h); return b;
}
// decode a PNG into RGBA pixels (Windows Imaging Component)
static bool LoadPng(const std::wstring& file, std::vector<unsigned char>& px, UINT& w, UINT& h) {
    static bool com = false; if (!com) { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); com = true; }
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(f->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(cv->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 2048 && h <= 2048) {
        px.resize((size_t)w * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()));
    }
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); f->Release();
    return ok;
}
// Pick the better of the two pictures, cut out the empty background and even out the brightness.
static ID3D11ShaderResourceView* MakeThumb(const std::wstring& lit, const std::wstring& flat) {
    std::vector<unsigned char> a, b; UINT aw = 0, ah = 0, bw = 0, bh = 0;
    bool haveA = LoadPng(lit, a, aw, ah), haveB = LoadPng(flat, b, bw, bh) && bw == aw && bh == ah;
    if (!haveA && !haveB) return nullptr;
    if (!haveA) { a = b; aw = bw; ah = bh; haveA = true; haveB = false; }
    size_t n = (size_t)aw * ah;
    auto lum = [](const unsigned char* p) { return (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.f; };
    auto isObj = [](const unsigned char* p) { return std::max(p[0], std::max(p[1], p[2])) > 5; };
    size_t covA = 0, covB = 0; double sumA = 0;
    for (size_t i = 0; i < n; i++) {
        if (isObj(&a[i * 4])) { covA++; sumA += lum(&a[i * 4]); }
        if (haveB && isObj(&b[i * 4])) covB++;
    }
    float meanA = covA ? (float)(sumA / covA) : 0;
    bool useFlat = haveB && (covA < covB / 2 || meanA < 0.05f || meanA > 0.93f);
    std::vector<unsigned char>& src = useFlat ? b : a;
    // brightness: bring the bright end of the model to ~90%
    std::vector<float> ls; ls.reserve(n);
    for (size_t i = 0; i < n; i++) if (isObj(&src[i * 4])) ls.push_back(lum(&src[i * 4]));
    float gain = 1;
    if (!ls.empty()) {
        size_t k = ls.size() * 95 / 100; std::nth_element(ls.begin(), ls.begin() + k, ls.end());
        if (ls[k] > 0.01f) gain = std::clamp(0.9f / ls[k], 0.6f, 4.f);
    }
    std::vector<unsigned char> out(n * 4);
    for (size_t i = 0; i < n; i++) {
        const unsigned char* p = &src[i * 4];
        int m = std::max((int)p[0], std::max((int)p[1], (int)p[2]));
        if (haveB) m = std::max(m, (int)std::max(b[i * 4], std::max(b[i * 4 + 1], b[i * 4 + 2])));
        for (int c = 0; c < 3; c++) out[i * 4 + c] = (unsigned char)std::min(255.f, p[c] * gain);
        out[i * 4 + 3] = (unsigned char)std::clamp(m * 40, 0, 255);   // empty background -> transparent
    }
    D3D11_TEXTURE2D_DESC d{}; d.Width = aw; d.Height = ah; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ out.data(), aw * 4, 0 };
    ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, &sd, &tex))) return nullptr;
    g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return srv;
}
static bool FileThere(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Called for each model card on screen: returns its picture once there is one.
static Thumb& ThumbFor(const std::string& path) {
    std::string id = ThumbId(path);
    Thumb& t = g_thumbs[id];
    if (t.state == 2 || t.state == 3) return t;
    ULONGLONG now = GetTickCount64();
    if (t.state == 1 && now - t.checkedAt < 250) return t;   // waiting on the game: look a few times a second
    t.checkedAt = now;
    static std::wstring dir; static ULONGLONG dirAt = 0;
    if (dir.empty() || now - dirAt > 2000) { dir = ThumbDir(); dirAt = now; }
    std::wstring base = dir + U2W(id);
    if (FileThere(base + L".fail")) { t.state = 3; return t; }
    bool lit = FileThere(base + L".png"), flat = FileThere(base + L"_b.png");
    // the plain-colour picture is saved second, so once it's there both are complete
    // (a lone lit picture is used if the second never arrives)
    if (lit && (flat || t.state == 0 || now - t.askedAt > 4000)) {
        if (ID3D11ShaderResourceView* srv = MakeThumb(base + L".png", base + L"_b.png")) { t.srv = srv; t.state = 2; return t; }
    }
    if (t.state == 0) { t.state = 1; t.askedAt = now; g_thumbAsk.push_back({ id, path }); }
    else if (t.state == 1 && now - t.askedAt > 30000) { t.state = 3; }   // the game never answered
    return t;
}
// send queued picture requests in small batches
static void ThumbFlush() {
    if (g_thumbAsk.empty() || GetTickCount64() - g_thumbSentAt < 300) return;
    std::wstring dir = ThumbDir();
    if (!g_thumbDirSent || g_thumbDirFor != dir) {
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        std::string d = W2U(dir); for (auto& c : d) if (c == '\\') c = '/';
        Ed("ed_thumbdir", d);
        g_thumbDirSent = true; g_thumbDirFor = dir;
    }
    std::string lines; int n = 0;   // one command line: entries "id\tpath" split by |
    while (!g_thumbAsk.empty() && n < 24) { lines += (n ? "|" : "") + g_thumbAsk.front().first + "\t" + g_thumbAsk.front().second; g_thumbAsk.erase(g_thumbAsk.begin()); n++; }
    Ed("ed_thumbs", lines);
    g_thumbSentAt = GetTickCount64();
}

// one card in the model grid
static void ModelCard(int i, const std::string& path, const std::string& name, float cw, int flags = -1) {
    ImGui::PushID(i);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float th = cw;                                   // square picture
    float lh = ImGui::GetTextLineHeight() + Px(6);
    bool placing = g_placePath == path;
    ImGui::InvisibleButton("##card", ImVec2(cw, th + lh));
    bool hov = ImGui::IsItemHovered();
    bool ticked = g_pakTicked.count(path) != 0;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (ImGui::GetIO().KeyCtrl) { if (ticked) g_pakTicked.erase(path); else g_pakTicked.insert(path); ticked = !ticked; }
        else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { Ed("ed_spawn", path); g_placePath.clear(); }
        else StartPlacing(path);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("cardmenu");
    if (hov) ImGui::SetTooltip("%s\nClick, then click in the game to place it.\nDouble-click to drop it where you're looking.\nCtrl+click to tick it, right-click to export.", path.c_str());
    if (ImGui::BeginPopup("cardmenu")) {
        bool canEx = flags < 0 || (flags & 2);
        ImGui::BeginDisabled(!canEx || g_impBusy > 0);
        if (ImGui::MenuItem(ICON_DOWNLOAD "  Export model")) ExportModelsFromGame({ { path, flags < 0 ? 0 : (flags & 1) ? 2 : 1 } });
        ImGui::EndDisabled();
        if (!canEx && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("This is an Unreal 5 game: its models can be placed but not exported yet");
        if (ImGui::MenuItem(ticked ? ICON_CIRCLE_XMARK "  Untick" : ICON_CHECK "  Tick for exporting together")) { if (ticked) g_pakTicked.erase(path); else g_pakTicked.insert(path); }
        if (ImGui::MenuItem(ICON_PLUS "  Add where I'm looking")) { Ed("ed_spawn", path); g_placePath.clear(); }
        ImGui::EndPopup();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bgc = ImGui::GetColorU32(placing ? Alpha(P.accent, 0.35f) : hov ? Alpha(P.text, 0.10f) : Alpha(P.text, 0.05f));
    dl->AddRectFilled(p0, ImVec2(p0.x + cw, p0.y + th + lh), bgc, Px(8));
    if (placing) dl->AddRect(p0, ImVec2(p0.x + cw, p0.y + th + lh), ImGui::GetColorU32(P.accent), Px(8), 0, Px(2));
    if (ticked) {
        dl->AddRect(p0, ImVec2(p0.x + cw, p0.y + th + lh), ImGui::GetColorU32(P.ok), Px(8), 0, Px(2));
        ImGui::PushFont(F_small); dl->AddText(ImVec2(p0.x + Px(6), p0.y + Px(4)), ImGui::GetColorU32(P.ok), ICON_CHECK); ImGui::PopFont();
    }
    Thumb& t = ThumbFor(path);
    float pad = Px(6);
    ImVec2 a(p0.x + pad, p0.y + pad), b(p0.x + cw - pad, p0.y + th - pad);
    if (t.state == 2 && t.srv) dl->AddImage((ImTextureID)t.srv, a, b);
    else {
        ImGui::PushFont(F_h2);
        const char* ic = t.state == 3 ? ICON_CUBE : ICON_HOURGLASS;
        ImVec2 is = ImGui::CalcTextSize(ic);
        dl->AddText(ImVec2((a.x + b.x - is.x) * 0.5f, (a.y + b.y - is.y) * 0.5f), ImGui::GetColorU32(Alpha(P.dim, t.state == 3 ? 0.5f : 0.8f)), ic);
        ImGui::PopFont();
    }
    // name, cut to fit
    ImGui::PushFont(F_small);
    std::string label = name; float maxw = cw - Px(10);
    if (ImGui::CalcTextSize(label.c_str()).x > maxw) {
        while (label.size() > 1 && ImGui::CalcTextSize((label + "...").c_str()).x > maxw) label.pop_back();
        label += "...";
    }
    ImVec2 ls = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(p0.x + (cw - ls.x) * 0.5f, p0.y + th + Px(1)), ImGui::GetColorU32(placing ? P.text : P.dim), label.c_str());
    ImGui::PopFont();
    ImGui::PopID();
}
// Export models straight from the game's files. kind 0 = not known (tries object, then character)
static void AesSyncNow();
static void ExportModelsFromGame(const std::vector<std::pair<std::string, int>>& list) {
    if (list.empty() || g_impBusy > 0) return;
    AesSyncNow();                                 // encrypted games: use the key saved for this game
    std::wstring paks = PaksDir();
    if (list.size() > 1) g_toasts.push_back({ "Exporting " + std::to_string(list.size()) + " models...", false, GetTickCount64() });
    ImpWork([list, paks](std::string& msg) {
        if (paks.empty()) { msg = "Choose your game on the Play page first."; return false; }
        int ok = 0; std::vector<std::string> bad; std::string lastMsg;
        for (auto& it : list) {
            std::string name = ShortName(it.first), m; std::wstring out; bool done = false;
            for (int kind : { 1, 2 }) {
                if (it.second && it.second != kind) continue;
                std::string m2;
                if (ModelExportParts(paks, name, { std::string(kind == 2 ? "skeletal" : "static") + "\t" + it.first }, "", out, m2)) { done = true; m = m2; break; }
                if (m.empty() || it.second) m = m2;
            }
            if (done) { ok++; lastMsg = m; } else bad.push_back(name + (m.empty() ? "" : " (" + m + ")"));
        }
        if (list.size() == 1) { msg = ok ? lastMsg + " It's in Models > Imported for this game." : bad[0]; return ok == 1; }
        msg = "Exported " + std::to_string(ok) + " of " + std::to_string(list.size()) + " models into Models > Imported.";
        if (!bad.empty()) { msg += " Couldn't export: "; for (size_t i = 0; i < bad.size() && i < 5; i++) msg += (i ? "; " : "") + bad[i]; if (bad.size() > 5) msg += "; and " + std::to_string(bad.size() - 5) + " more"; }
        return ok > 0;
    });
}
// Read every model in the game's files (in the background; the list fills in as it goes)
static std::atomic<int> g_pakGen{ 0 };   // bumped for every new read: an older read still running throws its results away
static void LoadGameModels() {
    AesSyncNow();
    std::wstring paks = PaksDir();
    int gen = ++g_pakGen;
    g_pakFor = paks; g_pakModelsState = 1; g_pakDone = 0; g_pakTotal = 0;
    { std::lock_guard<std::mutex> lk(g_pakModelsMx); g_pakModels.clear(); g_pakLower.clear(); g_pakFlags.clear(); g_pakModelsNote.clear(); }
    std::thread([paks, gen] {
        std::vector<GameMesh> v; std::string note;
        auto add = [gen](const GameMesh& m) {
            if (gen != g_pakGen) return;
            std::lock_guard<std::mutex> lk(g_pakModelsMx);
            g_pakModels.push_back(m.path); g_pakLower.push_back(W2U(Lower(U2W(m.path))));
            g_pakFlags.push_back((uint8_t)((m.skeletal ? 1 : 0) | (m.exportable ? 2 : 0)));
        };
        bool ok = false;
        try { ok = ModelListGameMeshes(paks, v, note, &g_pakDone, &g_pakTotal, add); } catch (...) { note = "the game's files couldn't be read"; }
        if (gen != g_pakGen) return;      // another game was chosen meanwhile
        std::lock_guard<std::mutex> lk(g_pakModelsMx);
        if (gen != g_pakGen) return;
        if (ok) {   // final list, in order
            g_pakModels.clear(); g_pakLower.clear(); g_pakFlags.clear();
            for (auto& m : v) { g_pakModels.push_back(m.path); g_pakLower.push_back(W2U(Lower(U2W(m.path)))); g_pakFlags.push_back((uint8_t)((m.skeletal ? 1 : 0) | (m.exportable ? 2 : 0))); }
        }
        g_pakModelsNote = note; g_pakModelsState = 2; g_framesLeft = 3;
    }).detach();
}
// Another game chosen (or connected): forget everything read from the old game's files, so the Content Browser,
// model lists and pictures show the new game without restarting the launcher.
static void CbResetForNewGame();
static void CheckGameSwitch() {
    static std::wstring last; static bool first = true;
    static ULONGLONG at = 0;
    ULONGLONG now = GetTickCount64();
    if (!first && now - at < 500) return;
    at = now;
    std::wstring cur = Lower(TargetExe());
    if (first) { first = false; last = cur; ModelSetGame(cur.empty() ? L"" : GameDisplayName()); g_impLoaded = false; return; }
    if (cur == last) return;
    last = cur;
    g_pakGen++;                                   // a read of the old game still running is thrown away
    { std::lock_guard<std::mutex> lk(g_pakModelsMx); g_pakModels.clear(); g_pakLower.clear(); g_pakFlags.clear(); g_pakModelsNote.clear(); }
    g_pakModelsState = 0; g_pakDone = 0; g_pakTotal = 0; g_pakFor.clear();
    for (auto& kv : g_thumbs) if (kv.second.srv) kv.second.srv->Release();
    g_thumbs.clear(); g_thumbAsk.clear(); g_thumbDirSent = false; g_thumbDirFor.clear();
    g_modelsAsked = false;
    ModelSetGame(cur.empty() ? L"" : GameDisplayName()); g_impLoaded = false;     // imported models are per game
    CbResetForNewGame();
    if (!cur.empty()) LoadGameModels();
    g_framesLeft = 3;
}
static void TabModels() {
    // source switch
    const char* srcs[] = { ICON_CUBE "  In this level", ICON_BOX_OPEN "  All game files", ICON_FILE_IMPORT "  Imported", ICON_PALETTE "  Materials" };
    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine();
        if (g_modelSrc == i ? AccentButton(srcs[i]) : ImGui::Button(srcs[i])) g_modelSrc = i;
    }
    if (g_modelSrc == 2) { ImGui::Dummy(ImVec2(0, Px(4))); ImportedModelsUI(true); return; }
    if (g_modelSrc == 3) { ImGui::Dummy(ImVec2(0, Px(4))); MaterialsUI(true); return; }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Px(72));
    if (ImGui::Button(g_modelPicsSetting ? ICON_LIST "##view" : ICON_CUBE "##view", ImVec2(Px(34), 0))) { g_modelPicsSetting = !g_modelPicsSetting; SaveSettings(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(g_modelPicsSetting ? "Show as a list" : "Show pictures");
    if (g_modelSrc == 0 && !g_modelsAsked) { g_modelsAsked = true; Ed("ed_models", g_modelFilter); }
    if (g_modelSrc == 1) {
        static ULONGLONG checked = 0;
        if (g_pakModelsState == 0) LoadGameModels();
        else if (g_pakModelsState == 2 && GetTickCount64() - checked > 3000) { checked = GetTickCount64(); if (Lower(PaksDir()) != Lower(g_pakFor)) LoadGameModels(); }   // another game
    }
    ImGui::SetNextItemWidth(g_modelSrc == 0 ? -Px(96) : -Px(250));
    bool enter = ImGui::InputTextWithHint("##mf", ICON_SEARCH "  Search models (e.g. chair, box, crate)", g_modelFilter, sizeof g_modelFilter, ImGuiInputTextFlags_EnterReturnsTrue);
    if (g_modelSrc == 0) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_ROTATE " Refresh", ImVec2(-1, 0)) || enter) Ed("ed_models", g_modelFilter);
    } else {
        const char* kinds[] = { "All", "Objects", "Characters" };
        for (int k = 0; k < 3; k++) { ImGui::SameLine(0, Px(4)); if (g_pakKind == k ? AccentButton(kinds[k]) : ImGui::Button(kinds[k])) g_pakKind = k; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Characters: models with a skeleton (people, animals, anything that animates)");
    }
    std::string f = W2U(Lower(U2W(g_modelFilter)));

    ImGui::PushFont(F_small);
    if (!g_placePath.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.accent);
        ImGui::TextWrapped("Click in the game to place %s. Hold Shift to place more than one. Esc cancels.", g_placeName.c_str());
        ImGui::PopStyleColor();
    } else if (g_modelSrc == 0)
        EdLabel(g_models.empty() ? "Models used in the level you're in. Press Refresh if this stays empty." :
                g_modelsTotal > (int)g_models.size() ? "First 4000 shown. Search to narrow it down." : "Click one, then click in the game to place it.");
    else if (g_pakModelsState == 1) {
        int d = g_pakDone, t = g_pakTotal; size_t n; { std::lock_guard<std::mutex> lk(g_pakModelsMx); n = g_pakModels.size(); }
        char b[200]; snprintf(b, sizeof b, "Reading the game's files: %d of %d checked, %d models found so far. Next time this is instant.", d, t, (int)n);
        EdLabel(b);
        ImGui::ProgressBar(t ? (float)d / t : 0.f, ImVec2(-1, Px(4)), "");
    } else {
        size_t n; { std::lock_guard<std::mutex> lk(g_pakModelsMx); n = g_pakModels.size(); }
        EdLabel(("Every model in the game's files (" + std::to_string(n) + "). Click to place, " ICON_DOWNLOAD " to export; tick several to export them together.").c_str());
    }
    ImGui::PopFont();

    // ticked models: export them together
    if (!g_pakTicked.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.ok); ImGui::Text(ICON_CHECK "  %d ticked", (int)g_pakTicked.size()); ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::BeginDisabled(g_impBusy > 0);
        if (AccentButton(g_impBusy > 0 ? ICON_DOWNLOAD "  Exporting..." : ICON_DOWNLOAD "  Export ticked")) {
            std::vector<std::pair<std::string, int>> list;
            {
                std::lock_guard<std::mutex> lk(g_pakModelsMx);
                std::unordered_map<std::string, uint8_t> fl; for (size_t i = 0; i < g_pakModels.size(); i++) fl[g_pakModels[i]] = g_pakFlags[i];
                for (auto& pth : g_pakTicked) {
                    auto it = fl.find(pth);
                    if (it == fl.end()) list.push_back({ pth, 0 });                                   // from the level list: kind not known
                    else if (it->second & 2) list.push_back({ pth, (it->second & 1) ? 2 : 1 });
                }
            }
            ExportModelsFromGame(list);
            g_pakTicked.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Clear")) g_pakTicked.clear();
    }

    // added this session
    if (!g_ed.added.empty()) {
        if (ImGui::CollapsingHeader(("Added by you (" + std::to_string(g_ed.added.size()) + ")###added").c_str())) {
            for (size_t i = 0; i < g_ed.added.size() && i < 50; i++) {
                auto& a = g_ed.added[i];
                ImGui::PushID((int)i + 100000);
                if (ImGui::Selectable(a.second.c_str(), a.first == g_ed.sel, 0, ImVec2(ImGui::GetContentRegionAvail().x - Px(34), 0))) { Ed("ed_select", a.first); }
                ImGui::SameLine();
                ImVec4 e = P.err;
                if (GhostButton(ICON_TRASH, ImVec2(Px(28), 0), &e)) Ed("ed_destroy", a.first);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove it");
                ImGui::PopID();
            }
        }
    }

    if (g_modelPicsSetting) {
    float bottom = ImGui::GetFrameHeightWithSpacing() + Px(10);
    ImGui::BeginChild("modelgrid", ImVec2(0, std::max(Px(120), ImGui::GetContentRegionAvail().y - bottom)), 0);
    {
        float avail = ImGui::GetContentRegionAvail().x, gap = Px(8);
        int cols = std::max(2, (int)((avail + gap) / (Px(104) + gap)));
        float cw = floorf((avail - gap * (cols - 1)) / cols);
        float rowH = cw + ImGui::GetTextLineHeight() + Px(6) + gap;
        static std::vector<int> shown; static std::string shownFilter = "\x01"; static size_t shownCount = (size_t)-1; static int shownKind = -1;
        std::unique_lock<std::mutex> lk(g_pakModelsMx, std::defer_lock);
        int count = 0;
        if (g_modelSrc == 0) count = (int)g_models.size();
        else if (g_pakModelsState >= 1) {
            lk.lock();
            if (shownFilter != f || shownCount != g_pakModels.size() || shownKind != g_pakKind) {
                shown.clear();
                for (int i = 0; i < (int)g_pakModels.size(); i++) {
                    if (g_pakKind && ((g_pakFlags[i] & 1) ? 2 : 1) != g_pakKind) continue;
                    if (f.empty() || g_pakLower[i].find(f) != std::string::npos) shown.push_back(i);
                }
                shownFilter = f; shownCount = g_pakModels.size(); shownKind = g_pakKind;
            }
            count = (int)shown.size();
        }
        int rows = (count + cols - 1) / cols;
        ImGuiListClipper clip; clip.Begin(rows, rowH);
        while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            for (int c = 0; c < cols; c++) {
                int i = r * cols + c; if (i >= count) break;
                if (c) ImGui::SameLine(0, gap);
                if (g_modelSrc == 0) ModelCard(i, g_models[i].path, g_models[i].name, cw);
                else { auto& pth = g_pakModels[shown[i]]; ModelCard(i, pth, ShortName(pth), cw, g_pakFlags[shown[i]]); }
            }
            ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
        }
        if (g_modelSrc == 1 && g_pakModelsState == 2 && g_pakModels.empty()) EdLabel(("Nothing found: " + g_pakModelsNote + ".").c_str());
        if (g_modelSrc == 1 && g_pakModelsState != 2 && count == 0) EdLabel("Reading the game's files...");
    }
    ImGui::EndChild();
    ThumbFlush();

    } else {
    float bottom = ImGui::GetFrameHeightWithSpacing() * 2 + Px(10);
    if (ImGui::BeginTable("models", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, ImVec2(0, std::max(Px(80), ImGui::GetContentRegionAvail().y - bottom)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Px(58));
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushFont(F_small); ImGui::TableHeadersRow(); ImGui::PopFont(); ImGui::PopStyleColor();
        if (g_modelSrc == 0) {
            ImGuiListClipper clip; clip.Begin((int)g_models.size());
            while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ModelRow(i, g_models[i].path, g_models[i].name);
        } else if (g_pakModelsState >= 1) {
            std::lock_guard<std::mutex> lk(g_pakModelsMx);
            static std::vector<int> shown; static std::string shownFilter = "\x01"; static size_t shownCount = (size_t)-1; static int shownKind = -1;
            if (shownFilter != f || shownCount != g_pakModels.size() || shownKind != g_pakKind) {
                shown.clear();
                for (int i = 0; i < (int)g_pakModels.size(); i++) {
                    if (g_pakKind && ((g_pakFlags[i] & 1) ? 2 : 1) != g_pakKind) continue;
                    if (f.empty() || g_pakLower[i].find(f) != std::string::npos) shown.push_back(i);
                }
                shownFilter = f; shownCount = g_pakModels.size(); shownKind = g_pakKind;
            }
            ImGuiListClipper clip; clip.Begin((int)shown.size());
            while (clip.Step()) for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) { auto& pth = g_pakModels[shown[k]]; ModelRow(k, pth, ShortName(pth), g_pakFlags[shown[k]]); }
            if (g_pakModels.empty() && g_pakModelsState == 2) { ImGui::TableNextRow(); ImGui::TableNextColumn(); EdLabel(("Nothing found: " + g_pakModelsNote + ".").c_str()); }
        } else { ImGui::TableNextRow(); ImGui::TableNextColumn(); EdLabel("Reading the game's files..."); }
        ImGui::EndTable();
    }

    }
    // add by path (e.g. your own model cooked into a .pak in Paks\~mods)
    ImGui::SetNextItemWidth(-Px(70));
    bool go = ImGui::InputTextWithHint("##mp", "Model path, e.g. /Game/Mods/MyModel/SM_MyModel", g_modelPath, sizeof g_modelPath, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((AccentButton(ICON_PLUS " Add", ImVec2(-1, 0)) || go) && g_modelPath[0]) StartPlacing(NormalizeLevelPath(g_modelPath));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your own models need to be cooked for this game and packed into a .pak\nin the game's Paks\\~mods folder. They then appear under All game files too.");
}


// Export the selected object's model straight from the game's files (Unreal 4 static meshes) into Imported models
static void AesSyncNow();   // (app.cpp) the saved decryption key for the game that's running
static void ExportSelectedButton(bool small) {
    bool has = !g_edSelParts.empty();
    ImGui::BeginDisabled(!has || g_impBusy > 0);
    std::string lbl = std::string(ICON_DOWNLOAD) + (g_impBusy > 0 ? "  Exporting..." : "  Export model");
    if (small ? ImGui::SmallButton(lbl.c_str()) : ImGui::Button(lbl.c_str())) ImGui::OpenPopup("##exportfmt");
    if (ImGui::BeginPopup("##exportfmt")) {
        // .glb / .fbx: a normal 3D file for Blender, other tools, or importing into another game (it's converted then)
        int fmt = 0;
        if (ImGui::MenuItem(ICON_DOWNLOAD "  Save as .glb (glTF)...")) fmt = 1;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("One file with the textures inside. Opens in Blender, Windows 3D Viewer and most tools.");
        if (ImGui::MenuItem(ICON_DOWNLOAD "  Save as .fbx...")) fmt = 2;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("For Blender, Maya, 3ds Max or Unreal. Textures are packed in and also saved next to it.");
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_BOX_OPEN "  Add to this game's models")) fmt = 3;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Straight into Models > Imported for the game you're playing (with its animations).");
        ImGui::EndPopup();
        if (fmt) {
            AesSyncNow();                                 // encrypted games: use the key saved for this game
            std::vector<std::string> parts = g_edSelParts; std::string xf = g_edSelActorXf; std::wstring paks = PaksDir();
            std::string name = g_ed.selName.empty() ? "Model" : g_ed.selName;
            if (parts.size() == 1) { std::string pth = g_edSelMesh; name = pth.substr(pth.find_last_of('/') + 1); size_t d = name.find('.'); if (d != std::string::npos) name = name.substr(0, d); }
            auto run = [parts, xf, paks, name, fmt](std::wstring file) {
                ImpWork([parts, xf, paks, name, fmt, file](std::string& msg) {
                    if (paks.empty()) { msg = "Choose your game on the Play page first."; return false; }
                    if (fmt == 3) {
                        std::wstring out; bool ok = ModelExportParts(paks, name, parts, xf, out, msg);
                        if (ok) msg += " It's in Models > Imported for this game.";
                        return ok;
                    }
                    bool ok = ModelExportPartsAs(paks, name, parts, xf, file, fmt, msg);
                    if (ok) msg += " Saved in " + W2U(DirOf(file)) + ". To use it in another game, import that file there.";
                    return ok;
                });
            };
            if (fmt == 3) run(L"");
            else {
                // the editor's own file browser: a Windows dialog or Explorer would take the focus (and the mouse) from the game
                std::wstring dir = DirOf(ModelsRoot()) + L"\\exported models"; MakeDirs(dir);
                std::wstring ext = fmt == 1 ? L"glb" : L"fbx";
                OvBrowse(fmt == 1 ? L"glTF binary (*.glb)\0*.glb\0" : L"FBX (*.fbx)\0*.fbx\0", L"Save the model as", dir, true, U2W(name) + L"." + ext,
                         fmt == 1 ? L"glb" : L"fbx", run);
            }
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!has) ImGui::SetTooltip("This object has no model to export");
        else {
            std::string t = "Save this object's model and colours out of the game, so you can add it to any other game";
            if (g_edSelParts.size() > 1) t += "\n(" + std::to_string(g_edSelParts.size()) + " pieces, saved together as one model)";
            for (size_t i = 0; i < g_edSelParts.size() && i < 6; i++) { std::string pth = g_edSelParts[i].substr(g_edSelParts[i].find('\t') + 1); t += "\n" + pth.substr(0, pth.find('\t')); }
            ImGui::SetTooltip("%s", t.c_str());
        }
    }
}
static void ExportSelectedButtonFwd(bool small) { ExportSelectedButton(small); }
static void MatEdOpenFwd(const std::string& actor);
static void TabInspector() {
    if (g_ed.sel.empty()) {
        EdLabel("Nothing selected.");
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextWrapped("Click anything in the game with the mouse, pick it in the Outliner, or select what's in the middle of the screen:");
        ImGui::PopStyleColor();
        if (AccentButton(ICON_CROSSHAIRS "  Select what I'm looking at")) Ed("ed_pick");
        return;
    }
    ImGui::PushFont(F_h2); ImGui::TextUnformatted(g_ed.selName.c_str()); ImGui::PopFont();
    ImGui::PushFont(F_small); EdLabel(g_ed.selClass.c_str()); ImGui::PopFont();
    const std::string& p = g_ed.sel;
    if (ImGui::Button(ICON_ARROW_RIGHT " Go to")) Ed("ed_goto", p);
    ImGui::SameLine(); ExportSelectedButton(false);
    ImGui::SameLine(); if (ImGui::Button(ICON_USER " Bring here")) Ed("ed_bring", p);
    ImGui::SameLine(); if (ImGui::Button(g_ed.shidden ? ICON_EYE " Show" : ICON_EYE " Hide")) Ed("ed_hide", p + "\t" + (g_ed.shidden ? "0" : "1"));
    ImGui::SameLine(); ImVec4 e = P.err;
    if (GhostButton(ICON_TRASH " Delete", ImVec2(0, 0), &e)) ImGui::OpenPopup("del");
    ImGui::SameLine(); if (ImGui::Button(ICON_PALETTE " Materials")) MatEdOpenFwd(p);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Material editor: change this object's materials with a node graph");
    ImGui::SameLine(); if (ImGui::Button(ICON_COPY)) Ed("ed_dupe", p);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate: add a copy of this model next to it");
    ImGui::SameLine(); if (ImGui::Button(ICON_CIRCLE_XMARK)) Ed("ed_deselect");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Deselect");
    // snapping
    if (ImGui::Button(ICON_DOWNLOAD "  Snap to ground")) Ed("ed_snap", p + "\tground");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop it straight down onto whatever is below  (%s)", g_edKeys.snapGround ? ImGui::GetKeyName((ImGuiKey)g_edKeys.snapGround) : "no hotkey");
    ImGui::SameLine();
    if (g_snapPick ? AccentButton(ICON_CROSSHAIRS "  Click where to put it...") : ImGui::Button(ICON_CROSSHAIRS "  Put on another object")) { g_snapPick = !g_snapPick; g_placePath.clear(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Then click a floor, table, wall or any object in the game:\nit's moved to sit on that surface  (%s)", g_edKeys.snapOnto ? ImGui::GetKeyName((ImGuiKey)g_edKeys.snapOnto) : "no hotkey");
    if (ImGui::BeginPopup("del")) {
        ImGui::Text("Delete %s from the level?", g_ed.selName.c_str());
        EdLabel("This can't be undone (reload the level to get it back).");
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, P.err); ImGui::PushStyleColor(ImGuiCol_Text, Hex(0xFFFFFF));
        if (ImGui::Button("Delete")) { Ed("ed_destroy", p); ImGui::CloseCurrentPopup(); }
        ImGui::PopStyleColor(2);
        ImGui::EndPopup();
    }

    ImGui::Dummy(ImVec2(0, Px(2)));
    SectionLabel("Transform");
    static float loc[3], rot[3], scl[3]; static std::string forSel; static bool editing = false;
    if (!editing || forSel != p) { memcpy(loc, g_ed.sloc, sizeof loc); memcpy(rot, g_ed.srot, sizeof rot); memcpy(scl, g_ed.sscale, sizeof scl); forSel = p; }
    bool ch = false, act = false;
    auto lab = [](const char* t) { ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(t); ImGui::SameLine(Px(80)); ImGui::SetNextItemWidth(-1); };
    lab("Location"); ch |= ImGui::DragFloat3("##loc", loc, 2.0f, 0, 0, "%.0f"); act |= ImGui::IsItemActive();
    lab("Rotation"); ch |= ImGui::DragFloat3("##rot", rot, 0.5f, -360, 360, "%.1f\xC2\xB0"); act |= ImGui::IsItemActive();
    lab("Scale");    ch |= ImGui::DragFloat3("##scl", scl, 0.01f, 0.01f, 100, "%.2f"); act |= ImGui::IsItemActive();
    editing = act;
    if (ch) Ed("ed_xform", p + "\t" + F3s(loc) + "\t" + F3s(rot) + "\t" + F3s(scl));

    ImGui::Dummy(ImVec2(0, Px(2)));
    SectionLabel("Properties");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##pf", ICON_SEARCH "  Filter properties (e.g. health, speed)", g_propFilter, sizeof g_propFilter);
    if (g_propsFor != p) { EdLabel("Loading..."); return; }
    std::string f = W2U(Lower(U2W(g_propFilter)));
    static std::string activeKey; static float activeF = 0; static int activeI = 0;
    if (ImGui::BeginTable("props", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        std::string lastCls; bool anyActive = false;
        for (size_t i = 0; i < g_props.size(); i++) {
            auto& pr = g_props[i];
            if (!f.empty() && W2U(Lower(U2W(pr.name))).find(f) == std::string::npos) continue;
            if (pr.cls != lastCls) {
                lastCls = pr.cls;
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.accent); ImGui::TextUnformatted(pr.cls.c_str()); ImGui::PopStyleColor(); ImGui::PopFont();
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(pr.name.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            ImGui::SetNextItemWidth(-1);
            std::string key = pr.cls + "." + pr.name;
            auto send = [&](const std::string& v) { pr.value = v; Ed("ed_set", p + "\t" + pr.name + "\t" + pr.kind + "\t" + v); };
            if (pr.kind == "bool") {
                bool b = pr.value == "1";
                if (ImGui::Checkbox("##v", &b)) send(b ? "1" : "0");
            } else if (pr.kind == "float") {
                float v = activeKey == key ? activeF : (float)atof(pr.value.c_str());
                if (ImGui::DragFloat("##v", &v, std::max(0.01f, std::fabs(v) * 0.005f), 0, 0, "%.3f")) { char b[64]; snprintf(b, sizeof b, "%.4f", v); send(b); }
                if (ImGui::IsItemActive()) { activeKey = key; activeF = v; anyActive = true; }
            } else if (pr.kind == "int") {
                int v = activeKey == key ? activeI : atoi(pr.value.c_str());
                if (ImGui::DragInt("##v", &v, 0.2f)) send(std::to_string(v));
                if (ImGui::IsItemActive()) { activeKey = key; activeI = v; anyActive = true; }
            } else if (pr.kind == "str") {
                char b[256]; snprintf(b, sizeof b, "%s", pr.value.c_str());
                if (ImGui::InputText("##v", b, sizeof b, ImGuiInputTextFlags_EnterReturnsTrue)) send(b);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted(pr.value.c_str()); ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        if (!anyActive) activeKey.clear();
        ImGui::EndTable();
    }
}

static void EditCamSection();
static void TabCamera() {
    EditCamSection();
    ImGui::Separator();
    bool on = g_bridge.freecam;
    SectionLabel("Freecam");
    if (AccentButton(on ? ICON_POWER "  Turn freecam off" : ICON_VIDEO "  Turn freecam on", ImVec2(Px(190), 0))) SendBridge("freecam", on ? "off" : "on");
    ImGui::SameLine();
    ImGui::BeginDisabled(!on);
    if (ImGui::Button(ICON_USER "  Move player here", ImVec2(-1, 0))) SendBridge("tp");
    ImGui::EndDisabled();
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Speed"); ImGui::SameLine(Px(120)); ImGui::SetNextItemWidth(-1);
    float sp;
    if (LiveSlider("##fcsp", g_fc.speed, sp, 0.1f, 20, "%.2fx", ImGuiSliderFlags_Logarithmic)) {
        g_fc.speed = sp; g_speedChangedAt = GetTickCount64();
        char b[32]; snprintf(b, sizeof b, "%.3f", sp); Ed("fcspeed", b);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
    EdLabel("Close the editor (F2) to fly - the game needs focus for WASD and the mouse.");

    ImGui::Dummy(ImVec2(0, Px(4)));
    SectionLabel("Field of view");
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("FOV"); ImGui::SameLine(Px(120)); ImGui::SetNextItemWidth(-Px(40));
    float fv;
    if (LiveSlider("##fov", g_ed.fov, fv, 20, 170, "%.0f\xC2\xB0")) { char b[32]; snprintf(b, sizeof b, "%.1f", fv); Ed("ed_fov", b); }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_defFov < 0);
    if (ImGui::Button(ICON_ROTATE "##fovr", ImVec2(-1, 0))) { char b[32]; snprintf(b, sizeof b, "%.1f", g_defFov); Ed("ed_fov", b); }
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, Px(4)));
    SectionLabel("Camera position");
    ImGui::Text("%s", g_bridge.cam.empty() ? "-" : g_bridge.cam.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_COPY)) ImGui::SetClipboardText(g_bridge.cam.c_str());
}

static void TabConsole(float histH = 0) {
    auto run = [](const std::string& c) {
        if (c.empty()) return;
        Ed("ed_console", c);
        g_consoleHist.erase(std::remove(g_consoleHist.begin(), g_consoleHist.end(), c), g_consoleHist.end());
        g_consoleHist.insert(g_consoleHist.begin(), c);
        if (g_consoleHist.size() > 30) g_consoleHist.pop_back();
    };
    ImGui::SetNextItemWidth(-Px(70));
    bool enter = ImGui::InputTextWithHint("##con", ICON_TERMINAL "  Unreal console command", g_consoleBuf, sizeof g_consoleBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (AccentButton("Run", ImVec2(-1, 0)) || enter) { run(g_consoleBuf); g_consoleBuf[0] = 0; ImGui::SetKeyboardFocusHere(-1); }
    SectionLabel("Quick commands");
    const char* quick[] = { "slomo 0.5", "slomo 1", "t.MaxFPS 0", "r.ScreenPercentage 100", "HighResShot 2" };
    float x = 0, avail = ImGui::GetContentRegionAvail().x;
    for (auto q : quick) {
        float w = ImGui::CalcTextSize(q).x + ImGui::GetStyle().FramePadding.x * 2;
        if (x > 0 && x + w < avail) ImGui::SameLine(); else x = 0;
        if (ImGui::Button(q)) run(q);
        x += w + ImGui::GetStyle().ItemSpacing.x;
    }
    ImGui::Dummy(ImVec2(0, Px(4)));
    SectionLabel("History");
    ImGui::BeginChild("hist", ImVec2(0, histH));
    for (auto& h : g_consoleHist) { ImGui::PushID(h.c_str()); if (ImGui::Selectable(h.c_str())) snprintf(g_consoleBuf, sizeof g_consoleBuf, "%s", h.c_str()); ImGui::PopID(); }
    if (g_consoleHist.empty()) EdLabel("Commands you run show up here.");
    ImGui::EndChild();
}

// ---- Cheats: developer console, Unreal's cheat commands, the game's own cheats and leftover developer menus
static EdCheats g_cheats;
static ULONGLONG g_cheatsSeen = 0;                 // last frame the cheats page was drawn
static char g_cheatFilter[64] = "";
static std::map<std::string, std::string> g_cheatArgs;
static float g_cheatSlomo = 1.f, g_cheatSize = 1.f;
static std::mutex g_devPakMx; static std::vector<std::string> g_devPak, g_devPakCheats; static std::string g_devPakNote; static std::atomic<int> g_devPakState{ 0 };
static std::wstring g_devPakFor;
static void CheatButtons(const std::vector<std::pair<const char*, const char*>>& items) {
    float x = 0, avail = ImGui::GetContentRegionAvail().x;
    for (auto& it : items) {
        float w = ImGui::CalcTextSize(it.first).x + ImGui::GetStyle().FramePadding.x * 2;
        if (x > 0 && x + w < avail) ImGui::SameLine(); else x = 0;
        if (ImGui::Button(it.first)) Ed("ed_cheat", it.second);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.second);
        x += w + ImGui::GetStyle().ItemSpacing.x;
    }
}
static void TabCheats() {
    ULONGLONG now = GetTickCount64();
    if (now - g_cheatsSeen > 1500) Ed("ed_devscan", "quiet");      // (re)opened: switch cheats on and look again
    g_cheatsSeen = now;                                            // (kept fresh by the main loop while the page is open)
    ReadEdCheats(g_cheats);
    if (g_devPakState != 1 && g_devPakFor != Lower(TargetExe())) g_devPakState = 0;     // another game: look again
    if (g_devPakState == 0) {
        g_devPakState = 1; g_devPakFor = Lower(TargetExe());
        std::thread([] {
            AesSyncNow();
            std::string note; std::vector<std::string> cms; auto v = PakDevMenus(note, &cms);
            std::lock_guard<std::mutex> lk(g_devPakMx); g_devPak = std::move(v); g_devPakCheats = std::move(cms); g_devPakNote = note; g_devPakState = 2; g_framesLeft = 3;
        }).detach();
    }

    SectionLabel("Developer console");
    if (!g_cheats.scanned) EdLabel("Switching cheats on...");
    else {
        ImGui::TextColored(g_cheats.cheatMgr ? P.ok : P.err, g_cheats.cheatMgr ? ICON_CHECK " Cheats on" : ICON_CIRCLE_XMARK " Cheats unavailable");
        ImGui::SameLine(0, Px(18));
        ImGui::TextColored(g_cheats.console ? P.ok : P.err, g_cheats.console ? ICON_CHECK " Console on" : ICON_CIRCLE_XMARK " Console unavailable");
        if (g_cheats.console) {
            std::string k = "Open it in the game with " + (g_cheats.consoleKeys.empty() ? std::string("~ or F10") : g_cheats.consoleKeys) + ", or type commands in the box above.";
            ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextWrapped("%s", k.c_str()); ImGui::PopStyleColor();
        }
    }
    if (ImGui::Button(ICON_ROTATE " Check again")) Ed("ed_devscan");

    ImGui::Dummy(ImVec2(0, Px(4)));
    SectionLabel("Cheats");
    CheatButtons({ { "God mode", "God" }, { "Fly", "Fly" }, { "Ghost", "Ghost" }, { "Walk", "Walk" }, { "Teleport", "Teleport" },
                   { "Debug camera", "ToggleDebugCamera" }, { "Freeze world", "PlayersOnly" }, { "Delete target", "DestroyTarget" },
                   { "Kill enemies", "KillPawns" }, { "Toggle HUD", "ShowHUD" } });

    ImGui::SetNextItemWidth(Px(170));
    ImGui::SliderFloat("##slomo", &g_cheatSlomo, 0.1f, 5.f, "Game speed %.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) { char b[48]; snprintf(b, sizeof b, "Slomo %.2f", g_cheatSlomo); Ed("ed_cheat", b); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(170));
    ImGui::SliderFloat("##size", &g_cheatSize, 0.1f, 5.f, "Player size %.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) { char b[48]; snprintf(b, sizeof b, "ChangeSize %.2f", g_cheatSize); Ed("ed_cheat", b); }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) { g_cheatSlomo = g_cheatSize = 1.f; Ed("ed_cheat", "Slomo 1"); Ed("ed_cheat", "ChangeSize 1"); }

    ImGui::Dummy(ImVec2(0, Px(4)));
    char hdr[80]; snprintf(hdr, sizeof hdr, "This game's own cheats (%d)", (int)g_cheats.cheats.size());
    SectionLabel(hdr);
    if (g_cheats.scanned && g_cheats.cheats.empty()) EdLabel("The developers didn't leave any cheat commands of their own in this game.");
    if (!g_cheats.cheats.empty()) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##cf", ICON_SEARCH "  Search cheats", g_cheatFilter, sizeof g_cheatFilter);
        std::string f = W2U(Lower(U2W(g_cheatFilter)));
        if (ImGui::BeginTable("cheats", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthFixed, Px(52));
            for (auto& c : g_cheats.cheats) {
                if (!f.empty() && W2U(Lower(U2W(c.name))).find(f) == std::string::npos) continue;
                ImGui::PushID(c.name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(c.name.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s", c.where.c_str(), c.params.empty() ? "" : "\nNeeds: ", c.params.c_str());
                ImGui::TableNextColumn();
                std::string& a = g_cheatArgs[c.name];
                bool enter = false;
                if (!c.params.empty()) {
                    char buf[128]; snprintf(buf, sizeof buf, "%s", a.c_str());
                    ImGui::SetNextItemWidth(-1);
                    enter = ImGui::InputTextWithHint("##a", c.params.c_str(), buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue);
                    a = buf;
                }
                ImGui::TableNextColumn();
                if (ImGui::Button("Run", ImVec2(-1, 0)) || enter) Ed("ed_cheat", a.empty() ? c.name : c.name + " " + a);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    ImGui::Dummy(ImVec2(0, Px(4)));
    SectionLabel("Cheat managers");
    {
        // the game's own cheat managers (in memory + in its files): using one unlocks its developers' cheats
        std::vector<EdDevMenu> cms = g_cheats.cheatClasses;
        if (g_devPakState == 2) {
            std::lock_guard<std::mutex> lk(g_devPakMx);
            for (auto& p : g_devPakCheats) {
                std::string pkg = p.substr(0, p.size() > 7 && p.compare(p.size() - 7, 7, ".uasset") == 0 ? p.size() - 7 : p.size());
                std::string n = pkg.substr(pkg.find_last_of('/') + 1);
                bool dup = false;
                for (auto& m : cms) { std::string lp = W2U(Lower(U2W(m.path))); if (lp.rfind(pkg + ".", 0) == 0 || lp == pkg) dup = true; }
                if (!dup) cms.push_back({ pkg, n, false });
            }
        }
        std::string cur = g_cheats.cheatCurrent.substr(g_cheats.cheatCurrent.find_last_of("./") + 1);
        if (!cur.empty() && cur.size() > 2 && cur.compare(cur.size() - 2, 2, "_C") == 0) cur.resize(cur.size() - 2);
        if (!cur.empty()) ImGui::TextColored(P.dim, "In use: %s", cur.c_str());
        if (cms.empty()) EdLabel(g_devPakState < 2 ? "Looking through the game's files..." : "This game only has Unreal's standard cheat manager.");
        for (auto& m : cms) {
            ImGui::PushID(("cm" + m.path).c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(m.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.path.c_str());
            ImGui::SameLine(ImGui::GetContentRegionMax().x - Px(80));
            if (m.open) { ImGui::BeginDisabled(); ImGui::Button(ICON_CHECK " In use", ImVec2(Px(80), 0)); ImGui::EndDisabled(); }
            else if (AccentButton(ICON_PLAY " Use", ImVec2(Px(80), 0))) Ed("ed_usecheat", m.path);
            ImGui::PopID();
        }
        if (!cms.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
            ImGui::TextWrapped("The game's own cheat manager holds the cheats its developers made. Use it, then its cheats appear in the list above.");
            ImGui::PopStyleColor();
        }
    }

    ImGui::Dummy(ImVec2(0, Px(4)));
    SectionLabel("Developer menus");
    // loaded in the game right now + found in its files (by name)
    std::vector<EdDevMenu> menus = g_cheats.menus;
    if (g_devPakState == 2) {
        std::lock_guard<std::mutex> lk(g_devPakMx);
        for (auto& p : g_devPak) {
            std::string pkg = p.substr(0, p.size() > 7 && p.compare(p.size() - 7, 7, ".uasset") == 0 ? p.size() - 7 : p.size());
            std::string n = pkg.substr(pkg.find_last_of('/') + 1);
            bool dup = false;
            for (auto& m : menus) if (m.path.rfind(pkg + ".", 0) == 0 || m.path == pkg) dup = true;
            if (!dup) menus.push_back({ pkg, n, false });
        }
    }
    if (menus.empty()) EdLabel(g_devPakState < 2 ? "Looking through the game's files..." : (g_devPakNote.empty() ? "None found in this game." : ("None found (" + g_devPakNote + ").").c_str()));
    for (auto& m : menus) {
        ImGui::PushID(m.path.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(m.name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.path.c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - Px(80));
        if (m.open ? ImGui::Button(ICON_CIRCLE_XMARK " Close", ImVec2(Px(80), 0)) : AccentButton(ICON_PLAY " Open", ImVec2(Px(80), 0))) Ed("ed_devmenu", m.path);
        ImGui::PopID();
    }
    if (!menus.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextWrapped("Menus the developers left in. Some only work in certain places or may not do anything in the finished game.");
        ImGui::PopStyleColor();
    }
}

// ---- small 2D helpers
static ImVec2 V2(float x, float y) { return ImVec2(x, y); }
static ImVec2 Vsub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
static ImVec2 Vadd2(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
static ImVec2 Vmul(ImVec2 a, float k) { return ImVec2(a.x * k, a.y * k); }
static float Vdot(ImVec2 a, ImVec2 b) { return a.x * b.x + a.y * b.y; }
static float Vlen(ImVec2 a) { return sqrtf(a.x * a.x + a.y * a.y); }
static float Vcross(ImVec2 a, ImVec2 b) { return a.x * b.y - a.y * b.x; }
static float DistSeg(ImVec2 p, ImVec2 a, ImVec2 b) {
    ImVec2 ab = Vsub(b, a); float l2 = Vdot(ab, ab);
    float t = l2 > 0 ? std::clamp(Vdot(Vsub(p, a), ab) / l2, 0.f, 1.f) : 0.f;
    return Vlen(Vsub(p, Vadd2(a, Vmul(ab, t))));
}

// ---- transform gizmo state
static int g_gzHover = -1, g_gzDrag = -1;            // 0/1/2 = X/Y/Z, 3 = uniform scale
static ImVec2 g_gzC, g_gzA[3];                       // centre + axis tips on screen (frozen while dragging)
static float g_gzLen = 100;
static ImVec2 g_gzStartMouse;
static float g_gzStart[3][3];                        // loc, rot, scale at drag start
static float g_gzStartCenter[3];                     // object centre at drag start (rotation pivot)
static ImVec2 g_gzTgtC, g_gzTgtA[3];                 // latest positions reported by the game (the drawn ones glide towards these)
static std::string g_gzFor;                          // which selection the drawn gizmo belongs to
static float g_gzAngPrev = 0, g_gzAngAcc = 0;
static ImVec2 g_gzOff;                               // predicted screen offset while moving
static ImVec2 g_gzStartC, g_gzStartA[3];             // gizmo on screen when the drag started (keeps dragging steady)
static float g_gzStartLen = 100;
static int g_gzMoveAxis = -1; static float g_gzMoveWd = 0;    // current move drag (world units along an axis)
static float g_gzHoldC[3]; static ULONGLONG g_gzHoldUntil = 0;  // after a move, keep the gizmo there until the game catches up
static bool g_projOn = false;                        // gizmo placed by our own projection this frame
static std::string g_gzReadout;
static std::string g_gzLastSent;
static const unsigned kAxisCol[3] = { 0xE5484D, 0x46A758, 0x3E8EF7 };
static const char* kAxisName[3] = { "X", "Y", "Z" };
// rotation rings: the plane each ring lies in (other two axes) and which rotator value it changes
static const int kRingU[3] = { 1, 0, 0 }, kRingV[3] = { 2, 2, 1 }, kRingComp[3] = { 2, 0, 1 };   // X->Roll, Y->Pitch, Z->Yaw

static ImVec2 RingPoint(int i, float t) {
    ImVec2 U = Vsub(g_gzA[kRingU[i]], g_gzC), V = Vsub(g_gzA[kRingV[i]], g_gzC);
    return Vadd2(g_gzC, Vadd2(Vmul(U, 0.8f * cosf(t)), Vmul(V, 0.8f * sinf(t))));
}

static int GizmoHitTest(ImVec2 m) {
    float best = Px(10); int hit = -1;
    if (g_gzMode == GZ_SCALE && Vlen(Vsub(m, g_gzC)) < Px(11)) return 3;
    if (g_gzMode == GZ_MOVE || g_gzMode == GZ_SCALE) {
        for (int i = 0; i < 3; i++) {
            ImVec2 dir = Vsub(g_gzA[i], g_gzC); float l = Vlen(dir); if (l < 1) continue;
            ImVec2 tip = Vadd2(g_gzA[i], Vmul(dir, Px(10) / l));
            float d = DistSeg(m, Vadd2(g_gzC, Vmul(dir, 0.15f)), tip);
            if (d < best) { best = d; hit = i; }
        }
    } else if (g_gzMode == GZ_ROTATE) {
        for (int i = 0; i < 3; i++) {
            ImVec2 prev = RingPoint(i, 0);
            for (int k = 1; k <= 48; k++) {
                ImVec2 p = RingPoint(i, k * 6.2831853f / 48);
                float d = DistSeg(m, prev, p);
                if (d < best) { best = d; hit = i; }
                prev = p;
            }
        }
    }
    return hit;
}

static void GizmoSend(const float loc[3], const float rot[3], const float scl[3]) {
    std::string a = g_ed.sel + "\t" + F3s(loc) + "\t" + F3s(rot) + "\t" + F3s(scl);
    if (a != g_gzLastSent) { g_gzLastSent = a; Ed("ed_xform", a); }
}

static void GizmoDrag(ImVec2 m, bool snap) {
    float loc[3], rot[3], scl[3];
    memcpy(loc, g_gzStart[0], sizeof loc); memcpy(rot, g_gzStart[1], sizeof rot); memcpy(scl, g_gzStart[2], sizeof scl);
    ImVec2 d = Vsub(m, g_gzStartMouse);
    char rb[64] = "";
    if (g_gzMode == GZ_MOVE && g_gzDrag < 3) {
        ImVec2 u = Vsub(g_gzStartA[g_gzDrag], g_gzStartC); float l2 = std::max(1.f, Vdot(u, u));
        float wd = Vdot(d, u) / l2 * g_gzStartLen;
        if (snap || g_snapOn[0]) { float st = std::max(0.01f, g_snapStep[0]); wd = roundf(wd / st) * st; }
        loc[g_gzDrag] += wd;
        g_gzMoveAxis = g_gzDrag; g_gzMoveWd = wd;
        g_gzOff = g_projOn ? ImVec2(0, 0) : Vmul(u, wd / g_gzStartLen);
        snprintf(rb, sizeof rb, "%s %+.0f", kAxisName[g_gzDrag], wd);
    } else if (g_gzMode == GZ_ROTATE && g_gzDrag < 3) {
        float ang = atan2f(m.y - g_gzC.y, m.x - g_gzC.x), da = ang - g_gzAngPrev;
        while (da > 3.14159265f) da -= 6.2831853f;
        while (da < -3.14159265f) da += 6.2831853f;
        g_gzAngAcc += da; g_gzAngPrev = ang;
        ImVec2 U = Vsub(g_gzA[kRingU[g_gzDrag]], g_gzC), V = Vsub(g_gzA[kRingV[g_gzDrag]], g_gzC);
        float deg = g_gzAngAcc * 57.29578f * (Vcross(U, V) >= 0 ? 1.f : -1.f);
        if (snap || g_snapOn[1]) { float st = std::max(0.01f, g_snapStep[1]); deg = roundf(deg / st) * st; }
        // ring Y runs X->Z, which is a negative turn about +Y in Unreal's maths
        float about = g_gzDrag == 1 ? -deg : deg;
        snprintf(rb, sizeof rb, "%s %+.1f\xC2\xB0", kAxisName[g_gzDrag], deg);
        g_gzReadout = rb;
        char d2[32]; snprintf(d2, sizeof d2, "%.3f", about);
        std::string a = g_ed.sel + "\t" + std::to_string(g_gzDrag) + "\t" + d2 + "\t" + F3s(g_gzStart[0]) + "\t" + F3s(g_gzStart[1]) + "\t" + F3s(g_gzStartCenter);
        if (a != g_gzLastSent) { g_gzLastSent = a; Ed("ed_rot", a); }
        return;
    } else if (g_gzMode == GZ_SCALE) {
        if (g_gzDrag == 3) {
            float f = std::max(0.01f, 1.f + (d.x - d.y) / Px(150));
            for (int i = 0; i < 3; i++) { scl[i] = g_gzStart[2][i] * f; if (snap || g_snapOn[2]) { float st = std::max(0.01f, g_snapStep[2]); scl[i] = std::max(st, roundf(scl[i] / st) * st); } }
            snprintf(rb, sizeof rb, "Scale %.2fx", scl[0] / std::max(0.0001f, g_gzStart[2][0]));
        } else {
            ImVec2 u = Vsub(g_gzStartA[g_gzDrag], g_gzStartC); float l2 = std::max(1.f, Vdot(u, u));
            float t = Vdot(d, u) / l2;
            scl[g_gzDrag] = std::max(0.01f, g_gzStart[2][g_gzDrag] * (1 + t));
            if (snap || g_snapOn[2]) { float st = std::max(0.01f, g_snapStep[2]); scl[g_gzDrag] = std::max(st, roundf(scl[g_gzDrag] / st) * st); }
            snprintf(rb, sizeof rb, "%s scale %.2f", kAxisName[g_gzDrag], scl[g_gzDrag]);
        }
    }
    g_gzReadout = rb;
    GizmoSend(loc, rot, scl);
}

static void DrawGizmo(ImDrawList* dl) {
    ImU32 shadow = IM_COL32(0, 0, 0, 150), hi = IM_COL32(255, 214, 10, 255);
    ImVec2 c = Vadd2(g_gzC, g_gzOff);
    float th = Px(3.5f), sh = Px(6.5f);
    auto col = [&](int i) { int a = g_gzDrag >= 0 ? g_gzDrag : g_gzHover; return a == i ? hi : ImGui::GetColorU32(Hex(kAxisCol[i])); };
    // draw the axis pointing most away from the camera first, so nearer ones sit on top
    int order[3] = { 0, 1, 2 };
    if (g_gzMode == GZ_MOVE || g_gzMode == GZ_SCALE) {
        for (int k = 0; k < 3; k++) {
            int i = order[k];
            ImVec2 a = Vadd2(g_gzA[i], g_gzOff), dir = Vsub(a, c); float l = Vlen(dir); if (l < 2) continue;
            ImVec2 n = Vmul(dir, 1.f / l), pr(-n.y, n.x);
            ImVec2 from = Vadd2(c, Vmul(n, Px(7)));
            dl->AddLine(from, a, shadow, sh); dl->AddLine(from, a, col(i), th);
            if (g_gzMode == GZ_MOVE) {
                ImVec2 tip = Vadd2(a, Vmul(n, Px(16))), b1 = Vadd2(a, Vmul(pr, Px(7))), b2 = Vsub(a, Vmul(pr, Px(7)));
                dl->AddTriangleFilled(Vadd2(tip, Vmul(n, Px(1.5f))), Vadd2(b1, Vmul(pr, Px(1.5f))), Vsub(b2, Vmul(pr, Px(1.5f))), shadow);
                dl->AddTriangleFilled(tip, b1, b2, col(i));
            } else {
                float r = Px(6);
                dl->AddRectFilled(V2(a.x - r - 1, a.y - r - 1), V2(a.x + r + 1, a.y + r + 1), shadow, Px(2));
                dl->AddRectFilled(V2(a.x - r, a.y - r), V2(a.x + r, a.y + r), col(i), Px(2));
            }
            ImGui::PushFont(F_small);
            dl->AddText(Vadd2(a, Vmul(n, Px(g_gzMode == GZ_MOVE ? 24 : 14))), col(i), kAxisName[i]);
            ImGui::PopFont();
        }
        if (g_gzMode == GZ_SCALE) {
            bool h = (g_gzDrag >= 0 ? g_gzDrag : g_gzHover) == 3; float r = Px(7);
            dl->AddRectFilled(V2(c.x - r - 1, c.y - r - 1), V2(c.x + r + 1, c.y + r + 1), shadow, Px(2));
            dl->AddRectFilled(V2(c.x - r, c.y - r), V2(c.x + r, c.y + r), h ? hi : IM_COL32(240, 240, 240, 255), Px(2));
        } else dl->AddCircleFilled(c, Px(4.5f), IM_COL32(240, 240, 240, 255), 16);
    } else if (g_gzMode == GZ_ROTATE) {
        for (int i = 0; i < 3; i++) {
            ImVec2 pts[64];
            for (int k = 0; k < 64; k++) pts[k] = RingPoint(i, k * 6.2831853f / 64);
            dl->AddPolyline(pts, 64, shadow, ImDrawFlags_Closed, sh);
            dl->AddPolyline(pts, 64, col(i), ImDrawFlags_Closed, th);
        }
        dl->AddCircleFilled(c, Px(4), IM_COL32(240, 240, 240, 255), 16);
    }
}

// ---- yellow selection outline (like Unreal's editor): the selected object's real shape is read from the game's
// files, drawn into a mask with the game's camera every frame, and its edge is put over the game in yellow
struct SelOutline {
    std::string key;                      // selection + pieces it was made for
    std::vector<float> local;             // vertices in the object's own space (so it follows moves / turns / scaling)
    std::vector<uint32_t> idx;
    int state = 0;                        // 0 none, 1 loading, 2 ready, 3 failed
    ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; int tw = 0, th = 0;
    std::vector<uint8_t> mask; std::vector<uint32_t> px;
    float lastSig[16]{}; bool drawn = false;
};
static SelOutline g_sol;
static std::mutex g_solMx;
static std::atomic<int> g_solGen{ 0 };
static bool g_selOutlineOn = true;
struct Xf3 { float l[3], F[3], R[3], U[3], s[3]; };
static void RotAxes(const float rot[3], float F[3], float R[3], float U[3]) {
    const float d = 0.017453293f;
    float cp = cosf(rot[0] * d), sp = sinf(rot[0] * d), cy = cosf(rot[1] * d), sy = sinf(rot[1] * d), cr = cosf(rot[2] * d), sr = sinf(rot[2] * d);
    F[0] = cp * cy; F[1] = cp * sy; F[2] = sp;
    R[0] = sr * sp * cy - cr * sy; R[1] = sr * sp * sy + cr * cy; R[2] = -sr * cp;
    U[0] = -(cr * sp * cy + sr * sy); U[1] = cy * sr - cr * sp * sy; U[2] = cr * cp;
}
static void SelOutlineRequest() {
    if (g_ed.sel.empty() || g_edSelParts.empty() || g_edSelActorXf.empty() || g_propsFor != g_ed.sel) return;   // the pieces of this object, not the last one
    std::string key = g_ed.sel; for (auto& p : g_edSelParts) key += "|" + p.substr(0, p.find('\t', p.find('\t') + 1));
    { std::lock_guard<std::mutex> lk(g_solMx); if (g_sol.key == key) return; g_sol.key = key; g_sol.state = 1; g_sol.local.clear(); g_sol.idx.clear(); }
    int gen = ++g_solGen;
    std::vector<std::string> parts = g_edSelParts; std::string xf = g_edSelActorXf; std::wstring paks = PaksDir();
    std::thread([parts, xf, paks, gen] {
        std::vector<float> xyz; std::vector<uint32_t> idx; std::string err;
        bool ok = !paks.empty() && ModelOutlineMesh(paks, parts, xyz, idx, err);
        // world -> the object's own space, using where the object was when its pieces were listed
        float al[3] = { 0, 0, 0 }, ar[3] = { 0, 0, 0 }, as[3] = { 1, 1, 1 };
        sscanf(xf.c_str(), "%f,%f,%f\t%f,%f,%f\t%f,%f,%f", &al[0], &al[1], &al[2], &ar[0], &ar[1], &ar[2], &as[0], &as[1], &as[2]);
        float F[3], R[3], U[3]; RotAxes(ar, F, R, U);
        for (size_t i = 0; i + 2 < xyz.size(); i += 3) {
            float d[3] = { xyz[i] - al[0], xyz[i + 1] - al[1], xyz[i + 2] - al[2] };
            float x = d[0] * F[0] + d[1] * F[1] + d[2] * F[2], y = d[0] * R[0] + d[1] * R[1] + d[2] * R[2], z = d[0] * U[0] + d[1] * U[1] + d[2] * U[2];
            xyz[i] = x / (as[0] ? as[0] : 1); xyz[i + 1] = y / (as[1] ? as[1] : 1); xyz[i + 2] = z / (as[2] ? as[2] : 1);
        }
        std::lock_guard<std::mutex> lk(g_solMx);
        if (gen != g_solGen) return;
        if (ok) { g_sol.local.swap(xyz); g_sol.idx.swap(idx); g_sol.state = 2; } else g_sol.state = 3;
        g_framesLeft = 3;
    }).detach();
}
static void DrawSelOutline(ImDrawList* dl, ImGuiViewport* vp, const CamPose& pose) {
    if (!g_selOutlineOn || g_ed.sel.empty()) return;
    SelOutlineRequest();
    std::lock_guard<std::mutex> lk(g_solMx);
    if (g_sol.state != 2 || g_sol.idx.empty() || !g_dev || !g_ctx) return;
    if (g_sol.key.compare(0, g_ed.sel.size() + 1, g_ed.sel + "|") != 0) return;     // made for another object
    // mask size: half the game view (sharp enough for a 2-3 pixel line, quick to fill)
    float k = std::min(0.5f, 1280.f / std::max(1.f, vp->Size.x));
    int W = std::max(16, (int)(vp->Size.x * k)), H = std::max(16, (int)(vp->Size.y * k));
    // the object where the game has it now
    float F[3], R[3], U[3]; RotAxes(g_ed.srot, F, R, U);
    const float* L = g_ed.sloc; const float* S = g_ed.sscale;
    float sig[16] = { pose.loc[0], pose.loc[1], pose.loc[2], pose.rot[0], pose.rot[1], pose.rot[2], pose.fov, L[0], L[1], L[2],
                      g_ed.srot[0], g_ed.srot[1], g_ed.srot[2], S[0] + S[1] * 3 + S[2] * 7, (float)W, (float)(g_sol.idx.size() + H) };
    bool same = g_sol.drawn && memcmp(sig, g_sol.lastSig, sizeof sig) == 0;
    if (!same) {
        memcpy(g_sol.lastSig, sig, sizeof sig);
        size_t nv = g_sol.local.size() / 3;
        std::vector<float> sx(nv), sy(nv); std::vector<uint8_t> vis(nv);
        // camera axes (the same maths as the gizmo's projection)
        float cF[3], cR[3], cU[3]; RotAxes(pose.rot, cF, cR, cU);
        float hx, hy; ProjHalf(pose, g_projMode, hx, hy);
        for (size_t i = 0; i < nv; i++) {
            const float* v = &g_sol.local[i * 3];
            float lx = v[0] * S[0], ly = v[1] * S[1], lz = v[2] * S[2];
            float w[3] = { L[0] + F[0] * lx + R[0] * ly + U[0] * lz, L[1] + F[1] * lx + R[1] * ly + U[1] * lz, L[2] + F[2] * lx + R[2] * ly + U[2] * lz };
            float r[3] = { w[0] - pose.loc[0], w[1] - pose.loc[1], w[2] - pose.loc[2] };
            float x = r[0] * cF[0] + r[1] * cF[1] + r[2] * cF[2];
            float y = r[0] * cR[0] + r[1] * cR[1] + r[2] * cR[2];
            float z = r[0] * cU[0] + r[1] * cU[1] + r[2] * cU[2];
            vis[i] = x > 1.f;
            if (vis[i]) { sx[i] = (0.5f + 0.5f * y / (x * hx)) * W; sy[i] = (0.5f - 0.5f * z / (x * hy)) * H; }
        }
        g_sol.mask.assign((size_t)W * H, 0);
        uint8_t* M = g_sol.mask.data();
        for (size_t i = 0; i + 2 < g_sol.idx.size(); i += 3) {
            uint32_t a = g_sol.idx[i], b = g_sol.idx[i + 1], c = g_sol.idx[i + 2];
            if (a >= nv || b >= nv || c >= nv || !vis[a] || !vis[b] || !vis[c]) continue;
            float x0 = sx[a], y0 = sy[a], x1 = sx[b], y1 = sy[b], x2 = sx[c], y2 = sy[c];
            float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (fabsf(area) < 1e-6f) continue;
            int minx = std::max(0, (int)floorf(std::min({ x0, x1, x2 }))), maxx = std::min(W - 1, (int)ceilf(std::max({ x0, x1, x2 })));
            int miny = std::max(0, (int)floorf(std::min({ y0, y1, y2 }))), maxy = std::min(H - 1, (int)ceilf(std::max({ y0, y1, y2 })));
            if (minx > maxx || miny > maxy) continue;
            if (maxx == minx && maxy == miny) { M[(size_t)miny * W + minx] = 1; continue; }   // smaller than a pixel
            float inv = 1.f / area;
            for (int yy = miny; yy <= maxy; yy++) {
                float py = yy + 0.5f; uint8_t* row = M + (size_t)yy * W;
                for (int xx = minx; xx <= maxx; xx++) {
                    float pxx = xx + 0.5f;
                    float w0 = ((x1 - pxx) * (y2 - py) - (x2 - pxx) * (y1 - py)) * inv;
                    float w1 = ((x2 - pxx) * (y0 - py) - (x0 - pxx) * (y2 - py)) * inv;
                    if (w0 < 0 || w1 < 0 || w0 + w1 > 1) continue;
                    row[xx] = 1;
                }
            }
        }
        // the edge: inside pixels next to an outside pixel, thickened by one
        g_sol.px.assign((size_t)W * H, 0);
        const uint32_t yel = 0xFF0AD6FF;      // ABGR: Unreal's selection yellow-orange
        auto at = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H ? M[(size_t)y * W + x] : 0; };
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            bool in = M[(size_t)y * W + x];
            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; dy++) for (int dx = -1; dx <= 1 && !edge; dx++) if ((dx || dy) && at(x + dx, y + dy) != in) edge = true;
            if (edge) g_sol.px[(size_t)y * W + x] = yel;
        }
        // upload
        if (!g_sol.tex || g_sol.tw != W || g_sol.th != H) {
            if (g_sol.srv) { g_sol.srv->Release(); g_sol.srv = nullptr; }
            if (g_sol.tex) { g_sol.tex->Release(); g_sol.tex = nullptr; }
            D3D11_TEXTURE2D_DESC d{}; d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_sol.tex))) { g_sol.tex = nullptr; return; }
            g_dev->CreateShaderResourceView(g_sol.tex, nullptr, &g_sol.srv);
            g_sol.tw = W; g_sol.th = H;
        }
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(g_sol.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            for (int y = 0; y < H; y++) memcpy((uint8_t*)ms.pData + (size_t)y * ms.RowPitch, &g_sol.px[(size_t)y * W], (size_t)W * 4);
            g_ctx->Unmap(g_sol.tex, 0);
        }
        g_sol.drawn = true;
    }
    if (g_sol.srv) dl->AddImage((ImTextureID)(intptr_t)g_sol.srv, vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));
}

// Click anywhere in the game (outside the editor panels) to select what's under the mouse;
// drag the gizmo handles to move / rotate / scale the selection.
static void WorldPicking(ImGuiViewport* vp) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImVec2 m = io.MousePos;
    bool inVp = g_ueVpMaxF.x <= g_ueVpMinF.x || (m.x >= g_ueVpMinF.x && m.x < g_ueVpMaxF.x && m.y >= g_ueVpMinF.y && m.y < g_ueVpMaxF.y);
    bool overGame = !io.WantCaptureMouse && ImGui::IsMousePosValid() && inVp && g_dragPath.empty();

    bool haveSel = !g_ed.sel.empty();
    // our own projection every frame when it's been checked against the game; otherwise follow the game's updates
    CamPose pose;
    g_projOn = haveSel && g_ed.hasCenter && (g_projMode == 0 || g_projMode == 1 || g_projMode == 3) && CurrentPose(pose);
    bool projVis = false;
    if (g_projOn) {
        float c[3]; memcpy(c, g_ed.scenter, sizeof c);
        if (g_gzDrag >= 0 && g_gzMode == GZ_MOVE && g_gzMoveAxis >= 0) { memcpy(c, g_gzStartCenter, sizeof c); c[g_gzMoveAxis] += g_gzMoveWd; }
        else if (g_gzHoldUntil) {
            float dd = fabsf(g_ed.scenter[0] - g_gzHoldC[0]) + fabsf(g_ed.scenter[1] - g_gzHoldC[1]) + fabsf(g_ed.scenter[2] - g_gzHoldC[2]);
            if (GetTickCount64() > g_gzHoldUntil || dd < 1.f || g_gzFor != g_ed.sel) g_gzHoldUntil = 0;
            else memcpy(c, g_gzHoldC, sizeof c);
        }
        float dx = c[0] - pose.loc[0], dy = c[1] - pose.loc[1], dz = c[2] - pose.loc[2];
        float len = std::max(1.f, sqrtf(dx * dx + dy * dy + dz * dz) * 0.16f);
        float sx, sy, ax[3], ay[3]; bool ok = ProjectPose(pose, g_projMode, c, sx, sy);
        for (int i = 0; i < 3 && ok; i++) { float t[3] = { c[0], c[1], c[2] }; t[i] += len; ok = ProjectPose(pose, g_projMode, t, ax[i], ay[i]); }
        if (ok) {
            g_gzC = V2(vp->Pos.x + sx * vp->Size.x, vp->Pos.y + sy * vp->Size.y);
            for (int i = 0; i < 3; i++) g_gzA[i] = V2(vp->Pos.x + ax[i] * vp->Size.x, vp->Pos.y + ay[i] * vp->Size.y);
            g_gzLen = len; g_gzOff = ImVec2(0, 0); g_gzFor = g_ed.sel;
            projVis = true;
        }
    }
    if (g_projOn && haveSel) DrawSelOutline(bg, vp, pose);
    bool shown = g_projOn ? projVis : g_ed.hasGizmo;
    bool gizmo = haveSel && shown && g_gzMode != GZ_SELECT;
    if (!g_projOn && g_ed.hasGizmo) {                                     // where the game says the gizmo is now
        g_gzTgtC = V2(vp->Pos.x + g_ed.gz[0] * vp->Size.x, vp->Pos.y + g_ed.gz[1] * vp->Size.y);
        for (int i = 0; i < 3; i++) g_gzTgtA[i] = V2(vp->Pos.x + g_ed.gz[2 + i * 2] * vp->Size.x, vp->Pos.y + g_ed.gz[3 + i * 2] * vp->Size.y);
    }
    if (!g_projOn && g_gzDrag < 0 && g_ed.hasGizmo) {        // glide smoothly towards it (snap on a new selection)
        bool snapNow = g_gzFor != g_ed.sel;
        float k = snapNow ? 1.f : 1.f - expf(-io.DeltaTime * 18.f);
        // fold the local move prediction into the drawn position so letting go doesn't jump back
        g_gzC = Vadd2(g_gzC, g_gzOff);
        for (int i = 0; i < 3; i++) g_gzA[i] = Vadd2(g_gzA[i], g_gzOff);
        g_gzOff = ImVec2(0, 0);
        g_gzC = Vadd2(g_gzC, Vmul(Vsub(g_gzTgtC, g_gzC), k));
        for (int i = 0; i < 3; i++) g_gzA[i] = Vadd2(g_gzA[i], Vmul(Vsub(g_gzTgtA[i], g_gzA[i]), k));
        g_gzLen = g_ed.gzLen;
        g_gzFor = g_ed.sel;
    }
    g_gzHover = (gizmo && overGame && g_gzDrag < 0) ? GizmoHitTest(m) : -1;

    if (g_gzDrag >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && haveSel) {
            GizmoDrag(m, io.KeyCtrl);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else {
            if (g_gzMode == GZ_MOVE && g_gzMoveAxis >= 0 && g_gzMoveWd != 0) {   // stay where it was dropped until the game reports the new spot
                memcpy(g_gzHoldC, g_gzStartCenter, sizeof g_gzHoldC); g_gzHoldC[g_gzMoveAxis] += g_gzMoveWd;
                g_gzHoldUntil = GetTickCount64() + 1000;
            }
            g_gzDrag = -1; g_gzMoveAxis = -1; g_gzReadout.clear(); g_gzLastSent.clear();
        }
    } else if (overGame && g_bridge.connected) {
        ImGui::SetMouseCursor(g_gzHover >= 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (g_snapPick && haveSel) {                     // put the selected object on what you clicked
                float nx = (m.x - vp->Pos.x) / vp->Size.x, ny = (m.y - vp->Pos.y) / vp->Size.y;
                char b[48]; snprintf(b, sizeof b, "%.4f,%.4f", nx, ny);
                Ed("ed_snap", g_ed.sel + "\tat\t" + b);
                g_clickPos = m; g_clickAt = GetTickCount64();
                g_snapPick = false;
            } else if (!g_placePath.empty()) {               // place the chosen model where you clicked
                float nx = (m.x - vp->Pos.x) / vp->Size.x, ny = (m.y - vp->Pos.y) / vp->Size.y;
                char b[48]; snprintf(b, sizeof b, "%.4f,%.4f", nx, ny);
                UeSpawnAt(g_placePath, nx, ny); (void)b;
                g_clickPos = m; g_clickAt = GetTickCount64();
                if (!io.KeyShift) g_placePath.clear();       // hold Shift to keep placing copies
            } else if (g_gzHover >= 0) {                     // start dragging a handle
                g_gzDrag = g_gzHover; g_gzStartMouse = m;
                memcpy(g_gzStart[0], g_ed.sloc, sizeof g_gzStart[0]); memcpy(g_gzStart[1], g_ed.srot, sizeof g_gzStart[1]); memcpy(g_gzStart[2], g_ed.sscale, sizeof g_gzStart[2]);
                if (g_ed.hasCenter) memcpy(g_gzStartCenter, g_ed.scenter, sizeof g_gzStartCenter); else memcpy(g_gzStartCenter, g_ed.sloc, sizeof g_gzStartCenter);
                g_gzAngPrev = atan2f(m.y - g_gzC.y, m.x - g_gzC.x); g_gzAngAcc = 0;
                g_gzStartC = g_gzC; for (int i = 0; i < 3; i++) g_gzStartA[i] = g_gzA[i];
                g_gzStartLen = g_gzLen; g_gzMoveAxis = -1; g_gzMoveWd = 0; g_gzHoldUntil = 0;
            } else {                                         // select what's under the mouse
                float nx = (m.x - vp->Pos.x) / vp->Size.x, ny = (m.y - vp->Pos.y) / vp->Size.y;
                char b[48]; snprintf(b, sizeof b, "%.4f,%.4f", nx, ny);
                Ed("ed_pickat", b);
                g_clickPos = m; g_clickAt = GetTickCount64();
                g_edSelectTab = 2;
            }
        }
    }

    // click ripple
    float t = (GetTickCount64() - g_clickAt) / 400.f;
    if (g_clickAt && t < 1.f) bg->AddCircle(g_clickPos, Px(6) + t * Px(26), ImGui::GetColorU32(Alpha(P.accent, 1.f - t)), 32, Px(2.5f));

    // selection: gizmo (or a ring in select mode) + name label
    if (haveSel && (g_projOn ? projVis : (g_ed.hasScreen || g_ed.hasGizmo))) {
        ImVec2 c = (g_projOn || g_ed.hasGizmo) ? Vadd2(g_gzC, g_gzOff) : V2(vp->Pos.x + g_ed.sscreen[0] * vp->Size.x, vp->Pos.y + g_ed.sscreen[1] * vp->Size.y);
        if (gizmo) DrawGizmo(bg);
        else {
            float r = Px(14);
            bg->AddCircle(c, r + Px(1), IM_COL32(0, 0, 0, 160), 40, Px(5));
            bg->AddCircle(c, r, ImGui::GetColorU32(P.accent), 40, Px(2.5f));
        }
        ImGui::PushFont(F_med);
        const std::string& label = g_gzReadout.empty() ? g_ed.selName : g_gzReadout;
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        ImVec2 lp = g_gzDrag >= 0 ? V2(m.x + Px(18), m.y + Px(14)) : V2(c.x - ts.x * 0.5f, c.y + Px(30));
        bg->AddRectFilled(V2(lp.x - Px(10), lp.y - Px(5)), V2(lp.x + ts.x + Px(10), lp.y + ts.y + Px(5)), ImGui::GetColorU32(Alpha(P.card, 0.92f)), Px(8));
        bg->AddText(lp, ImGui::GetColorU32(g_gzDrag >= 0 ? Hex(0xFFD60A) : P.text), label.c_str());
        ImGui::PopFont();
    }

    // what a click will do, next to the cursor
    if (overGame && (!g_placePath.empty() || (g_snapPick && haveSel)) && g_gzDrag < 0) {
        ImGui::PushFont(F_small);
        std::string hint = g_snapPick ? "Click where to put " + g_ed.selName : "Click to place " + g_placeName;
        ImVec2 ts = ImGui::CalcTextSize(hint.c_str()), lp = V2(m.x + Px(20), m.y + Px(18));
        bg->AddRectFilled(V2(lp.x - Px(8), lp.y - Px(4)), V2(lp.x + ts.x + Px(8), lp.y + ts.y + Px(4)), ImGui::GetColorU32(Alpha(P.card, 0.92f)), Px(6));
        bg->AddText(lp, ImGui::GetColorU32(P.accent), hint.c_str());
        ImGui::PopFont();
    }
}

// Editing camera settings (top of the editor's Camera tab)
static void EditCamSection() {
    SectionLabel("Editing camera");
    bool fc = g_edFreecam;
    if (ToggleSwitch("##edfc", &fc)) { g_edFreecam = fc; SaveSettings(); Ed("ed_editcam", fc ? "1" : "0"); }
    ImGui::SameLine(0, Px(10)); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Freecam while editing");
    if (g_edFreecam) {
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Speed"); ImGui::SameLine(Px(70));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##flyspd", &g_flySpeed, 0.05f, 30.f, "%.2fx", ImGuiSliderFlags_Logarithmic)) SaveSettings();
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(g_padSeen ? "Controller: left stick moves, right stick looks, LT/RT down/up, LB/RB speed, A selects what's in the middle, B deselects, Y changes tool."
                                         : "Controller: left stick moves, right stick looks, LT/RT down/up, LB/RB speed (plug one in any time).");
        ImGui::TextUnformatted("Mouse: hold the right button on the game, then look with the mouse and fly with W A S D, Q / E down / up, Shift faster, wheel for speed.");
        ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(0, Px(4)));
}

// Player tab of the editor: the same as the launcher's Player page, usable in-game without pausing it
static void TabPlayerInGame() { TabPlayer(); }   // (Turn into another character is on the launcher's Player page)

// Select / Move / Rotate / Scale (in the editor window)
static void DrawToolBar() {
    bool haveSel = !g_ed.sel.empty();
    struct { int mode; const char* icon; const char* name; int key; } tools[] = {
        { GZ_SELECT, ICON_CROSSHAIRS, "Select", g_edKeys.select }, { GZ_MOVE, ICON_ARROWS, "Move", g_edKeys.move },
        { GZ_ROTATE, ICON_ROTATE, "Rotate", g_edKeys.rotate }, { GZ_SCALE, ICON_EXPAND, "Scale", g_edKeys.scale } };
    for (auto& tl : tools) {
        char b[64]; snprintf(b, sizeof b, "%s  %s", tl.icon, tl.name);
        if (tl.mode != GZ_SELECT) ImGui::SameLine(0, Px(6));
        if (g_gzMode == tl.mode ? AccentButton(b) : ImGui::Button(b)) { g_gzMode = tl.mode; SaveSettings(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s tool  (%s)", tl.name, ImGui::GetKeyName((ImGuiKey)tl.key));
    }
    ImGui::SameLine(0, Px(14));
    ImGui::BeginDisabled(g_ed.undoCount <= 0);
    if (ImGui::Button(ICON_ROTATE "  Undo")) Ed("ed_undo");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (g_ed.undoCount > 0) ImGui::SetTooltip("Undo: %s  (Ctrl+Z)\n%d step%s you can undo", g_ed.undoWhat.c_str(), g_ed.undoCount, g_ed.undoCount == 1 ? "" : "s");
        else ImGui::SetTooltip("Nothing to undo yet  (Ctrl+Z)");
    }
    ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushFont(F_small);
    if (!g_placePath.empty()) ImGui::Text("Click in the game to place %s  \xC2\xB7  Esc cancels", g_placeName.c_str());
    else ImGui::TextUnformatted(haveSel ? (g_gzMode == GZ_SELECT ? "Click things in the game to select them" : "Drag a handle on the game  \xC2\xB7  Ctrl snaps")
                                        : "Click something in the game to select it");
    ImGui::PopFont(); ImGui::PopStyleColor();
}

// Editor hotkeys (only while the editor has focus and you're not typing).
static ULONGLONG g_delArmed = 0;
static void EditorHotkeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || g_gzDrag >= 0 || g_flyRMB) return;   // (while flying, WASD/Q/E move the camera)
    auto pressed = [](int k) { return k > 0 && ImGui::IsKeyPressed((ImGuiKey)k, false); };
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { Ed("ed_undo"); return; }
    if (pressed(g_edKeys.walk)) Ed("ed_move", "walk");
    if (pressed(g_edKeys.fly)) Ed("ed_move", "fly");
    if (pressed(g_edKeys.ghost)) Ed("ed_move", "ghost");
    if (g_snapPick && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g_snapPick = false; return; }
    if (pressed(g_edKeys.select)) { g_gzMode = GZ_SELECT; SaveSettings(); }
    if (pressed(g_edKeys.move)) { g_gzMode = GZ_MOVE; SaveSettings(); }
    if (pressed(g_edKeys.rotate)) { g_gzMode = GZ_ROTATE; SaveSettings(); }
    if (pressed(g_edKeys.scale)) { g_gzMode = GZ_SCALE; SaveSettings(); }
    if (!g_placePath.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g_placePath.clear(); return; }
    if (g_ed.sel.empty()) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) { Ed("ed_dupe", g_ed.sel); g_olRefreshAt = GetTickCount64() + 800; return; }
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) { Ed("ed_focus", g_ed.sel); return; }
    if (pressed(g_edKeys.deselect)) Ed("ed_deselect");
    if (pressed(g_edKeys.hide)) Ed("ed_hide", g_ed.sel + "\t" + (g_ed.shidden ? "0" : "1"));
    if (pressed(g_edKeys.snapGround)) Ed("ed_snap", g_ed.sel + "\tground");
    if (pressed(g_edKeys.snapOnto)) { g_snapPick = true; g_placePath.clear(); }
    if (pressed(g_edKeys.del)) {
        if (g_delArmed && GetTickCount64() - g_delArmed < 2500) { Ed("ed_destroy", g_ed.sel); g_delArmed = 0; g_olRefreshAt = GetTickCount64() + 800; }
        else {
            g_delArmed = GetTickCount64();
            g_toasts.push_back({ "Press " + std::string(ImGui::GetKeyName((ImGuiKey)g_edKeys.del)) + " again to delete " + g_ed.selName, true, GetTickCount64() });
        }
    }
}

// ---- the layer over the game: only the axis, the selection and click-to-select
static void UeDrawLayout();
static void DrawGameLayer() {
    EditorHotkeys();
    ImGuiViewport* mv = ImGui::GetMainViewport();
    g_ueOvOrigin = mv->Pos;
    ImGuiViewport gv = *mv;                 // where the game's picture is: all of the layer, or (fitted) the viewport
    RECT gr;
    if (g_ueFitted && g_ueHost.right > g_ueHost.left && GameClientRect(gr)) {
        gv.Pos = ImVec2(mv->Pos.x + (float)(gr.left - g_ueHost.left), mv->Pos.y + (float)(gr.top - g_ueHost.top));
        gv.Size = ImVec2((float)(gr.right - gr.left), (float)(gr.bottom - gr.top));
    }
    g_ueGamePos = gv.Pos; g_ueGameSize = gv.Size;
    WorldPicking(&gv);
    UeDrawLayout();
    UeFitStep(g_ueVpMinF, g_ueVpMaxF);
}

// Where the editor's cursor is on screen (the hooks' cursor while the game has focus, else the real one).
static POINT ScreenMouse() {
    POINT p;
    if (CaptureActive()) { std::lock_guard<std::mutex> lk(g_inMx); p = g_virt; }
    else GetCursorPos(&p);
    return p;
}

#include "mated.inc"
static void MatEdOpenFwd(const std::string& actor) { MatEdOpen(actor); }
static void MatEdCloseFwd() { if (M.open) MatEdClose(); }
#include "ueui.inc"

// ---- the editor window
static void DrawEditorWindow() {
    CheckGameSwitch();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    EditorHotkeys();
    UeThemePush();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##ue4sseditor", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    // header: drag it to move the window (the title bar can't be reached by the editor's own cursor)
    {
        ImVec2 p = ImGui::GetCursorScreenPos(); float d = Px(32);
        ImVec2 hdr0 = p;
        if (g_logo) ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)(intptr_t)g_logo, p, ImVec2(p.x + d, p.y + d), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, Px(6));
        ImGui::Dummy(ImVec2(d, d)); ImGui::SameLine(0, Px(10));
        ImGui::BeginGroup();
        ImGui::PushFont(F_med); ImGui::TextUnformatted("UE4SS Editor"); ImGui::PopFont();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6));
        ImGui::PushFont(F_small);
        std::string g = W2U(GameDisplayName()), m = g_bridge.currentMap.substr(g_bridge.currentMap.find_last_of('/') + 1);
        EdLabel((g + (m.empty() ? "" : "  \xC2\xB7  " + m)).c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        float right = ImGui::GetContentRegionMax().x;
        ImGui::SameLine(right - Px(30));
        ImVec2 closeAt = ImGui::GetCursorScreenPos();
        if (ImGui::Button(ICON_CIRCLE_XMARK "##close", ImVec2(Px(30), Px(30)))) OvHide();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close (F2)");
        // drag area: the header minus the close button
        ImVec2 back = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(hdr0);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##drag", ImVec2(closeAt.x - hdr0.x - Px(6), d));
        static POINT grab;
        if (ImGui::IsItemActivated()) { POINT sm = ScreenMouse(); RECT wr; GetWindowRect(g_edWnd, &wr); grab = { sm.x - wr.left, sm.y - wr.top }; }
        if (ImGui::IsItemActive()) {
            POINT sm = ScreenMouse();
            SetWindowPos(g_edWnd, nullptr, sm.x - grab.x, sm.y - grab.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (ImGui::IsItemDeactivated()) SaveEdWindowRect();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        ImGui::SetCursorScreenPos(back);
    }
    ImGui::Dummy(ImVec2(0, Px(4)));
    if (!g_bridge.connected) { EdLabel("The game isn't connected."); ImGui::End(); UeThemePop(); return; }
    if (M.open) {                       // the material editor takes the whole window while it's open
        ImGui::BeginChild("##mated", ImVec2(0, -Px(4)), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        MatEdDraw();
        ImGui::EndChild();
    } else {                            // panels undocked from around the game
        ImGui::PushFont(F_small);
        UeFloatingPanels();
        ImGui::PopFont();
    }

    // resize grip (bottom-right corner), also usable with the editor's own cursor
    {
        float gsz = Px(16);
        ImVec2 br(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
        ImGui::SetCursorScreenPos(ImVec2(br.x - gsz, br.y - gsz));
        ImGui::InvisibleButton("##grip", ImVec2(gsz, gsz));
        bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
        ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(br.x - Px(3), br.y - gsz + Px(2)), ImVec2(br.x - Px(3), br.y - Px(3)), ImVec2(br.x - gsz + Px(2), br.y - Px(3)),
                                                     ImGui::GetColorU32(Alpha(act || hov ? P.accent : P.dim, act || hov ? 0.9f : 0.4f)));
        static POINT grabBR;
        if (ImGui::IsItemActivated()) { POINT sm = ScreenMouse(); RECT wr; GetWindowRect(g_edWnd, &wr); grabBR = { wr.right - sm.x, wr.bottom - sm.y }; }
        if (act) {
            POINT sm = ScreenMouse(); RECT wr; GetWindowRect(g_edWnd, &wr);
            int w = std::max<int>(420, sm.x + grabBR.x - wr.left), h = std::max<int>(400, sm.y + grabBR.y - wr.top);
            SetWindowPos(g_edWnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (ImGui::IsItemDeactivated()) SaveEdWindowRect();
        if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    }
    FileBrowserDraw();          // (a file picked from the editor: drawn here, the game keeps focus)
    ImGui::End();

    UeThemePop();
}

// Draw one of the two windows with its own ImGui context.
static int RenderWindow(ImGuiContext* c, const FontSet& fonts, IDXGISwapChain* swap, ID3D11RenderTargetView*& rtv, UINT& rw, UINT& rh,
                        void (*createRT)(), void (*cleanupRT)(), std::vector<InEvt>& ev, void (*draw)(), const float clear[4], UINT syncInterval) {
    ImGui::SetCurrentContext(c);
    UseFonts(fonts);
    if (rw) { cleanupRT(); swap->ResizeBuffers(0, rw, rh, DXGI_FORMAT_UNKNOWN, 0); rw = rh = 0; createRT(); }
    FeedEvents(ev);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::PushFont(F_body);
    draw();
    ImGui::PopFont();
    int cursor = ImGui::GetMouseCursor();
    ImGui::Render();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swap->Present(syncInterval, 0);   // only the last window waits for the screen refresh (else the editor runs at half speed)
    return cursor;
}

static void RenderOverlay() {
    if (!g_ovVisible || !g_ovCtx) return;
    OvTrack();
    if (!g_ovVisible) return;
    OvPoll();
    bool cap = CaptureActive();
    if (cap) DispatchCapturedInput();
    else { std::lock_guard<std::mutex> lk(g_inMx); g_inQ.clear(); g_flyRMB = false; }
    FlyTick();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    FontSet keep = SaveFonts();
    int curGame = 0, curEd = 0;
    const float none[4] = { 0, 0, 0, 0 };
    if (g_edWnd) {                       // the separate window: only while it holds undocked panels or the Material Editor
        bool want = UeWantEdWindow(), vis = IsWindowVisible(g_edWnd) != 0;
        if (want && !vis) { PlaceEditorWindow(); SetWindowPos(g_edWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE); }
        else if (!want && vis) { SaveEdWindowRect(); ShowWindow(g_edWnd, SW_HIDE); }
    }
    bool edShown = g_edCtx && g_edWnd && IsWindowVisible(g_edWnd) && !IsIconic(g_edWnd);
    if (IsWindowVisible(g_ovHwnd))
        curGame = RenderWindow(g_ovCtx, g_ovFonts, g_ovSwap, g_ovRtv, g_ovResizeW, g_ovResizeH, OvCreateRT, OvCleanupRT, g_evGame, DrawGameLayer, none, edShown ? 0 : 1);
    else g_evGame.clear();
    if (edShown) {
        const float bgc[4] = { P.card.x, P.card.y, P.card.z, 1 };
        curEd = RenderWindow(g_edCtx, g_edFonts, g_edSwap, g_edRtv, g_edResizeW, g_edResizeH, EdCreateRT, EdCleanupRT, g_evEd, DrawEditorWindow, bgc, 1);
    } else { g_evEd.clear(); if (!IsWindowVisible(g_ovHwnd)) Sleep(10); }
    g_edSelectTab = -1;
    ImGui::SetCurrentContext(prev);
    UseFonts(keep);
    // while the game has focus, show the editor's cursor wherever it is
    UpdateCursorWindow(cap && g_ovVisible, g_route == RT_EDITOR ? curEd : g_route == RT_GAME ? curGame : 0);
}

static void OvShutdown() {
    if (g_ovVisible) {   // closing the launcher with the editor open: give the game its controls back
        SetEditingFlag(false);
        Ed("ed_close");
        for (int i = 0; i < 40; i++) { BridgeFlush(); Sleep(25); }
    }
    if (!g_ovCtx) return;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ovCtx);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(g_ovCtx);
    ImGui::SetCurrentContext(prev == g_ovCtx ? g_mainCtx : prev);
    g_ovCtx = nullptr;
    OvCleanupRT();
    if (g_ovSwap) { g_ovSwap->Release(); g_ovSwap = nullptr; }
    StopCapture();
    if (g_ovHwnd) DestroyWindow(g_ovHwnd);
    if (g_edCtx) {
        ImGui::SetCurrentContext(g_edCtx);
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_edCtx); g_edCtx = nullptr;
        ImGui::SetCurrentContext(prev == g_ovCtx || prev == g_edCtx ? g_mainCtx : prev);
    }
    EdCleanupRT();
    if (g_edSwap) { g_edSwap->Release(); g_edSwap = nullptr; }
    if (g_edWnd) DestroyWindow(g_edWnd);
    if (g_curWnd) DestroyWindow(g_curWnd);
}
