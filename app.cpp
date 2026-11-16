// UE4SS Launcher - modern Dear ImGui (DirectX 11) front end with switchable themes.
#include "core.h"
#include "models.h"
#include <set>
#include "icons.h"
#include "bridge.h"
#include "updater.h"
#include "classic.h"
#include <shellapi.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <wincodec.h>
#include <shlobj.h>
#include <functional>
#include <atomic>
#include <map>
#include <algorithm>
#include <cmath>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

void ImAssertLog(const char* expr, const char* file, int line) {
    static int count = 0;
    if (++count > 50) return;
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    FILE* f = _wfopen((std::wstring(tmp) + L"UE4SS-Launcher-errors.log").c_str(), L"a");
    if (f) { fprintf(f, "ImGui check failed: %s (%s:%d)\n", expr, file, line); fclose(f); }
}

#define IDR_FONT_REGULAR 800
#define IDR_FONT_SEMIBOLD 801
#define IDR_FONT_BOLD 802
#define IDR_FONT_ICONS 803
#define IDR_LOGO_RGBA 804   // 256x256 RGBA

// ================================================================ D3D
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;

static void CreateRT() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}
static void CleanupRT() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

// ---- UE4SS logo texture (with mipmaps so it stays crisp when drawn small)
static ID3D11ShaderResourceView* g_logo = nullptr;
static void LoadLogo() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(inst, MAKEINTRESOURCEW(IDR_LOGO_RGBA), RT_RCDATA);
    if (!r || SizeofResource(inst, r) != 256 * 256 * 4) return;
    const void* px = LockResource(LoadResource(inst, r));
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 256; d.MipLevels = 0; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &tex))) return;
    g_ctx->UpdateSubresource(tex, 0, nullptr, px, 256 * 4, 0);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = d.Format; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = (UINT)-1;
    if (SUCCEEDED(g_dev->CreateShaderResourceView(tex, &sv, &g_logo))) g_ctx->GenerateMips(g_logo);
    tex->Release();
}
static bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr))  // no GPU / remote desktop: software renderer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRT();
    return true;
}
static void CleanupDevice() {
    CleanupRT();
    if (g_logo) { g_logo->Release(); g_logo = nullptr; }
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
}

// ================================================================ themes
static ImVec4 Hex(unsigned v, float a = 1.f) { return ImVec4(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a); }
static ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
static ImVec4 Alpha(ImVec4 c, float a) { c.w = a; return c; }
static float Luma(const ImVec4& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

struct Theme { const char* name; unsigned bg, sidebar, card, text, dim, accent, border; bool dark; };
static const Theme kThemes[] = {
    { "Midnight",         0x0F1117, 0x0A0C11, 0x171A23, 0xE6E8EE, 0x8A90A2, 0x6C8CFF, 0x252A36, true  },
    { "Graphite",         0x18181B, 0x111113, 0x222226, 0xEDEDEF, 0x8E8E96, 0x3B82F6, 0x2E2E33, true  },
    { "Light",            0xF3F4F6, 0xFFFFFF, 0xFFFFFF, 0x161A22, 0x6B7280, 0x2563EB, 0xE2E5EA, false },
    { "Dracula",          0x282A36, 0x21222C, 0x343746, 0xF8F8F2, 0xA3A8C8, 0xBD93F9, 0x44475A, true  },
    { "Nord",             0x2E3440, 0x272C36, 0x3B4252, 0xECEFF4, 0xA3ADC2, 0x88C0D0, 0x4C566A, true  },
    { "Catppuccin Mocha", 0x1E1E2E, 0x181825, 0x313244, 0xCDD6F4, 0xA6ADC8, 0xCBA6F7, 0x45475A, true  },
    { "Forest",           0x111A15, 0x0C130F, 0x1A2620, 0xE3EDE6, 0x8FA597, 0x4ADE80, 0x26362D, true  },
    { "Crimson",          0x141012, 0x0E0B0C, 0x1F181B, 0xF0E6E8, 0xA3949A, 0xF43F5E, 0x30252A, true  },
    { "Paper",            0xF5F0E6, 0xFBF8F1, 0xFFFDF8, 0x3B3328, 0x857A6A, 0xD97706, 0xE6DDCC, false },
    { "Ocean",            0x0B1622, 0x08111A, 0x11202F, 0xE2ECF5, 0x8BA3B8, 0x22D3EE, 0x1C3044, true  },
};
static const int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);
static const unsigned kAccentPresets[] = { 0x6C8CFF, 0x3B82F6, 0x22D3EE, 0x4ADE80, 0xFACC15, 0xF97316, 0xF43F5E, 0xEC4899, 0xBD93F9 };

struct Palette { ImVec4 bg, sidebar, card, card2, input, text, dim, accent, accentHover, accentActive, onAccent, border, ok, warn, err; bool dark; };
static Palette P;

// user appearance settings
static int g_theme = 0;
static bool g_customAccent = false;
static float g_accent[3] = { 0.42f, 0.55f, 1.0f };
static int g_scalePct = 100;
static float g_rounding = 10.f;
static bool g_rgbBorder = true;         // a moving rainbow outline round the launcher window
static int g_rgbSpeed = 100, g_rgbWidth = 3;
static float g_dpiScale = 1.f;
static bool g_fontsDirty = true, g_styleDirty = true;

static float S() { return g_dpiScale * g_scalePct / 100.f; }

static void BuildPalette() {
    const Theme& t = kThemes[g_theme];
    P.dark = t.dark;
    P.bg = Hex(t.bg); P.sidebar = Hex(t.sidebar); P.card = Hex(t.card);
    P.text = Hex(t.text); P.dim = Hex(t.dim); P.border = Hex(t.border);
    P.accent = g_customAccent ? ImVec4(g_accent[0], g_accent[1], g_accent[2], 1) : Hex(t.accent);
    P.card2 = Mix(P.card, P.text, t.dark ? 0.06f : 0.04f);
    P.input = t.dark ? Mix(P.bg, P.card, 0.55f) : Mix(P.bg, P.card, 0.35f);
    P.accentHover = Mix(P.accent, t.dark ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1), 0.12f);
    P.accentActive = Mix(P.accent, t.dark ? ImVec4(0, 0, 0, 1) : ImVec4(0, 0, 0, 1), 0.15f);
    P.onAccent = Luma(P.accent) > 0.6f ? Hex(0x101114) : Hex(0xFFFFFF);
    P.ok = t.dark ? Hex(0x4ADE80) : Hex(0x16A34A);
    P.warn = t.dark ? Hex(0xFBBF24) : Hex(0xD97706);
    P.err = t.dark ? Hex(0xF87171) : Hex(0xDC2626);
}

static void ApplyStyle() {
    BuildPalette();
    ImGuiStyle st;
    float s = S(), r = g_rounding;
    st.WindowPadding = ImVec2(0, 0);
    st.WindowRounding = 0; st.WindowBorderSize = 0;
    st.ChildRounding = r + 2; st.ChildBorderSize = 1;
    st.FrameRounding = r * 0.8f; st.FrameBorderSize = 0;
    st.PopupRounding = r; st.PopupBorderSize = 1;
    st.GrabRounding = r * 0.8f; st.ScrollbarRounding = r; st.TabRounding = r * 0.7f;
    st.FramePadding = ImVec2(12, 8);
    st.ItemSpacing = ImVec2(10, 10);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.CellPadding = ImVec2(10, 7);
    st.ScrollbarSize = 10;
    st.GrabMinSize = 12;
    st.SelectableTextAlign = ImVec2(0, 0.5f);
    st.ScaleAllSizes(s);

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = P.text;
    c[ImGuiCol_TextDisabled] = P.dim;
    c[ImGuiCol_WindowBg] = P.bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = P.card;
    c[ImGuiCol_Border] = P.border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = P.input;
    c[ImGuiCol_FrameBgHovered] = Mix(P.input, P.text, 0.05f);
    c[ImGuiCol_FrameBgActive] = Mix(P.input, P.text, 0.08f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = P.card;
    c[ImGuiCol_MenuBarBg] = P.card;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = P.border;
    c[ImGuiCol_ScrollbarGrabHovered] = Mix(P.border, P.text, 0.15f);
    c[ImGuiCol_ScrollbarGrabActive] = Mix(P.border, P.text, 0.25f);
    c[ImGuiCol_CheckMark] = P.accent;
    c[ImGuiCol_SliderGrab] = P.accent;
    c[ImGuiCol_SliderGrabActive] = P.accentHover;
    c[ImGuiCol_Button] = P.card2;
    c[ImGuiCol_ButtonHovered] = Mix(P.card2, P.text, 0.07f);
    c[ImGuiCol_ButtonActive] = Mix(P.card2, P.text, 0.12f);
    c[ImGuiCol_Header] = Alpha(P.accent, 0.22f);
    c[ImGuiCol_HeaderHovered] = Alpha(P.accent, 0.12f);
    c[ImGuiCol_HeaderActive] = Alpha(P.accent, 0.30f);
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = P.border;
    c[ImGuiCol_ResizeGrip] = c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableHeaderBg] = P.card;
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = P.border;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = Alpha(P.text, 0.025f);
    c[ImGuiCol_TextSelectedBg] = Alpha(P.accent, 0.35f);
    c[ImGuiCol_NavCursor] = P.accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, P.dark ? 0.55f : 0.30f);
    ImGui::GetStyle() = st;

    // match the Windows title bar to the theme (Windows 10 20H1+/11; ignored elsewhere)
    BOOL dark = P.dark;
    DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof dark);
    auto ref = [](ImVec4 v) { return (COLORREF)RGB((int)(v.x * 255), (int)(v.y * 255), (int)(v.z * 255)); };
    COLORREF cap = ref(P.sidebar), txt = ref(P.text), brd = ref(P.border);
    DwmSetWindowAttribute(g_hwnd, 35, &cap, sizeof cap);
    DwmSetWindowAttribute(g_hwnd, 36, &txt, sizeof txt);
    DwmSetWindowAttribute(g_hwnd, 34, &brd, sizeof brd);
}

// ================================================================ fonts
static ImFont *F_body, *F_med, *F_small, *F_h2, *F_title;
static ImFont* AddFont(int resId, float px, bool icons) {
    ImGuiIO& io = ImGui::GetIO();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(inst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    ImFontConfig cfg; cfg.FontDataOwnedByAtlas = false; cfg.OversampleH = 2;
    ImFont* f = r ? io.Fonts->AddFontFromMemoryTTF(LockResource(LoadResource(inst, r)), (int)SizeofResource(inst, r), px, &cfg, io.Fonts->GetGlyphRangesDefault())
                  : io.Fonts->AddFontDefault();
    if (icons) {
        HRSRC ri = FindResourceW(inst, MAKEINTRESOURCEW(IDR_FONT_ICONS), RT_RCDATA);
        if (ri) {
            static const ImWchar ranges[] = { 0xe000, 0xf8ff, 0 };
            ImFontConfig ic; ic.MergeMode = true; ic.FontDataOwnedByAtlas = false; ic.PixelSnapH = true;
            ic.GlyphMinAdvanceX = px; ic.GlyphOffset = ImVec2(0, px * 0.04f);
            io.Fonts->AddFontFromMemoryTTF(LockResource(LoadResource(inst, ri)), (int)SizeofResource(inst, ri), px * 0.85f, &ic, ranges);
        }
    }
    return f;
}
static void BuildFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    float s = S();
    F_body = AddFont(IDR_FONT_REGULAR, std::round(15 * s), true);
    F_med = AddFont(IDR_FONT_SEMIBOLD, std::round(15 * s), true);
    F_small = AddFont(IDR_FONT_REGULAR, std::round(13 * s), true);
    F_h2 = AddFont(IDR_FONT_SEMIBOLD, std::round(18 * s), true);
    F_title = AddFont(IDR_FONT_BOLD, std::round(26 * s), true);
    io.Fonts->Build();
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

// ================================================================ app state
enum Page { PG_PLAY, PG_MODS, PG_LEVELS, PG_FREECAM, PG_EDITOR, PG_LOOK, PG_LOG, PG_PLAYER, PG_CHEATS, PG_MODELS };

static int g_page = PG_PLAY;
static std::vector<ProcInfo> g_procs;
static DWORD g_selPid = 0;
static char g_filter[128] = "";
static bool g_ueOnly = true;
static std::vector<ModInfo> g_mods;
static bool g_optConsole = false, g_optGui = false;
static bool g_runtimeMissing = false;
static std::vector<std::function<void()>> g_deferred;   // run after the frame (file dialogs etc.)
static std::wstring g_pendingPak;
static bool g_askReset = false, g_askClose = false, g_reallyClose = false;
static bool g_animating = false;
static int g_framesLeft = 3;
static ULONGLONG g_lastWatch = 0, g_lastProcRefresh = 0;
static bool g_inSizeMove = false;

// level browser state
static std::vector<LevelEntry> g_levels;
static std::string g_levelMethod;
static LevelSource g_levelSrc = LS_NONE;
static BridgeStatus g_bridge;
static int g_pendingCmd = 0;
static std::string g_pendingVerb, g_pendingArg;
static ULONGLONG g_pendingSince = 0, g_lastBridgePoll = 0;
static char g_levelFilter[128] = "";
static bool g_hideEngine = true;
static bool g_mainOnly = true;
static std::string g_confirmSub;   // sub-level waiting for "open anyway?"
static FreecamConfig g_fc;
static ULONGLONG g_speedChangedAt = 0;   // ignore the game's speed echo briefly after a local change
static char g_manualLevel[260] = "";
static bool g_autoScanned = false;
static void ReloadLevels();
// shared with the in-game editor overlay (overlay.inc)
static std::atomic<DWORD> g_gamePid{ 0 };
static bool g_actorsRequested = false;
static float g_defWalk = -1, g_defJump = -1, g_defGravity = -1, g_defFov = -1;
static bool g_ovVisible = false;
static void OvHide(bool refocusGame = true);
// editor hotkeys (ImGuiKey values), changeable on the Editor page
struct EdKeys { int select = ImGuiKey_Q, move = ImGuiKey_W, rotate = ImGuiKey_E, scale = ImGuiKey_R, del = ImGuiKey_Delete, hide = ImGuiKey_H, deselect = ImGuiKey_Escape;
                int walk = ImGuiKey_F6, fly = ImGuiKey_F7, ghost = ImGuiKey_F8, snapGround = ImGuiKey_End, snapOnto = ImGuiKey_G; };
static EdKeys g_edKeys;
enum GizmoMode { GZ_SELECT = 0, GZ_MOVE, GZ_ROTATE, GZ_SCALE };
static int g_gzMode = GZ_MOVE;
static bool g_edNoPause = true;
static bool g_modelPicsSetting = true;   // Models tab shows pictures
static int g_edWinRect[4] = { 0, 0, 0, 0 };   // editor window: x, y, width, height (0 = place it next to the game)
// Unreal-style editor layout: panels docked around the game (or undocked into their own window)
enum { UP_OUTLINER, UP_DETAILS, UP_PLACE, UP_CONTENT, UP_COUNT };
static bool g_upOpen[UP_COUNT] = { true, true, true, true }, g_upFloat[UP_COUNT] = { false, false, false, false };
static float g_upLeftW = 250, g_upRightW = 360, g_upBottomH = 240, g_upSplit = 0.42f;   // (unscaled pixels; split = Outliner's share)
static bool g_ueFit = true;   // fit the game window into the viewport between the panels (like Unreal's viewport)
static int g_snapOn[3] = { 0, 0, 0 }; static float g_snapStep[3] = { 10, 15, 0.25f };   // grid snapping: move (cm), rotate (deg), scale
static bool g_possTP = true, g_possAssist = true;
// camera view for any game: 0 = the game's own, 1 = third person, 2 = first person
static int g_viewMode = 0; static float g_viewFov = 90, g_viewDist = 320, g_viewShoulder = 45, g_viewHeight = 10; static bool g_viewBob = true, g_viewFace = true;
static void SendViewMode() {
    char b[200];
    snprintf(b, sizeof b, "mode=%s fov=%.0f dist=%.0f shoulder=%.0f height=%.0f bob=%d face=%d", g_viewMode == 2 ? "fp" : g_viewMode == 1 ? "tp" : "off",
             g_viewFov, g_viewDist, g_viewShoulder, g_viewHeight, g_viewBob ? 1 : 0, g_viewFace ? 1 : 0);
    BridgeSend("ed_viewmode", b);
}   // playing as another character: follow camera, drive it ourselves
static bool g_edFreecam = true;   // the editor flies a freecam (controller / right mouse + WASD)
static float g_flySpeed = 1.0f;   // editing camera speed
static bool g_updAuto = true;      // check GitHub for a newer UE4SS at start-up and install it
static int g_updChannel = 0;       // 0 = experimental (newest), 1 = stable releases   // keep the game focused (running) while the editor is open

static void Defer(std::function<void()> f) { g_deferred.push_back(std::move(f)); }

static void SaveSettings() {
    auto W = [](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"Launcher", k, v.c_str(), g_iniPath.c_str()); };
    W(L"GameExe", g_gameExe);
    W(L"AutoInject", g_autoInject ? L"1" : L"0");
    W(L"UnrealOnly", g_ueOnly ? L"1" : L"0");
    W(L"Theme", U2W(kThemes[g_theme].name));
    W(L"CustomAccent", g_customAccent ? L"1" : L"0");
    wchar_t hex[16]; swprintf(hex, 16, L"%02X%02X%02X", (int)(g_accent[0] * 255 + .5f), (int)(g_accent[1] * 255 + .5f), (int)(g_accent[2] * 255 + .5f));
    W(L"Accent", hex);
    W(L"Scale", std::to_wstring(g_scalePct));
    W(L"Rounding", std::to_wstring((int)g_rounding));
    W(L"RgbBorder", g_rgbBorder ? L"1" : L"0");
    W(L"ModelMaxTris", std::to_wstring(ModelMaxTriangles()));
    W(L"RgbSpeed", std::to_wstring(g_rgbSpeed));
    W(L"RgbWidth", std::to_wstring(g_rgbWidth));
    W(L"FreecamEnabled", g_fc.enabled ? L"1" : L"0");
    W(L"FreecamKey", U2W(g_fc.key));
    W(L"FreecamMode", U2W(g_fc.mode));
    W(L"FreecamHideText", g_fc.hideText ? L"1" : L"0");
    W(L"FreecamSpeed", std::to_wstring((int)(g_fc.speed * 100 + 0.5f)));
    WriteFreecamConfig(g_fc);
    W(L"EdKeySelect", std::to_wstring(g_edKeys.select)); W(L"EdKeyMove", std::to_wstring(g_edKeys.move));
    W(L"EdKeyRotate", std::to_wstring(g_edKeys.rotate)); W(L"EdKeyScale", std::to_wstring(g_edKeys.scale));
    W(L"EdKeyDelete", std::to_wstring(g_edKeys.del)); W(L"EdKeyHide", std::to_wstring(g_edKeys.hide));
    W(L"EdKeyDeselect", std::to_wstring(g_edKeys.deselect));
    W(L"EdKeyWalk", std::to_wstring(g_edKeys.walk)); W(L"EdKeyFly", std::to_wstring(g_edKeys.fly)); W(L"EdKeyGhost", std::to_wstring(g_edKeys.ghost));
    W(L"EdKeySnapGround", std::to_wstring(g_edKeys.snapGround)); W(L"EdKeySnapOnto", std::to_wstring(g_edKeys.snapOnto));
    W(L"EdFreecam", g_edFreecam ? L"1" : L"0");
    W(L"ViewMode", std::to_wstring(g_viewMode)); W(L"ViewFov", std::to_wstring((int)g_viewFov)); W(L"ViewDist", std::to_wstring((int)g_viewDist));
    W(L"ViewShoulder", std::to_wstring((int)g_viewShoulder)); W(L"ViewHeight", std::to_wstring((int)g_viewHeight)); W(L"ViewBob", g_viewBob ? L"1" : L"0"); W(L"ViewFace", g_viewFace ? L"1" : L"0");
    W(L"PossThirdPerson", g_possTP ? L"1" : L"0"); W(L"PossAssist", g_possAssist ? L"1" : L"0"); W(L"EdFlySpeed", std::to_wstring((int)(g_flySpeed * 100 + 0.5f))); W(L"EdGizmoMode", std::to_wstring(g_gzMode)); W(L"EdNoPause", g_edNoPause ? L"1" : L"0");
    { wchar_t b[64]; swprintf(b, 64, L"%d,%d,%d,%d", g_edWinRect[0], g_edWinRect[1], g_edWinRect[2], g_edWinRect[3]); W(L"EdWindow3", b); }
    {   wchar_t b[200]; int o = 0, f = 0; for (int i = 0; i < UP_COUNT; i++) { o |= g_upOpen[i] << i; f |= g_upFloat[i] << i; }
        swprintf(b, 200, L"%d,%d,%.0f,%.0f,%.0f,%.3f,%d,%d,%d,%g,%g,%g,%d", o, f, g_upLeftW, g_upRightW, g_upBottomH, g_upSplit,
                 g_snapOn[0], g_snapOn[1], g_snapOn[2], g_snapStep[0], g_snapStep[1], g_snapStep[2], (int)g_ueFit);
        W(L"UELayout", b); }
    W(L"UE4SSAutoUpdate", g_updAuto ? L"1" : L"0");
    W(L"ModelPictures", g_modelPicsSetting ? L"1" : L"0"); W(L"UE4SSChannel", g_updChannel ? L"stable" : L"experimental");
}
static void LoadSettings() {
    wchar_t buf[MAX_PATH * 2];
    auto G = [&](const wchar_t* k, const wchar_t* d) { GetPrivateProfileStringW(L"Launcher", k, d, buf, MAX_PATH * 2, g_iniPath.c_str()); return std::wstring(buf); };
    g_gameExe = G(L"GameExe", L"");
    g_autoInject = G(L"AutoInject", L"0") == L"1";
    g_ueOnly = G(L"UnrealOnly", L"1") == L"1";
    std::string tn = W2U(G(L"Theme", L"Midnight"));
    for (int i = 0; i < kThemeCount; i++) if (tn == kThemes[i].name) g_theme = i;
    g_customAccent = G(L"CustomAccent", L"0") == L"1";
    unsigned a = wcstoul(G(L"Accent", L"6C8CFF").c_str(), nullptr, 16);
    g_accent[0] = ((a >> 16) & 255) / 255.f; g_accent[1] = ((a >> 8) & 255) / 255.f; g_accent[2] = (a & 255) / 255.f;
    g_scalePct = std::clamp(_wtoi(G(L"Scale", L"100").c_str()), 80, 150);
    g_rounding = (float)std::clamp(_wtoi(G(L"Rounding", L"10").c_str()), 0, 16);
    g_rgbBorder = G(L"RgbBorder", L"1") == L"1";
    ModelSetMaxTriangles(_wtoi(G(L"ModelMaxTris", L"1000000").c_str()));
    g_rgbSpeed = std::clamp(_wtoi(G(L"RgbSpeed", L"100").c_str()), 0, 400);
    g_rgbWidth = std::clamp(_wtoi(G(L"RgbWidth", L"3").c_str()), 1, 12);
    g_fc.enabled = G(L"FreecamEnabled", L"1") == L"1";
    g_fc.key = W2U(G(L"FreecamKey", L"INS"));
    g_fc.mode = W2U(G(L"FreecamMode", L"auto"));
    g_fc.hideText = G(L"FreecamHideText", L"1") == L"1";
    g_fc.speed = std::clamp(_wtoi(G(L"FreecamSpeed", L"100").c_str()), 10, 500) / 100.f;
    auto key = [&](const wchar_t* k, int& v) { int x = _wtoi(G(k, L"0").c_str()); if (x >= ImGuiKey_NamedKey_BEGIN && x < ImGuiKey_NamedKey_END) v = x; };
    key(L"EdKeySelect", g_edKeys.select); key(L"EdKeyMove", g_edKeys.move); key(L"EdKeyRotate", g_edKeys.rotate); key(L"EdKeyScale", g_edKeys.scale);
    key(L"EdKeyDelete", g_edKeys.del); key(L"EdKeyHide", g_edKeys.hide); key(L"EdKeyDeselect", g_edKeys.deselect);
    auto key2 = [&](const wchar_t* k, int& v) { std::wstring s = G(k, L""); if (s.empty()) return; int x = _wtoi(s.c_str()); if (x == 0 || (x >= ImGuiKey_NamedKey_BEGIN && x < ImGuiKey_NamedKey_END)) v = x; };
    key2(L"EdKeyWalk", g_edKeys.walk); key2(L"EdKeyFly", g_edKeys.fly); key2(L"EdKeyGhost", g_edKeys.ghost);
    key2(L"EdKeySnapGround", g_edKeys.snapGround); key2(L"EdKeySnapOnto", g_edKeys.snapOnto);
    g_edFreecam = G(L"EdFreecam", L"1") == L"1";
    g_viewMode = _wtoi(G(L"ViewMode", L"0").c_str()); g_viewFov = (float)_wtoi(G(L"ViewFov", L"90").c_str()); g_viewDist = (float)_wtoi(G(L"ViewDist", L"320").c_str());
    g_viewShoulder = (float)_wtoi(G(L"ViewShoulder", L"45").c_str()); g_viewHeight = (float)_wtoi(G(L"ViewHeight", L"10").c_str());
    g_viewBob = G(L"ViewBob", L"1") == L"1"; g_viewFace = G(L"ViewFace", L"1") == L"1";
    if (g_viewMode < 0 || g_viewMode > 2) g_viewMode = 0; if (g_viewFov < 60 || g_viewFov > 130) g_viewFov = 90;
    g_possTP = G(L"PossThirdPerson", L"1") == L"1"; g_possAssist = G(L"PossAssist", L"1") == L"1";
    g_flySpeed = std::clamp(_wtoi(G(L"EdFlySpeed", L"100").c_str()), 5, 3000) / 100.f;
    g_gzMode = std::clamp(_wtoi(G(L"EdGizmoMode", L"1").c_str()), 0, 3);
    g_edNoPause = G(L"EdNoPause", L"1") == L"1";
    swscanf(G(L"EdWindow3", L"0,0,0,0").c_str(), L"%d,%d,%d,%d", &g_edWinRect[0], &g_edWinRect[1], &g_edWinRect[2], &g_edWinRect[3]);
    {   int o = 15, f = 0, fit = 1;
        swscanf(G(L"UELayout", L"15,0,250,360,240,0.42,0,0,0,10,15,0.25,1").c_str(), L"%d,%d,%f,%f,%f,%f,%d,%d,%d,%f,%f,%f,%d", &o, &f, &g_upLeftW, &g_upRightW, &g_upBottomH, &g_upSplit,
               &g_snapOn[0], &g_snapOn[1], &g_snapOn[2], &g_snapStep[0], &g_snapStep[1], &g_snapStep[2], &fit);
        g_ueFit = fit != 0;
        for (int i = 0; i < UP_COUNT; i++) { g_upOpen[i] = (o >> i) & 1; g_upFloat[i] = (f >> i) & 1; } }
    g_updAuto = G(L"UE4SSAutoUpdate", L"1") == L"1";
    g_modelPicsSetting = G(L"ModelPictures", L"1") == L"1";
    g_updChannel = G(L"UE4SSChannel", L"experimental") == L"stable" ? 1 : 0;
}

static void RefreshProcs() { g_procs = EnumProcs(); g_lastProcRefresh = GetTickCount64(); }
static void RefreshMods() { g_mods = ReadMods(); g_optConsole = IniGetBool(L"ConsoleEnabled"); g_optGui = IniGetBool(L"GuiConsoleEnabled"); }

// ================================================================ widgets
static float Px(float v) { return v * S(); }

static bool ToggleSwitch(const char* id, bool* v) {
    float h = Px(22), w = Px(40);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool changed = false;
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID key = ImGui::GetItemID();
    float target = *v ? 1.f : 0.f, t = st->GetFloat(key, target);
    t += (target - t) * std::min(1.f, ImGui::GetIO().DeltaTime * 16.f);
    if (std::fabs(target - t) > 0.002f) g_animating = true; else t = target;
    st->SetFloat(key, t);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 off = Mix(P.card2, P.text, 0.12f);
    ImVec4 col = Mix(off, P.accent, t);
    if (ImGui::IsItemHovered()) col = Mix(col, P.text, 0.06f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(col), h * 0.5f);
    float r = h * 0.5f - Px(3);
    ImVec4 knob = (t > 0.5f && Luma(P.accent) > 0.75f) ? Hex(0x101114) : (P.dark || t > 0.5f ? Hex(0xF4F5F8) : Hex(0xFFFFFF));
    dl->AddCircleFilled(ImVec2(p.x + h * 0.5f + t * (w - h), p.y + h * 0.5f), r, ImGui::GetColorU32(knob), 24);
    return changed;
}

static bool AccentButton(const char* label, ImVec2 size = ImVec2(0, 0), ImFont* font = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Button, P.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, P.accentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, P.accentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, P.onAccent);
    if (font) ImGui::PushFont(font);
    bool r = ImGui::Button(label, size);
    if (font) ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return r;
}
static bool GhostButton(const char* label, ImVec2 size = ImVec2(0, 0), ImVec4* tint = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Alpha(tint ? *tint : P.text, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Alpha(tint ? *tint : P.text, 0.14f));
    if (tint) ImGui::PushStyleColor(ImGuiCol_Text, *tint);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(tint ? 4 : 3);
    return r;
}

static void BeginCard(const char* id, float height = 0, float padX = 18, float padY = 16) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(padX), Px(padY)));
    ImGuiChildFlags f = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (height == 0) f |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild(id, ImVec2(0, height), f, height == 0 ? ImGuiWindowFlags_NoScrollbar : 0);
}
static void EndCard() { ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor(); }

static void Pill(const char* text, ImVec4 col) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float padX = Px(10), padY = Px(4);
    ImVec2 sz(ts.x + padX * 2, ts.y + padY * 2);
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::GetColorU32(Alpha(col, 0.16f)), sz.y * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + padX, p.y + padY), ImGui::GetColorU32(col), text);
    ImGui::Dummy(sz);
}

// Shortens long paths in the middle: C:\Program Files\...\Win64\Game.exe
static std::string FitPath(const std::string& s, float maxW) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW || s.size() < 8) return s;
    size_t keepR = s.size() / 2, keepL = s.size() / 2;
    std::string out;
    while (keepL > 3 && keepR > 3) {
        out = s.substr(0, keepL) + "..." + s.substr(s.size() - keepR);
        if (ImGui::CalcTextSize(out.c_str()).x <= maxW) return out;
        if (keepL > keepR) keepL--; else keepR--;
    }
    return out;
}

static void IconBox(const char* icon, ImVec4 col, float size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ImGui::GetColorU32(Alpha(col, 0.16f)), g_rounding * S());
    ImGui::PushFont(F_h2);
    ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f), ImGui::GetColorU32(col), icon);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(size, size));
}

static void PageHeader(const char* title, const char* sub) {
    ImGui::PushFont(F_title); ImGui::TextUnformatted(title); ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted(sub); ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, Px(6)));
}

static void SectionLabel(const char* t) {
    ImGui::PushFont(F_small);
    ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
    std::string up = t; for (auto& c : up) c = (char)toupper((unsigned char)c);
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor(); ImGui::PopFont();
}

static ImVec4 LevelColor(int lvl) { return lvl == LOG_OK ? P.ok : lvl == LOG_WARN ? P.warn : lvl == LOG_ERR ? P.err : P.dim; }
static const char* LevelIcon(int lvl) { return lvl == LOG_OK ? ICON_CIRCLE_CHECK : lvl == LOG_WARN ? ICON_WARN : lvl == LOG_ERR ? ICON_CIRCLE_XMARK : ICON_INFO; }

static void LogLines(int maxLines, bool wrap) {
    std::lock_guard<std::mutex> lk(g_logMx);
    int start = maxLines > 0 ? std::max(0, (int)g_logs.size() - maxLines) : 0;
    for (int i = (int)g_logs.size() - 1; i >= start; i--) {
        auto& l = g_logs[i];
        ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(l.level));
        ImGui::TextUnformatted(LevelIcon(l.level));
        ImGui::PopStyleColor();
        ImGui::SameLine(0, Px(8));
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted(l.time.c_str()); ImGui::PopStyleColor();
        ImGui::SameLine(0, Px(10));
        if (wrap) ImGui::TextWrapped("%s", l.text.c_str()); else ImGui::TextUnformatted(l.text.c_str());
    }
}

// ================================================================ file pickers
static std::wstring PickFile(const wchar_t* filter, const wchar_t* title, const std::wstring& initial = L"") {
    wchar_t file[MAX_PATH * 2]{};
    wcsncpy(file, initial.c_str(), MAX_PATH * 2 - 1);
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = g_hwnd; ofn.lpstrFilter = filter; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = title; ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&ofn) ? file : L"";
}
static std::wstring PickSaveFile(const wchar_t* filter, const wchar_t* title, const std::wstring& name, const wchar_t* ext) {
    wchar_t file[MAX_PATH * 2]{};
    wcsncpy(file, name.c_str(), MAX_PATH * 2 - 1);
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = g_hwnd; ofn.lpstrFilter = filter; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH * 2; ofn.lpstrDefExt = ext;
    ofn.lpstrTitle = title; ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&ofn) ? file : L"";
}
static std::wstring PickFolder(const wchar_t* title) {
    BROWSEINFOW bi{}; bi.hwndOwner = g_hwnd; bi.lpszTitle = title; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";
    wchar_t path[MAX_PATH * 2]; SHGetPathFromIDListW(pidl, path); CoTaskMemFree(pidl);
    return path;
}

static void ChooseGame() {
    std::wstring f = PickFile(L"Game (*.exe)\0*.exe\0", L"Choose the game's .exe", g_gameExe);
    if (f.empty()) return;
    g_gameExe = f; InvalidateTarget();
    std::wstring t = TargetExe();
    ReloadLevels();
    SaveSettings();
    if (Lower(t) != Lower(f)) Log(L"Found the actual game exe: " + NameOf(t) + L". UE4SS will be loaded into that.", LOG_OK);
    else if (int cg = ClassicGen(t)) {
        Log(L"Game set: " + ClassicGameName(t) + L". This is an " + U2W(ClassicGenName(cg)) + L" game; UE4SS only works with Unreal Engine 4 and 5, so mods and the in-game tools can't load into it.", LOG_WARN);
    }
    else if (IsUnityGame(t)) {
        Log(L"Game set: " + StemOf(t) + L". This is a Unity game; this launcher is for Unreal Engine games only.", LOG_WARN);
    }
    else if (!LooksUnreal(NameOf(t), t)) Log(L"This doesn't look like an Unreal Engine game exe (usually <Name>-Win64-Shipping.exe).", LOG_WARN);
    else Log(L"Game set: " + GameDisplayName(), LOG_OK);
}
static void InstallModFile() {
    std::wstring f = PickFile(L"Mod files (*.zip;*.pak;*.lua)\0*.zip;*.pak;*.lua\0All files\0*.*\0", L"Choose a mod to install");
    if (f.empty()) return;
    std::wstring ext = ExtOf(f);
    if (ext == L".pak") { if (Lower(f).find(L"logicmods") != std::wstring::npos) InstallPak(f, true); else g_pendingPak = f; }
    else if (ext == L".lua") InstallLua(f);
    else if (ext == L".zip") InstallZipAsync(f);
    else Log(L"Unsupported file type. Use a .zip, .pak or .lua file, or Add folder.", LOG_WARN);
}

// ================================================================ level bridge
static void ReloadLevels() { g_levelSrc = LoadLevels(g_levels, g_levelMethod); }

static void SendBridge(const std::string& verb, const std::string& arg = "") {
    if (!g_bridge.connected) { Log(L"The game isn't connected. Launch it with mods from the Play page first.", LOG_WARN); return; }
    int id = BridgeSend(verb, arg);
    if (!id) { Log(L"Couldn't send the command to the game.", LOG_ERR); return; }
    g_pendingCmd = id; g_pendingVerb = verb; g_pendingArg = arg; g_pendingSince = GetTickCount64();
    if (verb == "list") Log(L"Scanning the game for levels...");
    else if (verb == "open") Log(L"Opening " + U2W(arg) + L"...");
    else if (verb == "reload") Log(L"Reloading the current level...");
    else if (verb == "freecam") Log(L"Toggling freecam...");
    else if (verb == "tp") Log(L"Teleporting the player to the camera...");
}

// Make the launcher follow the game it's actually connected to (e.g. after loading mods into a running game).
static void UpdateGamePid() {
    std::wstring exe = FindInjectedGameExe();
    if (exe.empty()) exe = TargetExe();
    g_gamePid = FindProcessIdByPath(exe);
}
static void AdoptConnectedGame() {
    std::wstring exe = FindInjectedGameExe();
    if (exe.empty()) return;
    std::wstring cur = TargetExe();
    if (!cur.empty() && Lower(cur) == Lower(exe)) return;
    g_gameExe = exe; InvalidateTarget();
    SaveSettings();
    Log(L"Connected game set to " + GameDisplayName() + L".", LOG_OK);
    ReloadLevels();
}

// ---- finding the game's AES key in the running game (after injecting), in the background
static std::wstring AesSettingKey(); static void AesSyncNow();
static std::atomic<int> g_keyHunt{ 0 };      // 0 idle, 1 searching, 2 finished (result waiting)
static std::mutex g_keyMx; static std::string g_keyFound, g_keyMsg; static bool g_keyOk = false; static std::wstring g_keyGame;
static bool g_keyHuntedThisRun = false;
static void StartKeyHunt(bool userAsked) {
    DWORD pid = g_gamePid; std::wstring paks = PaksDir();
    if (g_keyHunt == 1 || !pid || paks.empty()) { if (userAsked && !pid) Log(L"Start the game from the launcher first: the key is found in the running game.", LOG_WARN); return; }
    AesSyncNow();
    g_keyHunt = 1; std::wstring game = GameDisplayName();
    if (userAsked) Log(L"Looking for " + game + L"'s AES key in the running game...", LOG_INFO);
    std::thread([pid, paks, game, userAsked] {
        std::string k, m; bool ok = ModelFindAesKey(pid, paks, k, m);
        { std::lock_guard<std::mutex> lk(g_keyMx); g_keyFound = k; g_keyMsg = m; g_keyOk = ok; g_keyGame = game; }
        g_keyHunt = 2; g_framesLeft = 3;
        (void)userAsked;
    }).detach();
}
static void PollKeyHunt() {
    if (g_keyHunt != 2) return;
    std::string k, m; bool ok; std::wstring game;
    { std::lock_guard<std::mutex> lk(g_keyMx); k = g_keyFound; m = g_keyMsg; ok = g_keyOk; game = g_keyGame; }
    g_keyHunt = 0;
    if (ok && !k.empty() && game == GameDisplayName()) {
        wchar_t cur[200] = L""; GetPrivateProfileStringW(L"Launcher", AesSettingKey().c_str(), L"", cur, 200, g_iniPath.c_str());
        bool isNew = Lower(cur) != Lower(U2W(k));
        WritePrivateProfileStringW(L"Launcher", AesSettingKey().c_str(), U2W(k).c_str(), g_iniPath.c_str());
        AesSyncNow();
        if (isNew) Log(L"Found " + game + L"'s AES key in the running game and saved it. Exporting models from it now works.", LOG_OK);
    } else if (!ok) Log(U2W(m), LOG_WARN);
}
static void PollBridge() {
    PollKeyHunt();
    BridgeStatus prev = g_bridge;
    g_bridge = GetBridgeStatus();
    static ULONGLONG lastPid = 0;
    if (g_bridge.connected && (!g_gamePid || GetTickCount64() - lastPid > 5000)) { lastPid = GetTickCount64(); UpdateGamePid(); }
    if (g_bridge.connected && g_gamePid && !g_keyHuntedThisRun) { g_keyHuntedThisRun = true; StartKeyHunt(false); }   // the game is injected: find its key
    if (g_bridge.connected && !prev.connected) {
        Log(L"Connected to the game." + (g_bridge.currentMap.empty() ? std::wstring() : L" Current level: " + U2W(g_bridge.currentMap)), LOG_OK);
        if (g_status != ST_INJECTED) g_status = ST_INJECTED;
        AdoptConnectedGame();
        if (g_viewMode != 0) SendViewMode();      // first / third person stays on between game starts
        UpdateGamePid();
        BridgeCacheLevels();          // keep any scan the game already did
        ReloadLevels();
        if (!g_autoScanned && g_levels.empty()) { g_autoScanned = true; SendBridge("list"); }
    } else if (!g_bridge.connected && prev.connected) {
        g_gamePid = 0; g_actorsRequested = false; g_defWalk = g_defJump = g_defGravity = g_defFov = -1; g_keyHuntedThisRun = false;
        OvHide(false);
        Log(L"The game closed or stopped responding.", LOG_WARN);
        g_status = ST_IDLE; g_autoScanned = false;
    } else if (g_bridge.connected && !g_bridge.currentMap.empty() && g_bridge.currentMap != prev.currentMap && !prev.currentMap.empty())
        Log(L"Level loaded: " + U2W(g_bridge.currentMap), LOG_OK);
    if (g_bridge.connected && prev.connected && g_bridge.freecam != prev.freecam && !g_pendingCmd)
        Log(g_bridge.freecam ? (g_bridge.fcMode == "debug" ? L"Freecam on (Unreal debug camera)" : L"Freecam on (simple freecam)") : L"Freecam off", LOG_OK);

    // follow speed changes made in-game (+/- keys, mouse wheel in the debug camera)
    if (g_bridge.connected && g_bridge.fcSpeed > 0 && GetTickCount64() - g_speedChangedAt > 3000 &&
        std::fabs(g_bridge.fcSpeed - g_fc.speed) > 0.005f) {
        g_fc.speed = std::clamp(g_bridge.fcSpeed, 0.05f, 50.f);
        SaveSettings();
    }

    if (g_pendingCmd) {
        bool ok; std::string msg;
        if (BridgeResult(g_pendingCmd, ok, msg)) {
            Log(U2W(msg.empty() ? (ok ? "Done." : "Failed.") : msg), ok ? LOG_OK : LOG_ERR);
            if (ok && g_pendingVerb == "list") { BridgeCacheLevels(); ReloadLevels(); }
            g_pendingCmd = 0;
        } else if (GetTickCount64() - g_pendingSince > 20000) {
            Log(L"The game didn't answer. Make sure the LauncherBridge mod is enabled, then restart the game.", LOG_WARN);
            g_pendingCmd = 0;
        }
    }
}

// ================================================================ pages
static const char* StatusText(ImVec4& col) {
    switch (g_status.load()) {
    case ST_WAITING: col = P.warn; return ICON_HOURGLASS "  Waiting for game";
    case ST_INJECTING: col = P.accent; return ICON_SYRINGE "  Loading UE4SS";
    case ST_INJECTED: col = P.ok; return ICON_CIRCLE_CHECK "  Mods active";
    case ST_FAILED: col = P.err; return ICON_CIRCLE_XMARK "  Failed";
    default: col = P.dim; return ICON_CIRCLE "  Idle";
    }
}

static void PagePlay() {
    PageHeader("Play", "Launch your game with UE4SS and your mods loaded.");

    if (g_runtimeMissing) {
        ImGui::PushStyleColor(ImGuiCol_Border, Alpha(P.warn, 0.5f));
        BeginCard("runtime");
        ImGui::PushStyleColor(ImGuiCol_Text, P.warn); ImGui::TextUnformatted(ICON_WARN); ImGui::PopStyleColor();
        ImGui::SameLine(); ImGui::PushFont(F_med); ImGui::TextUnformatted("Microsoft Visual C++ runtime missing"); ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextWrapped("UE4SS needs the Visual C++ 2015-2022 (x64) runtime. Install it, then restart the launcher."); ImGui::PopStyleColor();
        if (AccentButton(ICON_DOWNLOAD "  Download runtime")) OpenUrl(L"https://aka.ms/vs/17/release/vc_redist.x64.exe");
        ImGui::SameLine();
        if (GhostButton("Check again")) g_runtimeMissing = RuntimeMissing();
        EndCard();
        ImGui::PopStyleColor();
    }

    // ---- game card
    BeginCard("game", 0, 20, 20);
    {
        float box = Px(56);
        IconBox(ICON_GAMEPAD, P.accent, box);
        ImGui::SameLine(0, Px(16));
        ImGui::BeginGroup();
        std::string name = W2U(GameDisplayName());
        ImGui::PushFont(F_h2);
        ImGui::TextUnformatted(name.empty() ? "No game selected" : name.c_str());
        ImGui::PopFont();
        float btnW = Px(150);
        float avail = ImGui::GetContentRegionAvail().x - btnW - Px(10);
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushFont(F_small);
        std::string path = W2U(TargetExe());
        ImGui::TextUnformatted(path.empty() ? "Choose the game's .exe to get started" : FitPath(path, avail).c_str());
        ImGui::PopFont(); ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - btnW);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(8));
        if (ImGui::Button(g_gameExe.empty() ? ICON_FOLDER_OPEN "  Choose game" : ICON_PEN "  Change", ImVec2(btnW, 0))) Defer(ChooseGame);

        ImGui::Dummy(ImVec2(0, Px(4)));
        bool can = !TargetExe().empty();
        ImGui::BeginDisabled(!can);
        bool unity = can && IsUnityGame(TargetExe());
        bool classic = can && ClassicGen(TargetExe()) != 0;
        if (AccentButton(unity || classic ? ICON_PLAY "   Launch game" : ICON_PLAY "   Launch game with mods", ImVec2(-1, Px(50)), F_med)) {
            SaveSettings();
            if (!classic) { BridgeReset(); g_autoScanned = false; }
            LaunchGame();
        }
        ImGui::EndDisabled();

        ImGui::Dummy(ImVec2(0, Px(2)));
        if (unity || classic) { /* Unity / Unreal 1-3: nothing to load */ }
        else if (ToggleSwitch("##auto", &g_autoInject)) {
            SaveSettings();
            if (g_autoInject) {
                if (TargetExe().empty()) { g_autoInject = false; Log(L"Choose your game first.", LOG_WARN); }
                else Log(L"Watching for " + NameOf(TargetExe()) + L". Start the game any way you like.");
            } else Log(L"Stopped watching for the game.");
        }
        if (!unity && !classic) {
            ImGui::SameLine(0, Px(12));
            ImGui::BeginGroup();
            ImGui::PushFont(F_med); ImGui::TextUnformatted("Load mods whenever this game starts"); ImGui::PopFont();
            ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
            ImGui::TextUnformatted("Keep the launcher open, then start the game from Steam or anywhere else.");
            ImGui::PopStyleColor(); ImGui::PopFont();
            ImGui::EndGroup();
        }
    }
    EndCard();

    // ---- running processes
    ImGui::Dummy(ImVec2(0, Px(4)));
    SectionLabel("Already running? Load mods into it");
    float recentH = ImGui::GetTextLineHeightWithSpacing() * 3 + Px(26);
    BeginCard("procs", std::max(Px(180), ImGui::GetContentRegionAvail().y - recentH - Px(14)));
    {
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(w - Px(250));
        ImGui::InputTextWithHint("##filter", ICON_SEARCH "  Search by name or PID", g_filter, sizeof g_filter);
        ImGui::SameLine();
        float baseY = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(baseY + (ImGui::GetFrameHeight() - Px(22)) * 0.5f);
        if (ToggleSwitch("##ue", &g_ueOnly)) SaveSettings();
        ImGui::SameLine(0, Px(8));
        ImGui::SetCursorPosY(baseY);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Unreal only");
        ImGui::SameLine(ImGui::GetContentRegionMax().x - Px(40));
        if (ImGui::Button(ICON_ROTATE "##ref", ImVec2(Px(40), 0))) RefreshProcs();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh");

        std::string filt = W2U(Lower(U2W(g_filter)));
        float tableH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight() - Px(10);
        int shown = 0;
        if (ImGui::BeginTable("proctable", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, ImVec2(0, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, Px(70));
            ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 0.65f);
            ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushFont(F_small);
            ImGui::TableHeadersRow();
            ImGui::PopFont(); ImGui::PopStyleColor();
            for (auto& p : g_procs) {
                if (g_ueOnly && !p.isUnreal) continue;
                std::string nm = W2U(p.name);
                if (!filt.empty() && W2U(Lower(p.name)).find(filt) == std::string::npos && std::to_string(p.pid).find(filt) == std::string::npos) continue;
                shown++;
                ImGui::TableNextRow(0, Px(30));
                ImGui::TableNextColumn();
                ImGui::PushID((int)p.pid);
                bool sel = p.pid == g_selPid;
                std::string label = std::string(p.isUnreal ? ICON_GAMEPAD "  " : ICON_CUBE "  ") + nm;
                if (ImGui::Selectable(label.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, Px(22)))) {
                    g_selPid = p.pid;
                    if (ImGui::IsMouseDoubleClicked(0)) { g_handled.push_back(p.pid); Log(L"Loading UE4SS into " + p.name + L"..."); InjectAsync(p.pid, 0); }
                }
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::Text("%lu", p.pid);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FitPath(W2U(p.path), ImGui::GetContentRegionAvail().x).c_str());
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
        if (!shown) {
            ImVec2 c = ImGui::GetItemRectMin(), m = ImGui::GetItemRectMax();
            const char* t = g_ueOnly ? "No Unreal games running. Start one, or turn off \"Unreal only\"." : "No matching processes.";
            ImVec2 ts = ImGui::CalcTextSize(t);
            ImGui::GetWindowDrawList()->AddText(ImVec2((c.x + m.x - ts.x) * 0.5f, (c.y + m.y) * 0.5f), ImGui::GetColorU32(P.dim), t);
        }
        ImGui::BeginDisabled(g_selPid == 0);
        if (ImGui::Button(ICON_SYRINGE "  Load UE4SS into selected game", ImVec2(-1, 0))) {
            for (auto& p : g_procs) if (p.pid == g_selPid) Log(L"Loading UE4SS into " + p.name + L"...");
            g_handled.push_back(g_selPid);
            InjectAsync(g_selPid, 0);
        }
        ImGui::EndDisabled();
    }
    EndCard();

    // ---- recent activity
    BeginCard("recent", 0, 16, 12);
    {
        ImGui::PushFont(F_small);
        LogLines(3, false);
        ImGui::PopFont();
    }
    EndCard();
}


// ---- UE4SS updates
static std::string ShortVer(const std::string& v) {   // v3.0.1-1196-g236adf26 -> v3.0.1-1196
    size_t g = v.find("-g"); return g == std::string::npos ? v : v.substr(0, g);
}
static void UpdateBanner() {
    UpdInfo u = UpdGet();
    bool busy = u.phase == UPD_DOWNLOADING || u.phase == UPD_INSTALLING;
    static std::string dismissed;
    bool show = busy || u.waitingForGame || (u.available && dismissed != u.latest);
    if (!show) return;
    ImGui::PushStyleColor(ImGuiCol_Border, Alpha(P.accent, 0.5f));
    BeginCard("updbanner", 0, 16, 10);
    ImGui::PopStyleColor();
    IconBox(ICON_DOWNLOAD, P.accent, Px(30));
    ImGui::SameLine(0, Px(12));
    ImGui::BeginGroup();
    char t[160];
    if (u.phase == UPD_DOWNLOADING) snprintf(t, sizeof t, "Downloading UE4SS %s...  %d%%", ShortVer(u.latest).c_str(), (int)(u.progress * 100));
    else if (u.phase == UPD_INSTALLING) snprintf(t, sizeof t, "Installing UE4SS %s...", ShortVer(u.latest).c_str());
    else if (u.waitingForGame) snprintf(t, sizeof t, "UE4SS %s will install when you close the game", ShortVer(u.latest).c_str());
    else snprintf(t, sizeof t, "UE4SS %s is available", ShortVer(u.latest).c_str());
    ImGui::PushFont(F_med); ImGui::TextUnformatted(t); ImGui::PopFont();
    ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
    ImGui::Text("You have %s. Your settings and mods are kept.", ShortVer(InstalledUE4SSVersion()).c_str());
    ImGui::PopStyleColor(); ImGui::PopFont();
    ImGui::EndGroup();
    if (busy) {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - Px(160));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(6));
        ImGui::ProgressBar(u.phase == UPD_INSTALLING ? 1.f : u.progress, ImVec2(Px(160), Px(8)), "");
    } else if (!u.waitingForGame) {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - Px(200));
        if (AccentButton(ICON_DOWNLOAD "  Update", ImVec2(Px(110), 0))) UpdInstall();
        ImGui::SameLine();
        if (ImGui::Button("Later", ImVec2(Px(80), 0))) dismissed = u.latest;
    }
    EndCard();
    ImGui::Dummy(ImVec2(0, Px(4)));
}
static void UpdateSection() {
    SectionLabel("UE4SS version");
    UpdInfo u = UpdGet();
    ImGui::PushFont(F_med); ImGui::Text("%s", ShortVer(InstalledUE4SSVersion()).c_str()); ImGui::PopFont();
    const char* pill = nullptr; ImVec4 pc = P.dim; std::string pt;
    if (u.phase == UPD_CHECKING) pill = "checking...";
    else if (u.phase == UPD_DOWNLOADING || u.phase == UPD_INSTALLING) { pill = "updating..."; pc = P.accent; }
    else if (u.available) { pt = ShortVer(u.latest) + " available"; pill = pt.c_str(); pc = P.warn; }
    else if (u.checked) { pill = "up to date"; pc = P.ok; }
    if (pill) { ImGui::SameLine(); ImGui::PushFont(F_small); Pill(pill, pc); ImGui::PopFont(); }
    if (!u.error.empty()) {
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.err); ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(u.error.c_str());
        ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
    }
    bool busy = u.phase != UPD_IDLE;
    ImGui::BeginDisabled(busy);
    if (u.available && !u.waitingForGame) { if (AccentButton(ICON_DOWNLOAD "  Update now", ImVec2(-1, 0))) UpdInstall(); }
    else if (ImGui::Button(ICON_ROTATE "  Check for updates", ImVec2(-1, 0))) UpdCheck(g_updChannel, false);
    if (UpdCanUndo()) {
        if (ImGui::Button(ICON_CLOCK "  Undo last update", ImVec2(-1, 0))) UpdUndo();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Go back to the UE4SS you had before the last update");
    }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, Px(2)));
    if (ToggleSwitch("##updauto", &g_updAuto)) SaveSettings();
    ImGui::SameLine(0, Px(12));
    ImGui::BeginGroup();
    ImGui::PushFont(F_med); ImGui::TextUnformatted("Update automatically"); ImGui::PopFont();
    ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("Checks GitHub when the launcher opens and installs new UE4SS builds (after you close the game).");
    ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, Px(2)));
    ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted("Release channel"); ImGui::PopStyleColor(); ImGui::PopFont();
    const char* chans[] = { "Experimental (newest, recommended)", "Stable releases only" };
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##updch", &g_updChannel, chans, 2)) { SaveSettings(); UpdCheck(g_updChannel, false); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Experimental builds support the newest Unreal games and get fixes first.\nThe last stable release (v3.0.1, 2024) is older than what the launcher ships with.");
    if (ImGui::SmallButton("Release notes on GitHub")) OpenUrl(L"https://github.com/UE4SS-RE/RE-UE4SS/releases");
}

static void PageMods() {
    int on = 0; for (auto& m : g_mods) on += m.enabled;
    char sub[96]; snprintf(sub, sizeof sub, "%d of %d UE4SS mods enabled. Changes apply next time the game starts.", on, (int)g_mods.size());
    PageHeader("Mods", sub);

    // toolbar
    ImGui::BeginDisabled(g_busy);
    if (AccentButton(g_busy ? ICON_HOURGLASS "  Installing..." : ICON_FILE_IMPORT "  Install mod file")) Defer(InstallModFile);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(".zip, .pak or .lua");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FOLDER_PLUS "  Add folder")) Defer([] { std::wstring p = PickFolder(L"Choose the mod's folder (the one containing Scripts or dlls)"); if (!p.empty()) InstallFromFolder(p, NameOf(p)); });
    ImGui::SameLine();
    if (ImGui::Button(ICON_FOLDER_OPEN "  Mods folder")) OpenFolder(ModsDir());
    ImGui::SameLine();
    if (ImGui::Button(ICON_BOX_OPEN "  Game Paks folder")) { std::wstring p = PaksDir(); if (p.empty()) Log(L"Choose your game on the Play page first.", LOG_WARN); else OpenFolder(p); }
    ImGui::SameLine();
    if (ImGui::Button(ICON_ROTATE "##rm")) RefreshMods();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh");
    ImGui::Dummy(ImVec2(0, Px(2)));

    float sideW = Px(300);
    float h = ImGui::GetContentRegionAvail().y;
    // ---- mod list
    ImGui::BeginGroup();
    BeginCard("modlist", h, 8, 8);
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(10), Px(2)));
        ImGui::BeginChild("modscroll", ImVec2(ImGui::GetContentRegionAvail().x - sideW - Px(16), 0));
        for (size_t i = 0; i < g_mods.size(); i++) {
            auto& m = g_mods[i];
            ImGui::PushID((int)i);
            float rowH = Px(52), w = ImGui::GetContentRegionAvail().x;
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool hov = ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w, p.y + rowH));
            if (hov) ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), ImGui::GetColorU32(Alpha(P.text, 0.04f)), g_rounding * S());
            ImGui::SetCursorScreenPos(ImVec2(p.x + Px(10), p.y + Px(9)));
            IconBox(m.isCpp ? ICON_CODE : ICON_SCROLL, m.enabled ? P.accent : P.dim, Px(34));
            ImGui::SameLine(0, Px(12));
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y + Px(8)));
            ImGui::BeginGroup();
            std::string nm = W2U(m.name);
            ImGui::PushFont(F_med); ImGui::PushStyleColor(ImGuiCol_Text, m.enabled ? P.text : P.dim);
            ImGui::TextUnformatted(nm.c_str());
            ImGui::PopStyleColor(); ImGui::PopFont();
            ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
            ImGui::TextUnformatted(m.isCpp ? "C++ mod" : "Lua mod");
            ImGui::PopStyleColor(); ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - Px(52), p.y + (rowH - Px(22)) * 0.5f));
            if (ToggleSwitch("##en", &m.enabled)) {
                SetModEnabled(m.name, m.enabled);
                Log(std::wstring(m.enabled ? L"Enabled " : L"Disabled ") + m.name + L". Restart the game to apply.", LOG_OK);
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + Px(2)));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
        if (g_mods.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted("No mods found in the Mods folder."); ImGui::PopStyleColor(); }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // ---- side panel inside the same card
        ImGui::SameLine(0, Px(8));
        ImVec2 sp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(sp.x - Px(4), sp.y), ImVec2(sp.x - Px(4), sp.y + ImGui::GetContentRegionAvail().y), ImGui::GetColorU32(P.border));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(14), Px(10)));
        ImGui::BeginChild("modside", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        SectionLabel("UE4SS options");
        auto opt = [](const char* id, bool* v, const char* title, const char* desc) -> bool {
            bool c = ToggleSwitch(id, v);
            ImGui::SameLine(0, Px(12));
            ImGui::BeginGroup();
            ImGui::PushFont(F_med); ImGui::TextUnformatted(title); ImGui::PopFont();
            ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
            ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(desc); ImGui::PopTextWrapPos();
            ImGui::PopStyleColor(); ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, Px(4)));
            return c;
        };
        if (opt("##con", &g_optConsole, "Log console", "Opens a window showing what UE4SS and your mods are doing.")) {
            IniSet({ { L"ConsoleEnabled", g_optConsole ? L"1" : L"0" } });
            Log(L"Saved. Restart the game to apply.", LOG_OK);
        }
        if (opt("##gui", &g_optGui, "Debug window", "UE4SS's live object viewer. Toggle in-game with Ctrl+O.")) {
            const wchar_t* v = g_optGui ? L"1" : L"0";
            IniSet({ { L"GuiConsoleEnabled", v }, { L"GuiConsoleVisible", v } });
            Log(L"Saved. Restart the game to apply.", LOG_OK);
        }
        ImGui::Separator();
        UpdateSection();
        ImGui::Separator();
        SectionLabel("Tips");
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextWrapped(".pak mods go into the game's Paks folder (~mods, or LogicMods for Blueprint mods). "
                           "ConsoleEnablerMod unlocks the Unreal console in-game.");
        ImGui::PopStyleColor(); ImGui::PopFont();
        ImGui::Separator();
        SectionLabel("UE4SS files");
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextUnformatted(FitPath(W2U(g_dataDir), ImGui::GetContentRegionAvail().x).c_str());
        ImGui::PopStyleColor(); ImGui::PopFont();
        if (ImGui::Button(ICON_FOLDER_OPEN "  Open", ImVec2(Px(90), 0))) OpenFolder(g_dataDir);
        ImGui::SameLine();
        ImVec4 e = P.err;
        if (GhostButton(ICON_ROTATE "  Reset files", ImVec2(-1, 0), &e)) g_askReset = true;
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    EndCard();
    ImGui::EndGroup();
}

static const LevelEntry* FindLevel(const std::string& path) {
    for (auto& l : g_levels) if (_stricmp(l.path.c_str(), path.c_str()) == 0) return &l;
    return nullptr;
}
static void OpenLevelCmd(const std::string& path, bool confirmed = false) {
    const LevelEntry* l = FindLevel(path);
    bool sub = l ? l->sub : LooksLikeSubLevel(path);
    if (sub && !confirmed) { g_confirmSub = l ? l->path : path; return; }
    SendBridge("open", l ? l->path : path);
}
// Typed input: accept file paths, /Game paths, Windows paths or just a level name.
static void OpenTypedLevel(const std::string& typed) {
    std::string p = NormalizeLevelPath(typed);
    if (p.empty()) return;
    if (p[0] != '/') {   // bare name: look it up
        std::vector<const LevelEntry*> hits;
        for (auto& l : g_levels) if (_stricmp(l.name.c_str(), p.c_str()) == 0) hits.push_back(&l);
        if (hits.size() == 1) p = hits[0]->path;
        else {
            Log(hits.empty() ? L"No level called \"" + U2W(p) + L"\" was found. Pick one from the list, or use a path like /Game/Maps/MyLevel."
                             : L"More than one level is called \"" + U2W(p) + L"\". Type its full path instead.", LOG_WARN);
            return;
        }
    }
    if (!g_levels.empty() && !FindLevel(p)) {
        Log(L"\"" + U2W(p) + L"\" isn't one of this game's levels, so it wasn't opened (Unreal would just send you back to the main menu).", LOG_WARN);
        return;
    }
    if (p != typed) Log(L"Using level path " + U2W(p));
    snprintf(g_manualLevel, sizeof g_manualLevel, "%s", p.c_str());
    OpenLevelCmd(p);
}

static void PageLevels() {
    PageHeader("Levels", "Browse the maps in your game and jump straight to any of them.");
    bool busy = g_pendingCmd != 0;

    // ---- connection card
    BeginCard("conn", 0, 20, 16);
    {
        IconBox(g_bridge.connected ? ICON_SIGNAL : ICON_PLUG, g_bridge.connected ? P.ok : P.dim, Px(48));
        ImGui::SameLine(0, Px(14));
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(2));
        ImGui::PushFont(F_h2);
        ImGui::TextUnformatted(g_bridge.connected ? "Connected to game" : "Game not connected");
        ImGui::PopFont();
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        if (g_bridge.connected) {
            std::string cur = g_bridge.currentMap.empty() ? "loading..." : g_bridge.currentMap;
            ImGui::Text("Current level:  %s", cur.c_str());
        } else ImGui::TextUnformatted("Launch the game with mods from the Play page to switch levels.");
        ImGui::PopStyleColor(); ImGui::PopFont();
        ImGui::EndGroup();

        float bw1 = Px(150), bw2 = Px(140);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw1 - bw2 - Px(10));
        float by = ImGui::GetCursorPosY() + (Px(48) - ImGui::GetFrameHeight()) * 0.5f;
        ImGui::SetCursorPosY(by);
        ImGui::BeginDisabled(!g_bridge.connected || busy);
        if (AccentButton(busy && g_pendingVerb == "list" ? ICON_HOURGLASS "  Scanning..." : ICON_SEARCH "  Scan levels", ImVec2(bw1, 0))) SendBridge("list");
        ImGui::SameLine();
        ImGui::SetCursorPosY(by);
        if (ImGui::Button(ICON_ROTATE "  Reload level", ImVec2(bw2, 0))) SendBridge("reload");
        ImGui::EndDisabled();
    }
    EndCard();

    // ---- toolbar
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Px(520));
    ImGui::InputTextWithHint("##lvlfilter", ICON_SEARCH "  Search levels", g_levelFilter, sizeof g_levelFilter);
    ImGui::SameLine();
    float baseY = ImGui::GetCursorPosY();
    auto toggle = [&](const char* id, bool* v, const char* label, const char* tip) {
        ImGui::SetCursorPosY(baseY + (ImGui::GetFrameHeight() - Px(22)) * 0.5f);
        ToggleSwitch(id, v);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine(0, Px(8)); ImGui::SetCursorPosY(baseY);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    toggle("##mainonly", &g_mainOnly, "Main levels only", "Hides pieces of bigger levels (lighting, sound, layout...) that can't be opened on their own.");
    ImGui::SameLine(0, Px(16));
    toggle("##hideeng", &g_hideEngine, "Hide engine maps", "Hides Unreal Engine's built-in test maps.");

    // filter
    std::string f = W2U(Lower(U2W(g_levelFilter)));
    std::vector<const LevelEntry*> shown;
    for (auto& l : g_levels) {
        if (g_hideEngine && (l.path.rfind("/Engine/", 0) == 0 || l.path.rfind("/Script/", 0) == 0)) continue;
        if (g_mainOnly && l.sub && !(g_bridge.connected && l.path == g_bridge.currentMap)) continue;
        if (!f.empty() && W2U(Lower(U2W(l.path))).find(f) == std::string::npos) continue;
        shown.push_back(&l);
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushFont(F_small);
    const char* src = g_levelSrc == LS_CACHE ? (g_bridge.connected ? "from the game" : "from last scan") : g_levelSrc == LS_FILES ? "found in game files" : "";
    char cnt[96]; snprintf(cnt, sizeof cnt, "%d level%s %s", (int)shown.size(), shown.size() == 1 ? "" : "s", src);
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(cnt).x);
    ImGui::SetCursorPosY(baseY);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(cnt);
    ImGui::PopFont(); ImGui::PopStyleColor();

    // ---- level list
    float manualH = ImGui::GetFrameHeight() + Px(34);
    BeginCard("levels", std::max(Px(160), ImGui::GetContentRegionAvail().y - manualH - Px(10)), 8, 8);
    {
        if (shown.empty()) {
            ImVec2 a = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
            const char* l1 = g_levels.empty() ? "No levels found yet" : "No levels match your search";
            const char* l2 = g_levels.empty()
                ? (g_bridge.connected ? "Click Scan levels to read the list from the game." : "Launch the game with mods, then the list fills in automatically.")
                : "Try a different search, or turn off Main levels only.";
            ImGui::PushFont(F_h2); ImVec2 t1 = ImGui::CalcTextSize(l1);
            ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + (sz.x - t1.x) / 2, a.y + sz.y / 2 - t1.y - Px(4)), ImGui::GetColorU32(P.text), l1);
            ImGui::PopFont();
            ImVec2 t2 = ImGui::CalcTextSize(l2);
            ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + (sz.x - t2.x) / 2, a.y + sz.y / 2 + Px(4)), ImGui::GetColorU32(P.dim), l2);
        } else {
            float rowH = Px(52);
            ImGuiListClipper clip;
            clip.Begin((int)shown.size(), rowH + Px(2));
            while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const LevelEntry& l = *shown[i];
                bool current = g_bridge.connected && l.path == g_bridge.currentMap;
                ImGui::PushID(i);
                float w = ImGui::GetContentRegionAvail().x;
                ImVec2 p = ImGui::GetCursorScreenPos();
                bool hov = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w, p.y + rowH));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (current) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), ImGui::GetColorU32(Alpha(P.accent, 0.10f)), g_rounding * S());
                else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), ImGui::GetColorU32(Alpha(P.text, 0.04f)), g_rounding * S());
                ImGui::SetCursorScreenPos(ImVec2(p.x + Px(10), p.y + Px(9)));
                IconBox(ICON_MAP, current ? P.accent : P.dim, Px(34));
                ImGui::SameLine(0, Px(12));
                ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y + Px(8)));
                ImGui::BeginGroup();
                ImGui::PushFont(F_med); ImGui::TextUnformatted(l.name.c_str()); ImGui::PopFont();
                if (l.sub) { ImGui::SameLine(0, Px(10)); ImGui::PushFont(F_small); Pill("Part of a level", P.warn); ImGui::PopFont(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(8)); }
                ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
                ImGui::TextUnformatted(FitPath(l.folder, w - Px(260)).c_str());
                ImGui::PopStyleColor(); ImGui::PopFont();
                ImGui::EndGroup();
                float bw = Px(96);
                if (current) {
                    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - Px(100), p.y + (rowH - ImGui::GetFontSize() - Px(8)) * 0.5f));
                    ImGui::PushFont(F_small); Pill(ICON_LOCATION "  Current", P.accent); ImGui::PopFont();
                }
                ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - Px(10), p.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
                ImGui::BeginDisabled(!g_bridge.connected || busy);
                bool go = hov || current ? AccentButton(current ? ICON_ROTATE "  Reload" : ICON_DOOR "  Open", ImVec2(bw, 0))
                                         : ImGui::Button(ICON_DOOR "  Open", ImVec2(bw, 0));
                ImGui::EndDisabled();
                if (go) OpenLevelCmd(l.path);
                else if (hov && ImGui::IsMouseDoubleClicked(0) && g_bridge.connected && !busy) OpenLevelCmd(l.path);
                if (hov) ImGui::SetTooltip("%s", l.path.c_str());
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + Px(2)));
                ImGui::Dummy(ImVec2(0, 0));
                ImGui::PopID();
            }
        }
    }
    EndCard();

    // ---- open by path
    BeginCard("manual", 0, 14, 10);
    {
        ImGui::BeginDisabled(!g_bridge.connected || busy);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Px(130));
        bool enter = ImGui::InputTextWithHint("##manual", ICON_ROUTE "  Open by name or path, e.g. HK_Project_MainStart or /Game/Map/...", g_manualLevel, sizeof g_manualLevel, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button(ICON_ARROW_RIGHT "  Go", ImVec2(-1, 0)) || enter) && g_manualLevel[0]) OpenTypedLevel(g_manualLevel);
        ImGui::EndDisabled();
    }
    EndCard();
}

static const struct { const char* id; const char* label; } kFcKeys[] = {
    { "INS", "Insert" }, { "HOME", "Home" }, { "END", "End" }, { "DEL", "Delete" }, { "PAGE_UP", "Page Up" }, { "PAGE_DOWN", "Page Down" },
    { "F5", "F5" }, { "F6", "F6" }, { "F7", "F7" }, { "F8", "F8" }, { "F9", "F9" }, { "F11", "F11" }, { "F12", "F12" } };
static const char* KeyLabel(const std::string& id) { for (auto& k : kFcKeys) if (id == k.id) return k.label; return id.c_str(); }

static void StatTile(const char* label, const char* value, float w) {
    ImGui::BeginGroup();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = Px(70);
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(P.card2), g_rounding * S());
    ImGui::SetCursorScreenPos(ImVec2(p.x + Px(14), p.y + Px(10)));
    SectionLabel(label);
    ImGui::SetCursorScreenPos(ImVec2(p.x + Px(14), p.y + Px(30)));
    ImGui::PushFont(F_h2); ImGui::TextUnformatted(value); ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(w, 0));
    ImGui::EndGroup();
}

static void KeyCap(const char* k) {
    ImGui::PushFont(F_small);
    ImVec2 ts = ImGui::CalcTextSize(k), p = ImGui::GetCursorScreenPos();
    float padX = Px(7), padY = Px(3);
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ts.x + padX * 2, p.y + ts.y + padY * 2), ImGui::GetColorU32(P.card2), Px(5));
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + ts.x + padX * 2, p.y + ts.y + padY * 2), ImGui::GetColorU32(P.border), Px(5));
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + padX, p.y + padY), ImGui::GetColorU32(P.text), k);
    ImGui::Dummy(ImVec2(ts.x + padX * 2, ts.y + padY * 2));
    ImGui::PopFont();
}
static void ControlRow(std::initializer_list<const char*> caps, const char* what) {
    bool first = true;
    for (auto c : caps) { if (!first) ImGui::SameLine(0, Px(4)); KeyCap(c); first = false; }
    ImGui::SameLine(Px(150));
    ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted(what); ImGui::PopStyleColor();
}

static void PageFreecam() {
    PageHeader("Freecam", "Fly the camera anywhere in the game. Press your hotkey in-game, or use the buttons here.");
    bool busy = g_pendingCmd != 0, conn = g_bridge.connected, on = conn && g_bridge.freecam;

    // ---- status card
    BeginCard("fcstatus", 0, 20, 16);
    {
        IconBox(on ? ICON_VIDEO : ICON_CAMERA, on ? P.accent : conn ? P.text : P.dim, Px(48));
        ImGui::SameLine(0, Px(14));
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(2));
        ImGui::PushFont(F_h2);
        ImGui::TextUnformatted(!conn ? "Game not connected" : on ? "Freecam is on" : "Freecam is off");
        ImGui::PopFont();
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        if (!conn) ImGui::TextUnformatted("Launch the game with mods from the Play page.");
        else if (on) ImGui::Text("%s  \xC2\xB7  %.2gx speed  \xC2\xB7  press %s to return", g_bridge.fcMode == "debug" ? "Unreal debug camera" : "Simple freecam", g_fc.speed, KeyLabel(g_fc.key));
        else if (g_fc.enabled) ImGui::Text("Press %s in the game to start flying.", KeyLabel(g_fc.key));
        else ImGui::TextUnformatted("The hotkey is switched off - use the button instead.");
        ImGui::PopStyleColor(); ImGui::PopFont();
        ImGui::EndGroup();

        float bw1 = Px(170), bw2 = Px(190);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw1 - bw2 - Px(10));
        float by = ImGui::GetCursorPosY() + (Px(48) - ImGui::GetFrameHeight()) * 0.5f;
        ImGui::SetCursorPosY(by);
        ImGui::BeginDisabled(!conn || busy);
        if (AccentButton(on ? ICON_POWER "  Turn freecam off" : ICON_VIDEO "  Turn freecam on", ImVec2(bw1, 0))) SendBridge("freecam", on ? "off" : "on");
        ImGui::SameLine(); ImGui::SetCursorPosY(by);
        ImGui::BeginDisabled(!on);
        if (ImGui::Button(ICON_USER "  Move player to camera", ImVec2(bw2, 0))) SendBridge("tp");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Teleports your character to where the freecam is, then turns freecam off.");
        ImGui::EndDisabled();
    }
    EndCard();

    // ---- speed
    ImGui::Dummy(ImVec2(0, Px(2)));
    SectionLabel("Speed");
    BeginCard("fcspeedcard", 0, 18, 14);
    {
        auto setSpeed = [&](float v) {
            g_fc.speed = std::clamp(v, 0.05f, 50.f);
            g_speedChangedAt = GetTickCount64();
            SaveSettings();                                   // the game also re-reads this within ~2 s
            if (on && !g_pendingCmd) { char b[32]; snprintf(b, sizeof b, "%.3f", g_fc.speed); SendBridge("fcspeed", b); }
        };
        ImGui::PushFont(F_h2);
        char big[32]; snprintf(big, sizeof big, "%.2gx", g_fc.speed);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(big);
        ImGui::PopFont();
        ImGui::SameLine(Px(90));
        float presetsW = Px(6 * 58 + 5 * 6);
        ImGui::SetNextItemWidth(std::max(Px(160), ImGui::GetContentRegionAvail().x - presetsW - Px(14)));
        static float drag = 1.f;
        static bool dragging = false;
        if (!dragging) drag = g_fc.speed;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(4));
        if (ImGui::SliderFloat("##fcspeedslider", &drag, 0.1f, 20.0f, "", ImGuiSliderFlags_Logarithmic)) g_speedChangedAt = GetTickCount64();
        dragging = ImGui::IsItemActive();
        if (dragging) g_fc.speed = drag;
        if (ImGui::IsItemDeactivatedAfterEdit()) setSpeed(drag);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to change how fast the freecam flies");
        const float presets[] = { 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f };
        for (float pv : presets) {
            ImGui::SameLine(0, Px(6));
            char lb[16]; snprintf(lb, sizeof lb, "%gx", pv);
            bool cur = std::fabs(g_fc.speed - pv) < 0.01f;
            if (cur ? AccentButton(lb, ImVec2(Px(58), 0)) : ImGui::Button(lb, ImVec2(Px(58), 0))) setSpeed(pv);
        }
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextUnformatted("Works for both camera types and applies straight away. In-game, press + or - to change it (the mouse wheel also works in the debug camera).");
        ImGui::PopStyleColor(); ImGui::PopFont();
    }
    EndCard();

    // ---- camera position
    ImGui::Dummy(ImVec2(0, Px(2)));
    SectionLabel("Camera position");
    BeginCard("fcpos", 0, 16, 14);
    {
        double x = 0, y = 0, z = 0, pitch = 0, yaw = 0;
        bool have = conn && sscanf(g_bridge.cam.c_str(), "%lf,%lf,%lf", &x, &y, &z) == 3;
        if (conn) sscanf(g_bridge.rot.c_str(), "%lf,%lf", &pitch, &yaw);
        float gap = Px(10), w = (ImGui::GetContentRegionAvail().x - Px(110) - gap * 4) / 5;
        char b[5][32];
        auto fmt = [&](char* o, double v) { if (have) snprintf(o, 32, "%.0f", v); else snprintf(o, 32, "-"); };
        fmt(b[0], x); fmt(b[1], y); fmt(b[2], z);
        if (have) { snprintf(b[3], 32, "%.0f\xC2\xB0", pitch); snprintf(b[4], 32, "%.0f\xC2\xB0", yaw); } else { strcpy(b[3], "-"); strcpy(b[4], "-"); }
        const char* labels[] = { "X", "Y", "Z", "Pitch", "Yaw" };
        for (int i = 0; i < 5; i++) { if (i) ImGui::SameLine(0, gap); StatTile(labels[i], b[i], w); }
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(!have);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (Px(70) - ImGui::GetFrameHeight()) * 0.5f);
        if (ImGui::Button(ICON_COPY "  Copy", ImVec2(-1, 0))) {
            char t[128]; snprintf(t, sizeof t, "X=%.1f Y=%.1f Z=%.1f Pitch=%.1f Yaw=%.1f", x, y, z, pitch, yaw);
            ImGui::SetClipboardText(t); Log(L"Camera position copied.", LOG_OK);
        }
        ImGui::EndDisabled();
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextUnformatted(conn ? "Read straight from the game's camera every second - no memory scanning, so it keeps working after game updates."
                                    : "Shows the live camera position while the game is connected.");
        ImGui::PopStyleColor(); ImGui::PopFont();
    }
    EndCard();

    // ---- settings + controls side by side
    ImGui::Dummy(ImVec2(0, Px(2)));
    float colW = (ImGui::GetContentRegionAvail().x - Px(14)) * 0.5f;
    float rowH = ImGui::GetContentRegionAvail().y;
    ImGui::BeginGroup();
    SectionLabel("Settings");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(18), Px(14)));
    ImGui::BeginChild("fcset", ImVec2(colW, rowH - ImGui::GetTextLineHeightWithSpacing()), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        bool changed = false;
        changed |= ToggleSwitch("##fcen", &g_fc.enabled);
        ImGui::SameLine(0, Px(12)); ImGui::AlignTextToFramePadding(); ImGui::PushFont(F_med); ImGui::TextUnformatted("Hotkey"); ImGui::PopFont();
        ImGui::SameLine(colW - Px(18) - Px(150));
        ImGui::SetNextItemWidth(Px(150));
        ImGui::BeginDisabled(!g_fc.enabled);
        if (ImGui::BeginCombo("##fckey", KeyLabel(g_fc.key))) {
            for (auto& k : kFcKeys) if (ImGui::Selectable(k.label, g_fc.key == k.id)) { g_fc.key = k.id; changed = true; }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        ImGui::AlignTextToFramePadding(); ImGui::PushFont(F_med); ImGui::TextUnformatted("Camera type"); ImGui::PopFont();
        ImGui::SameLine(colW - Px(18) - Px(220));
        ImGui::SetNextItemWidth(Px(220));
        const char* modes[][2] = { { "auto", "Automatic (recommended)" }, { "debug", "Unreal debug camera" }, { "builtin", "Simple freecam" } };
        const char* cur = modes[0][1]; for (auto& m : modes) if (g_fc.mode == m[0]) cur = m[1];
        if (ImGui::BeginCombo("##fcmode", cur)) {
            for (auto& m : modes) if (ImGui::Selectable(m[1], g_fc.mode == m[0])) { g_fc.mode = m[0]; changed = true; }
            ImGui::EndCombo();
        }
        changed |= ToggleSwitch("##fchide", &g_fc.hideText);
        ImGui::SameLine(0, Px(12)); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Hide debug camera text");
        if (changed) { SaveSettings(); Log(L"Freecam settings saved" + std::wstring(conn ? L" - applied to the game." : L"."), LOG_OK); }
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextWrapped("Automatic uses Unreal's debug camera, and switches to the simple freecam in games that removed it.%s",
                           g_fc.key == "INS" ? " Note: Insert also reloads Blueprint mods if you have any installed." : "");
        ImGui::PopStyleColor(); ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(); ImGui::PopStyleColor();
    ImGui::EndGroup();

    ImGui::SameLine(0, Px(14));
    ImGui::BeginGroup();
    SectionLabel("Controls");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(18), Px(14)));
    ImGui::BeginChild("fcctl", ImVec2(0, rowH - ImGui::GetTextLineHeightWithSpacing()), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8), Px(7)));
        ControlRow({ KeyLabel(g_fc.key) }, "Freecam on / off");
        ControlRow({ "W", "A", "S", "D" }, "Fly around");
        ControlRow({ "E", "Q" }, "Up / down");
        ControlRow({ "Mouse" }, "Look around (or arrow keys)");
        ControlRow({ "+", "-" }, "Change speed (numpad works too)");
        ControlRow({ "Wheel" }, "Change speed (debug camera)");
        ControlRow({ "Shift", "Ctrl" }, "Boost / slow while held (simple)");
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(); ImGui::PopStyleColor();
    ImGui::EndGroup();
}

static void ThemePreview(int i, float w, float h) {
    const Theme& t = kThemes[i];
    ImGui::PushID(i);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("theme", ImVec2(w, h + Px(30)));
    bool hov = ImGui::IsItemHovered(), sel = g_theme == i;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = g_rounding * S();
    ImVec4 acc = (g_customAccent) ? ImVec4(g_accent[0], g_accent[1], g_accent[2], 1) : Hex(t.accent);
    auto U = [](ImVec4 v) { return ImGui::GetColorU32(v); };
    // mini window
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U(Hex(t.bg)), r);
    dl->AddRectFilled(p, ImVec2(p.x + w * 0.28f, p.y + h), U(Hex(t.sidebar)), r, ImDrawFlags_RoundCornersLeft);
    for (int k = 0; k < 3; k++)
        dl->AddRectFilled(ImVec2(p.x + w * 0.05f, p.y + h * (0.18f + k * 0.16f)), ImVec2(p.x + w * 0.22f, p.y + h * (0.18f + k * 0.16f) + Px(5)),
                          U(k == 0 ? acc : Alpha(Hex(t.dim), 0.6f)), Px(3));
    ImVec2 c0(p.x + w * 0.34f, p.y + h * 0.14f), c1(p.x + w * 0.94f, p.y + h * 0.86f);
    dl->AddRectFilled(c0, c1, U(Hex(t.card)), r * 0.6f);
    dl->AddRectFilled(ImVec2(c0.x + w * 0.05f, c0.y + h * 0.12f), ImVec2(c0.x + w * 0.38f, c0.y + h * 0.12f + Px(6)), U(Hex(t.text)), Px(3));
    dl->AddRectFilled(ImVec2(c0.x + w * 0.05f, c0.y + h * 0.27f), ImVec2(c0.x + w * 0.30f, c0.y + h * 0.27f + Px(4)), U(Hex(t.dim)), Px(2));
    dl->AddRectFilled(ImVec2(c0.x + w * 0.05f, c1.y - h * 0.30f), ImVec2(c1.x - w * 0.05f, c1.y - h * 0.10f), U(acc), r * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), U(sel ? P.accent : hov ? Mix(P.border, P.text, 0.3f) : P.border), r, 0, sel ? Px(2.5f) : Px(1));
    if (sel) {
        ImVec2 cc(p.x + w - Px(14), p.y + Px(14));
        dl->AddCircleFilled(cc, Px(10), U(P.accent), 20);
        ImGui::PushFont(F_small);
        ImVec2 ts = ImGui::CalcTextSize(ICON_CHECK);
        dl->AddText(ImVec2(cc.x - ts.x * 0.5f, cc.y - ts.y * 0.5f), U(P.onAccent), ICON_CHECK);
        ImGui::PopFont();
    }
    ImGui::PushFont(sel ? F_med : F_body);
    dl->AddText(ImVec2(p.x + Px(2), p.y + h + Px(7)), U(sel ? P.text : P.dim), t.name);
    ImGui::PopFont();
    if (clicked) { g_theme = i; g_styleDirty = true; SaveSettings(); }
    ImGui::PopID();
}

static void PageLook() {
    PageHeader("Appearance", "Pick a theme and make the launcher yours.");
    ImGui::BeginChild("lookscroll", ImVec2(0, 0));
    SectionLabel("Theme");
    {
        float avail = ImGui::GetContentRegionAvail().x - Px(4);
        int cols = std::max(2, (int)(avail / Px(190)));
        float gap = Px(14), w = (avail - gap * (cols - 1)) / cols, h = w * 0.58f;
        for (int i = 0; i < kThemeCount; i++) {
            if (i % cols) ImGui::SameLine(0, gap);
            ThemePreview(i, w, h);
        }
    }
    ImGui::Dummy(ImVec2(0, Px(8)));
    SectionLabel("Accent colour");
    BeginCard("accent");
    {
        if (ToggleSwitch("##ca", &g_customAccent)) { g_styleDirty = true; SaveSettings(); }
        ImGui::SameLine(0, Px(12)); ImGui::AlignTextToFramePadding();
        ImGui::PushFont(F_med); ImGui::TextUnformatted("Use a custom accent colour"); ImGui::PopFont();
        ImGui::BeginDisabled(!g_customAccent);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < (int)(sizeof kAccentPresets / sizeof kAccentPresets[0]); i++) {
            if (i) ImGui::SameLine(0, Px(10));
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos(); float d = Px(28);
            if (ImGui::InvisibleButton("sw", ImVec2(d, d))) {
                ImVec4 c = Hex(kAccentPresets[i]); g_accent[0] = c.x; g_accent[1] = c.y; g_accent[2] = c.z; g_styleDirty = true; SaveSettings();
            }
            ImVec4 c = Hex(kAccentPresets[i], g_customAccent ? 1.f : 0.35f);
            dl->AddCircleFilled(ImVec2(p.x + d / 2, p.y + d / 2), d / 2 - Px(2), ImGui::GetColorU32(c), 24);
            ImVec4 cur(g_accent[0], g_accent[1], g_accent[2], 1);
            if (g_customAccent && std::fabs(cur.x - c.x) + std::fabs(cur.y - c.y) + std::fabs(cur.z - c.z) < 0.02f)
                dl->AddCircle(ImVec2(p.x + d / 2, p.y + d / 2), d / 2 + Px(1), ImGui::GetColorU32(P.text), 24, Px(2));
            ImGui::PopID();
        }
        ImGui::SameLine(0, Px(16));
        if (ImGui::ColorEdit3("##acc", g_accent, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) g_styleDirty = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        ImGui::SameLine(); ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted("Custom..."); ImGui::PopStyleColor();
        ImGui::EndDisabled();
    }
    EndCard();

    ImGui::Dummy(ImVec2(0, Px(8)));
    SectionLabel("Layout");
    BeginCard("layout");
    {
        static int scale = g_scalePct;
        ImGui::PushFont(F_med); ImGui::TextUnformatted("Interface size"); ImGui::PopFont();
        ImGui::SetNextItemWidth(Px(320));
        ImGui::SliderInt("##scale", &scale, 80, 150, "%d%%");
        if (ImGui::IsItemDeactivatedAfterEdit()) { g_scalePct = scale; g_fontsDirty = g_styleDirty = true; SaveSettings(); }
        ImGui::PushFont(F_med); ImGui::TextUnformatted("Corner roundness"); ImGui::PopFont();
        ImGui::SetNextItemWidth(Px(320));
        if (ImGui::SliderFloat("##round", &g_rounding, 0, 16, "%.0f px")) g_styleDirty = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        ImGui::Dummy(ImVec2(0, Px(4)));
        if (ToggleSwitch("##rgb", &g_rgbBorder)) SaveSettings();
        ImGui::SameLine(0, Px(12)); ImGui::AlignTextToFramePadding();
        ImGui::PushFont(F_med); ImGui::TextUnformatted("RGB outline round the window"); ImGui::PopFont();
        ImGui::BeginDisabled(!g_rgbBorder);
        ImGui::SetNextItemWidth(Px(320));
        ImGui::SliderInt("##rgbspeed", &g_rgbSpeed, 0, 400, g_rgbSpeed ? "Speed %d%%" : "Still");
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        ImGui::SetNextItemWidth(Px(320));
        ImGui::SliderInt("##rgbwidth", &g_rgbWidth, 1, 12, "Thickness %d px");
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, Px(4)));
        if (ImGui::Button(ICON_ROTATE "  Reset to defaults")) {
            g_rgbBorder = true; g_rgbSpeed = 100; g_rgbWidth = 3;
            g_theme = 0; g_customAccent = false; g_scalePct = scale = 100; g_rounding = 10;
            g_fontsDirty = g_styleDirty = true; SaveSettings();
        }
    }
    EndCard();
    ImGui::EndChild();
}

static void PageLog() {
    PageHeader("Activity", "Everything the launcher has done this session.");
    if (ImGui::Button(ICON_COPY "  Copy all")) {
        std::string all;
        { std::lock_guard<std::mutex> lk(g_logMx); for (auto& l : g_logs) all += "[" + l.time + "] " + l.text + "\r\n"; }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_ERASER "  Clear")) { std::lock_guard<std::mutex> lk(g_logMx); g_logs.clear(); }
    BeginCard("logcard", ImGui::GetContentRegionAvail().y, 18, 14);
    LogLines(0, true);
    EndCard();
}

// ================================================================ sidebar & popups
static bool NavItem(const char* icon, const char* label, bool active) {
    float w = ImGui::GetContentRegionAvail().x, h = Px(40);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = g_rounding * S() * 0.8f;
    if (active) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(Alpha(P.accent, 0.16f)), r);
        dl->AddRectFilled(ImVec2(p.x, p.y + Px(10)), ImVec2(p.x + Px(3), p.y + h - Px(10)), ImGui::GetColorU32(P.accent), Px(2));
    } else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(Alpha(P.text, 0.05f)), r);
    ImGui::PushFont(active ? F_med : F_body);
    ImVec4 col = active ? P.text : hov ? Mix(P.dim, P.text, 0.5f) : P.dim;
    float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
    ImVec2 is = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p.x + Px(16) + (Px(18) - is.x) * 0.5f, ty), ImGui::GetColorU32(active ? P.accent : col), icon);
    dl->AddText(ImVec2(p.x + Px(46), ty), ImGui::GetColorU32(col), label);
    ImGui::PopFont();
    return clicked;
}

static void Sidebar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.sidebar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(14), Px(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::BeginChild("sidebar", ImVec2(Px(230), 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    // logo
    {
        ImVec2 p = ImGui::GetCursorScreenPos(); float d = Px(38);
        if (g_logo) {
            ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)(intptr_t)g_logo, p, ImVec2(p.x + d, p.y + d), ImVec2(0, 0), ImVec2(1, 1),
                                                        IM_COL32_WHITE, g_rounding * S() * 0.8f);
            if (!P.dark) ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + d, p.y + d), ImGui::GetColorU32(P.border), g_rounding * S() * 0.8f);
        } else {
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + d, p.y + d), ImGui::GetColorU32(P.accent), g_rounding * S());
            ImGui::PushFont(F_h2);
            ImVec2 ts = ImGui::CalcTextSize(ICON_BOLT);
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (d - ts.x) / 2, p.y + (d - ts.y) / 2), ImGui::GetColorU32(P.onAccent), ICON_BOLT);
            ImGui::PopFont();
        }
        ImGui::Dummy(ImVec2(d, d));
        ImGui::SameLine(0, Px(12));
        ImGui::BeginGroup();
        ImGui::PushFont(F_h2); ImGui::TextUnformatted("UE4SS"); ImGui::PopFont();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(10));
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted("Launcher"); ImGui::PopStyleColor(); ImGui::PopFont();
        ImGui::EndGroup();
    }
    ImGui::Dummy(ImVec2(0, Px(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, Px(4)));
    struct { const char* icon; const char* label; int page; } nav[] = {
        { ICON_PLAY, "Play", PG_PLAY }, { ICON_USER, "Player", PG_PLAYER }, { ICON_BOLT, "Cheats", PG_CHEATS }, { ICON_PUZZLE, "Mods", PG_MODS }, { ICON_MAP, "Levels", PG_LEVELS }, { ICON_VIDEO, "Freecam", PG_FREECAM }, { ICON_PEN_RULER, "Editor", PG_EDITOR }, { ICON_BOX_OPEN, "Models", PG_MODELS },
        { ICON_PALETTE, "Appearance", PG_LOOK }, { ICON_LIST, "Activity", PG_LOG } };
    for (auto& n : nav) if (n.page >= 0 && NavItem(n.icon, n.label, g_page == n.page)) {
        g_page = n.page;
        if (g_page == PG_MODS) RefreshMods();
        if (g_page == PG_PLAY) RefreshProcs();
        if (g_page == PG_LEVELS) ReloadLevels();
    }
    ImGui::PopStyleVar();

    // status card pinned to the bottom, with the Ko-fi button under it
    float cardH = Px(96), coffeeH = Px(36);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - cardH - coffeeH - Px(18) - Px(8));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Mix(P.sidebar, P.text, 0.04f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(14), Px(12)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, g_rounding * S());
    ImGui::BeginChild("status", ImVec2(0, cardH), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    SectionLabel("Status");
    ImVec4 col; const char* st = StatusText(col);
    ImGui::PushFont(F_small); Pill(st, col); ImGui::PopFont();
    std::string g = W2U(GameDisplayName());
    if (g_bridge.connected && !g_bridge.currentMap.empty()) {
        std::string m = g_bridge.currentMap; size_t sl = m.find_last_of('/');
        g += (g.empty() ? "" : "  \xC2\xB7  ") + (sl == std::string::npos ? m : m.substr(sl + 1));
    }
    ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
    ImGui::TextUnformatted(FitPath(g.empty() ? "No game selected" : g, ImGui::GetContentRegionAvail().x).c_str());
    ImGui::PopStyleColor(); ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar(2); ImGui::PopStyleColor();

    // Buy me a coffee (Ko-fi)
    {
        ImGui::Dummy(ImVec2(0, Px(8) - ImGui::GetStyle().ItemSpacing.y));
        ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x;
        bool clicked = ImGui::InvisibleButton("##kofi", ImVec2(w, coffeeH));
        bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec4 kofi = Hex(0xFF5E5B);                                   // Ko-fi's red
        ImVec4 bg = act ? Mix(kofi, Hex(0x000000), 0.15f) : hov ? Mix(kofi, Hex(0xFFFFFF), 0.10f) : kofi;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + coffeeH), ImGui::GetColorU32(bg), g_rounding * S());
        // a little coffee cup
        ImGui::PushFont(F_body);
        const char* label = "Buy me a coffee";
        ImVec2 ts = ImGui::CalcTextSize(label);
        float cupW = Px(16), gap = Px(9), total = cupW + Px(4) + gap + ts.x;
        float x0 = p.x + (w - total) / 2, cy = p.y + coffeeH / 2;
        ImU32 white = IM_COL32(255, 255, 255, 255);
        ImVec2 c0(x0, cy - Px(5)), c1(x0 + cupW, cy + Px(7));
        dl->AddRectFilled(c0, c1, white, Px(3), ImDrawFlags_RoundCornersBottom);
        dl->AddCircle(ImVec2(c1.x + Px(1), cy + Px(1)), Px(4), white, 0, Px(2));          // handle
        {   // a small heart on the cup, like Ko-fi's logo
            ImU32 hc = ImGui::GetColorU32(bg); float hx = x0 + cupW / 2, hy = cy + Px(0.5f), r = Px(1.9f);
            dl->AddCircleFilled(ImVec2(hx - r, hy - r * 0.3f), r, hc); dl->AddCircleFilled(ImVec2(hx + r, hy - r * 0.3f), r, hc);
            dl->AddTriangleFilled(ImVec2(hx - r * 2, hy), ImVec2(hx + r * 2, hy), ImVec2(hx, hy + r * 2.2f), hc);
        }
        dl->AddText(ImVec2(x0 + cupW + Px(4) + gap, cy - ts.y / 2), white, label);
        ImGui::PopFont();
        if (hov) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); ImGui::SetTooltip("Support me on Ko-fi\nko-fi.com/glitch3r"); }
        if (clicked) OpenUrl(L"https://ko-fi.com/glitch3r");
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

static void Popups() {
    if (!g_pendingPak.empty() && !ImGui::IsPopupOpen("Install .pak mod")) ImGui::OpenPopup("Install .pak mod");
    if (g_askReset) { ImGui::OpenPopup("Reset UE4SS files"); g_askReset = false; }
    if (!g_confirmSub.empty() && !ImGui::IsPopupOpen("Open level piece")) ImGui::OpenPopup("Open level piece");
    if (g_askClose) { ImGui::OpenPopup("Close launcher?"); g_askClose = false; }

    auto begin = [](const char* name) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(Px(440), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(24), Px(22)));
        bool open = ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleVar();
        return open;
    };
    auto title = [](const char* icon, const char* t, ImVec4 c) {
        IconBox(icon, c, Px(40)); ImGui::SameLine(0, Px(12)); ImGui::AlignTextToFramePadding();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(4));
        ImGui::PushFont(F_h2); ImGui::TextUnformatted(t); ImGui::PopFont();
    };
    float bw = (Px(440) - Px(48) - Px(10)) / 2;

    if (begin("Install .pak mod")) {
        title(ICON_BOX_OPEN, "Where does this .pak go?", P.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextWrapped("%s", W2U(NameOf(g_pendingPak)).c_str());
        ImGui::TextWrapped("If the mod's page says to put it in LogicMods, it's a Blueprint mod. Otherwise choose Regular.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, Px(4)));
        if (ImGui::Button("Blueprint (LogicMods)", ImVec2(bw, Px(38)))) { InstallPak(g_pendingPak, true); g_pendingPak.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (AccentButton("Regular (~mods)", ImVec2(bw, Px(38)))) { InstallPak(g_pendingPak, false); g_pendingPak.clear(); ImGui::CloseCurrentPopup(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { g_pendingPak.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (begin("Open level piece")) {
        std::string nm = g_confirmSub.substr(g_confirmSub.find_last_of('/') + 1);
        title(ICON_WARN, "This is part of a bigger level", P.warn);
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextWrapped("\"%s\" looks like one piece of a larger level (for example its layout, lighting or sound). "
                           "Games load these pieces inside a main level, so opening it on its own usually gives an empty scene "
                           "or sends you back to the main menu.", nm.c_str());
        ImGui::TextWrapped("To reach that area, open the main level instead, or use the game's own chapter select.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, Px(4)));
        if (AccentButton("Cancel", ImVec2(bw, Px(38))) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { g_confirmSub.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Open anyway", ImVec2(bw, Px(38)))) { std::string p = g_confirmSub; g_confirmSub.clear(); ImGui::CloseCurrentPopup(); OpenLevelCmd(p, true); }
        ImGui::EndPopup();
    }
    if (begin("Reset UE4SS files")) {
        title(ICON_ROTATE, "Reset UE4SS files?", P.err);
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextWrapped("Restores UE4SS, its settings and the built-in mods to the bundled versions. Mods you installed stay installed.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, Px(4)));
        if (ImGui::Button("Cancel", ImVec2(bw, Px(38))) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, P.err); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Mix(P.err, P.text, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_Text, Hex(0xFFFFFF));
        if (ImGui::Button("Reset", ImVec2(bw, Px(38)))) {
            int n = ExtractBundle(true); ReregisterUserMods(); RefreshMods();
            Log(L"Restored " + std::to_wstring(n) + L" UE4SS files.", LOG_OK);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndPopup();
    }
    if (begin("Close launcher?")) {
        title(ICON_POWER, "Close the launcher?", P.warn);
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextWrapped("\"Load mods whenever this game starts\" is on. If you close the launcher, mods won't load the next time the game starts.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, Px(4)));
        if (AccentButton("Keep open", ImVec2(bw, Px(38))) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("Close anyway", ImVec2(bw, Px(38)))) { g_reallyClose = true; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

#include "overlay.inc"

// ---- play as another character
static std::vector<EdActor> g_npcs;
static int g_npcTotal = 0;
static char g_npcFilter[96] = "";
static void SendPossOpts() { BridgeSend("ed_possopts", std::string("tp=") + (g_possTP ? "1" : "0") + " assist=" + (g_possAssist ? "1" : "0")); }
static std::vector<EdSwapModel> g_swapList; static int g_swapTotal = 0; static char g_swapFilter[64] = "";
static std::mutex g_swapPakMx; static std::vector<std::string> g_swapPak; static std::string g_swapPakNote; static std::atomic<int> g_swapPakState{ 0 };
static void CameraViewSection() {
    SectionLabel("Camera view");
    ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("Play any Unreal game in first or third person. First person puts you behind your character's own eyes with its "
                           "real body (look down to see it); third person is an over-the-shoulder camera that stays out of walls.");
    ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
    const char* modes[] = { ICON_VIDEO "  Game's own", ICON_USER "  Third person", ICON_EYE "  First person" };
    float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        bool on = g_viewMode == i;
        if (on ? AccentButton(modes[i], ImVec2(w, Px(36))) : ImGui::Button(modes[i], ImVec2(w, Px(36)))) { g_viewMode = i; SaveSettings(); SendViewMode(); }
    }
    bool changed = false;
    if (g_viewMode != 0) {
        ImGui::SetNextItemWidth(Px(220)); changed |= ImGui::SliderFloat("Field of view", &g_viewFov, 60, 130, "%.0f\xc2\xb0");
        if (g_viewMode == 2) {
            ImGui::SameLine(0, Px(20)); changed |= ImGui::Checkbox("Head bob", &g_viewBob);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: the camera moves with your character's head as it walks and runs.\nOff: steady camera at eye height.");
        } else {
            ImGui::SetNextItemWidth(Px(220)); changed |= ImGui::SliderFloat("Distance", &g_viewDist, 120, 800, "%.0f");
            ImGui::SameLine(0, Px(20)); ImGui::SetNextItemWidth(Px(160)); changed |= ImGui::SliderFloat("Shoulder", &g_viewShoulder, -120, 120, "%.0f");
            ImGui::SetNextItemWidth(Px(220)); changed |= ImGui::SliderFloat("Height", &g_viewHeight, -60, 120, "%.0f");
            ImGui::SameLine(0, Px(20)); changed |= ImGui::Checkbox("Turn to face where I walk", &g_viewFace);
        }
    }
    if (changed && !ImGui::IsMouseDown(0)) { SaveSettings(); SendViewMode(); }
    static bool dragging = false;
    if (ImGui::IsMouseDown(0) && changed) dragging = true;
    if (dragging && !ImGui::IsMouseDown(0)) { dragging = false; SaveSettings(); SendViewMode(); }
}

static void ModelSwapSection() {
    SectionLabel("Model swap");
    ReadEdSwap(g_swapList, g_swapTotal);
    static bool asked = false;
    if (!asked) { asked = true; BridgeSend("ed_swaplist", g_swapFilter); SendPossOpts(); }
    if (g_swapPakState == 0) {
        g_swapPakState = 1;
        std::thread([] { std::string note; auto v = PakCharacters(note); std::lock_guard<std::mutex> lk(g_swapPakMx); g_swapPak = std::move(v); g_swapPakNote = note; g_swapPakState = 2; g_framesLeft = 3; }).detach();
    }
    ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("Change your player model to any character in the game, like RDR2's Model Swap. You're still you: same controls, "
                           "camera, abilities and progress, just with their body and clothes. Characters with a similar skeleton move with your "
                           "animations; others use their own.");
    ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
    if (!g_ed.lookingAs.empty() || !g_ed.swapped.empty() || !g_ed.playingAs.empty()) {
        std::string who = !g_ed.lookingAs.empty() ? g_ed.lookingAs : !g_ed.swapped.empty() ? g_ed.swapped : g_ed.playingAs;
        ImGui::PushStyleColor(ImGuiCol_Text, P.accent); ImGui::AlignTextToFramePadding();
        ImGui::Text(ICON_USER "  Your model: %s", who.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (AccentButton(ICON_ROTATE "  Back to my model")) { BridgeSend("ed_lookback"); BridgeSend("ed_swapback"); }
    }
    ImGui::SetNextItemWidth(-Px(110));
    bool enter = ImGui::InputTextWithHint("##swf", ICON_SEARCH "  Search characters", g_swapFilter, sizeof g_swapFilter, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(ICON_ROTATE " Refresh", ImVec2(-1, 0)) || enter) BridgeSend("ed_swaplist", g_swapFilter);
    // the game's list + character blueprints found in its files
    struct Row { std::string path, name, where; int count; };
    std::vector<Row> rows;
    for (auto& m : g_swapList) rows.push_back({ m.path, m.name, m.where, m.count });
    std::string f = W2U(Lower(U2W(g_swapFilter)));
    if (g_swapPakState == 2) {
        std::lock_guard<std::mutex> lk(g_swapPakMx);
        for (auto& p : g_swapPak) {
            bool dup = false;
            for (auto& m : g_swapList) if (m.path.rfind(p + ".", 0) == 0) { dup = true; break; }
            if (dup) continue;
            std::string n = p.substr(p.find_last_of('/') + 1);
            std::string nice = n.rfind("BP_", 0) == 0 ? n.substr(3) : n;
            for (auto& c : nice) if (c == '_') c = ' ';
            if (!f.empty() && W2U(Lower(U2W(n))).find(f) == std::string::npos) continue;
            rows.push_back({ p, nice, "files", 0 });
        }
    }
    ImGui::PushFont(F_small);
    EdLabel(rows.empty() ? (g_swapPakState < 2 ? "Looking for characters..." : "No characters found. Press Refresh once you're in a level.")
                         : "In the level first, then ones the game has loaded, then ones found in its files (these may not all work).");
    ImGui::PopFont();
    if (ImGui::BeginTable("swap", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, Px(300)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Px(90));
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushFont(F_small); ImGui::TableHeadersRow(); ImGui::PopFont(); ImGui::PopStyleColor();
        ImGuiListClipper clip; clip.Begin((int)rows.size());
        while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            auto& r = rows[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(r.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.path.c_str());
            ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
            if (r.where == "level") ImGui::Text("In level (%d)", r.count);
            else ImGui::TextUnformatted(r.where == "loaded" ? "Loaded" : "Game files");
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (AccentButton("Swap", ImVec2(-1, 0))) BridgeSend("ed_modelswap", r.path);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ================================================================ Player page
// Cheats, movement mode, speeds and position for your character, straight from the launcher.
static void PageCheats() {
    PageHeader("Cheats", "Turn on the game's cheats and developer console, run cheat commands, and open developer menus left in the game.");
    if (!g_bridge.connected) {
        BeginCard("chnone", 0, 20, 18);
        IconBox(ICON_BOLT, P.dim, Px(40));
        ImGui::SameLine(0, Px(14));
        ImGui::BeginGroup();
        ImGui::PushFont(F_med); ImGui::TextUnformatted("No game connected"); ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted("Start your game from the Play page (with mods), then come back here."); ImGui::PopStyleColor();
        ImGui::EndGroup();
        EndCard();
        return;
    }
    float h = ImGui::GetContentRegionAvail().y;
    BeginCard("chcard", h - Px(4), 22, 18);
    ImGui::BeginChild("chscroll", ImVec2(0, 0));
    SectionLabel("Console command");
    TabConsole(Px(70));
    ImGui::Dummy(ImVec2(0, Px(6)));
    ImGui::Separator();
    TabCheats();
    ImGui::EndChild();
    EndCard();
}

static void PagePlayer() {
    PageHeader("Player", "Change your character while you play: god mode, walk / fly / ghost, speeds, gravity and position.");
    if (!g_bridge.connected) {
        BeginCard("plnone", 0, 20, 18);
        IconBox(ICON_USER, P.dim, Px(40));
        ImGui::SameLine(0, Px(14));
        ImGui::BeginGroup();
        ImGui::PushFont(F_med); ImGui::TextUnformatted("No game connected"); ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted("Start your game from the Play page (with mods), then come back here."); ImGui::PopStyleColor();
        ImGui::EndGroup();
        EndCard();
        return;
    }
    float h = ImGui::GetContentRegionAvail().y;
    BeginCard("plcard", h - Px(4), 22, 18);
    ImGui::BeginChild("plscroll", ImVec2(0, 0));
    TabPlayer();
    ImGui::Dummy(ImVec2(0, Px(6)));
    ImGui::Separator();
    CameraViewSection();
    ImGui::Dummy(ImVec2(0, Px(6)));
    ImGui::Separator();
    ModelSwapSection();
    ImGui::Dummy(ImVec2(0, Px(6)));
    ImGui::Separator();
    SectionLabel("Hotkeys");
    ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
    auto kn = [](int k) { return k ? ImGui::GetKeyName((ImGuiKey)k) : "-"; };
    ImGui::Text("Walk %s     Fly %s     Ghost %s", kn(g_edKeys.walk), kn(g_edKeys.fly), kn(g_edKeys.ghost));
    ImGui::TextUnformatted("They work in the game at any time. Change them on the Editor page.");
    ImGui::PopStyleColor();
    ImGui::EndChild();
    EndCard();
}

static std::atomic<int> g_fmState{ 0 }; static std::string g_fmErr;   // 1 downloading, 2 done, 3 failed
static std::wstring PaksFolder() {
    std::wstring t = TargetExe(); if (t.empty()) return L"";
    std::wstring d = DirOf(t);
    for (int i = 0; i < 5 && !d.empty(); i++) {
        if (DirExists(d + L"\\Content\\Paks")) return d + L"\\Content\\Paks";
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && DirExists(d + L"\\" + fd.cFileName + L"\\Content\\Paks")) { FindClose(h); return d + L"\\" + fd.cFileName + L"\\Content\\Paks"; }
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::wstring p = DirOf(d); if (p == d) break; d = p;
    }
    return L"";
}
static std::string EngineVersionFromLog() {
    std::string t; HANDLE h = CreateFileW((ModsDir() + L"\\..\\UE4SS.log").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    char b[65536]; DWORD n = 0; ReadFile(h, b, sizeof b, &n, nullptr); CloseHandle(h); t.assign(b, n);
    size_t k = t.find("Found EngineVersion: "); if (k == std::string::npos) return "";
    size_t e = t.find_first_of("\r\n", k); return t.substr(k + 21, e == std::string::npos ? std::string::npos : e - k - 21);
}
static char g_aesBuf[100] = ""; static std::wstring g_aesFor;
static std::wstring AesSettingKey() { std::wstring n = GameDisplayName(); for (auto& c : n) if (!iswalnum(c)) c = L'_'; return L"Aes_" + n; }
static void AesSync() {            // the key that goes with the current game
    std::wstring g = GameDisplayName();
    if (g == g_aesFor) return;
    g_aesFor = g;
    wchar_t kb[200] = L"";
    if (!g.empty()) GetPrivateProfileStringW(L"Launcher", AesSettingKey().c_str(), L"", kb, 200, g_iniPath.c_str());
    std::wstring k = kb;
    snprintf(g_aesBuf, sizeof g_aesBuf, "%s", W2U(k).c_str());
    ModelSetAesKey(g_aesBuf);
}
static void AesSyncNow() { g_aesFor = L"\x1"; AesSync(); }   // re-read it (it may have been changed since)
static bool g_showFModelCards = false;
static void PageModels() {
    AesSync();
    PageHeader("Models", "Take models (and their animations) out of one Unreal game and put them into another.");
    ImGui::BeginChild("mdscroll", ImVec2(0, 0));
    if (g_showFModelCards) {      // (FModel and the AES key: hidden - the key is still found automatically)
    SectionLabel("1. Export from a game");
    BeginCard("mdexp", 0, 20, 16);
    {
        std::wstring fm = FModelExe();
        ImGui::PushFont(F_med); ImGui::TextUnformatted("FModel"); ImGui::PopFont();
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("FModel opens an Unreal game's packed files and saves models, textures and animations out of them. It's a free separate program (it needs Microsoft's .NET 8 Desktop Runtime).");
        ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, Px(4)));
        if (fm.empty()) {
            ImGui::BeginDisabled(g_fmState == 1);
            if (AccentButton(g_fmState == 1 ? ICON_HOURGLASS "  Downloading FModel..." : ICON_DOWNLOAD "  Download FModel")) {
                g_fmState = 1;
                std::thread([] { std::string e; bool ok = FModelDownload(e); g_fmErr = e; g_fmState = ok ? 2 : 3; g_framesLeft = 3;
                                 Log(ok ? L"FModel downloaded." : U2W("FModel download failed: " + e), ok ? LOG_OK : LOG_ERR); }).detach();
            }
            ImGui::EndDisabled();
            if (g_fmState == 3) { ImGui::SameLine(); ImGui::TextColored(P.warn, "%s", g_fmErr.c_str()); }
        } else {
            if (AccentButton(ICON_ROCKET "  Open FModel")) ShellExecuteW(nullptr, L"open", fm.c_str(), nullptr, DirOf(fm).c_str(), SW_SHOWNORMAL);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FOLDER_OPEN "  Exported files")) { std::wstring e = FModelExports(); MakeDirs(e); OpenFolder(e); }
        }
        std::wstring paks = PaksFolder(); std::string ver = EngineVersionFromLog();
        if (!paks.empty()) {
            ImGui::Dummy(ImVec2(0, Px(4)));
            ImGui::TextColored(P.dim, "Your game's packed files:"); ImGui::SameLine();
            ImGui::TextUnformatted(FitPath(W2U(paks), ImGui::GetContentRegionAvail().x - Px(70)).c_str()); ImGui::SameLine();
            if (ImGui::SmallButton(ICON_COPY " Copy")) ImGui::SetClipboardText(W2U(paks).c_str());
            if (!ver.empty()) ImGui::TextColored(P.dim, "Engine version (pick this in FModel): Unreal Engine %s", ver.c_str());
        }
        ImGui::Dummy(ImVec2(0, Px(4)));
        ImGui::PushFont(F_small); ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("In FModel:\n"
            "  1. Add the game's folder (Directory > Selector), choose its Unreal Engine version, and Load.\n"
            "  2. Settings > Models: set Mesh Export Format to ActorX (psk) to keep animations, or glTF. Turn on Export Materials so textures come too.\n"
            "  3. Find a model (SK_ = characters with skeletons, SM_ = objects) under Content, right-click > Save Model.\n"
            "  4. For animations, right-click an AnimSequence (A_ / Anim_) > Save Animation (.psa).\n"
            "  5. Some games need an AES key to open their files; FModel will ask for it.");
        ImGui::PopTextWrapPos(); ImGui::PopFont();
    }
    EndCard();
    ImGui::Dummy(ImVec2(0, Px(2)));
    BeginCard("mdaes", 0, 20, 14);
    {
        ImGui::PushFont(F_med); ImGui::TextUnformatted(ICON_SHIELD "  Game decryption key (AES)"); ImGui::PopFont();
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("Only needed for games that encrypt their files. Found automatically: when you start a game from the launcher, it looks for the key in the running game and saves it here. You can also paste a key (0x...). Saved for this game only.");
        ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
        ImGui::SetNextItemWidth(-Px(90));
        bool edited = ImGui::InputTextWithHint("##aes", "0x0000000000000000000000000000000000000000000000000000000000000000", g_aesBuf, sizeof g_aesBuf);
        ImGui::SameLine();
        bool valid = ModelSetAesKey(g_aesBuf);
        ImGui::TextColored(g_aesBuf[0] == 0 ? P.dim : valid ? P.ok : P.warn, "%s", g_aesBuf[0] == 0 ? "none" : valid ? ICON_CHECK " ok" : "64 digits");
        if (edited && !GameDisplayName().empty()) WritePrivateProfileStringW(L"Launcher", AesSettingKey().c_str(), U2W(g_aesBuf).c_str(), g_iniPath.c_str());
        ImGui::Dummy(ImVec2(0, Px(2)));
        if (g_keyHunt == 1) { ImGui::BeginDisabled(); ImGui::Button(ICON_HOURGLASS "  Looking in the running game..."); ImGui::EndDisabled(); }
        else if (ImGui::Button(ICON_SEARCH "  Find the key in the running game")) StartKeyHunt(true);
    }
    EndCard();
    ImGui::Dummy(ImVec2(0, Px(2)));
    }
    SectionLabel(g_showFModelCards ? "2. Import into any game" : "1. Import into any game");
    BeginCard("mdimp", 0, 20, 16);
    {
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("Quickest way: in the in-game editor, select anything - an object, a character, something built from several pieces - and press Export model (Inspector tab). Works in Unreal 4 games (encrypted ones too: their key is found automatically when you start the game from the launcher).");
        ImGui::Dummy(ImVec2(0, Px(2)));
        ImGui::TextUnformatted("Imported models are built inside the game you're playing, out of one piece per bone, coloured from their texture. Animations play by moving the pieces. "
                               "Animations from FModel (.psa) next to the model are added automatically. Works in Unreal Engine 4.25 and newer games that include Unreal's procedural mesh feature (most do). "
                               "Added models are saved with your level edits. Imported models belong to the game you have chosen: to use one in another game, "
                               "export it as .glb or .fbx (Export model in the editor) and import that file in the other game.");
        ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, Px(4)));
        {   // how detailed an imported model may be; bigger ones are simplified down to this
            static int maxK = -1; if (maxK < 0) maxK = ModelMaxTriangles() / 1000;
            ImGui::SetNextItemWidth(Px(360));
            ImGui::SliderInt("##maxtris", &maxK, 50, 3000, "Most triangles per model: %dk");
            if (ImGui::IsItemDeactivatedAfterEdit()) { ModelSetMaxTriangles(maxK * 1000); maxK = ModelMaxTriangles() / 1000; SaveSettings(); }
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Models with more triangles than this are simplified down to it automatically when they're imported or exported.\n"
                                                          "Higher keeps more detail but takes longer to build in the game and uses more memory (over about 1,500k can stall the game for a while).");
        }
        ImGui::Dummy(ImVec2(0, Px(4)));
        ImportedModelsUI(false);
    }
    EndCard();
    ImGui::Dummy(ImVec2(0, Px(2)));
    SectionLabel(g_showFModelCards ? "3. Materials" : "2. Materials");
    BeginCard("mdmats", 0, 20, 16);
    MaterialsUI(false);
    EndCard();
    ImGui::EndChild();
}

static void PageEditor() {
    PageHeader("Editor", "An in-game editor in its own window, for changing things while you play. Press F2 in the game to open it.");
    ImGui::BeginChild("edscroll", ImVec2(0, 0));
    BeginCard("edstatus", 0, 20, 16);
    {
        bool conn = g_bridge.connected;
        IconBox(ICON_PEN_RULER, conn ? P.accent : P.dim, Px(48));
        ImGui::SameLine(0, Px(14));
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(2));
        ImGui::PushFont(F_h2); ImGui::TextUnformatted(!conn ? "Game not connected" : g_ovVisible ? "Editor is open" : "Ready - press F2 in the game"); ImGui::PopFont();
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextUnformatted(!conn ? "Launch the game with mods from the Play page first." : "F2 opens and closes the editor over the game window.");
        ImGui::PopStyleColor(); ImGui::PopFont();
        ImGui::EndGroup();
        float bw = Px(170);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (Px(48) - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::BeginDisabled(!conn);
        if (AccentButton(g_ovVisible ? ICON_POWER "  Close editor" : ICON_WINDOW "  Open editor", ImVec2(bw, 0))) { if (g_ovVisible) OvHide(); else OvShow(); }
        ImGui::EndDisabled();
    }
    EndCard();
    ImGui::Dummy(ImVec2(0, Px(2)));
    BeginCard("edshare", 0, 20, 14);
    {
        ImGui::PushFont(F_med); ImGui::TextUnformatted(ICON_LINK "  Share your edits"); ImGui::PopFont();
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
        if (g_ed.layoutMap.empty()) ImGui::TextUnformatted("Save what you've moved, hidden, deleted or added as a .uelayout file and send it to people. They load it here (or in the editor's World tab) and get the same changes in their game. Start the game with mods and go into a level to share.");
        else ImGui::Text("Level: %s   \xc2\xb7   %d saved change%s. Anyone with this launcher and the same game can load the file you share.",
                         g_ed.layoutMap.substr(g_ed.layoutMap.find_last_of('/') + 1).c_str(), g_ed.layoutCount, g_ed.layoutCount == 1 ? "" : "s");
        ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, Px(2)));
        LayoutShareButtons(false);
    }
    EndCard();
    ImGui::Dummy(ImVec2(0, Px(2)));
    BeginCard("ednopause", 0, 20, 14);
    {
        if (ToggleSwitch("##nopause", &g_edNoPause)) {
            SaveSettings();
            if (g_ovVisible) { OvHide(); OvShow(); }
        }
        ImGui::SameLine(0, Px(12)); ImGui::BeginGroup();
        ImGui::PushFont(F_med); ImGui::TextUnformatted("Keep the game running while editing"); ImGui::PopFont();
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(g_edNoPause
            ? "The game keeps focus so it doesn't pause. While the editor is open your mouse and keyboard go to the editor window and the axis on the game (with the editor's own cursor), not the game. Alt+Tab still works."
            : "The editor takes focus when it opens. Some games pause or open their menu when that happens.");
        ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
        ImGui::EndGroup();
    }
    EndCard();
    ImGui::Dummy(ImVec2(0, Px(2)));
    SectionLabel("What's in it");
    BeginCard("edwhat", 0, 20, 16);
    {
        struct { const char* icon; const char* title; const char* desc; } items[] = {
            { ICON_USER, "Player", "God mode, walk / fly / ghost through walls, walk speed, jump height, gravity, game speed, teleport." },
            { ICON_CUBE, "World", "Every actor in the level, nearest first, with a filter. Aim and click the crosshair to pick what you're looking at." },
            { ICON_PEN, "Inspector", "Move, rotate, scale, hide or delete the selected actor, teleport to it or bring it to you, and edit its values (health, speeds, flags...)." },
            { ICON_VIDEO, "Camera", "Freecam on/off and speed, field of view, camera position." },
            { ICON_TERMINAL, "Console", "Run Unreal console commands, with quick buttons and history." } };
        for (auto& it : items) {
            ImGui::PushStyleColor(ImGuiCol_Text, P.accent); ImGui::TextUnformatted(it.icon); ImGui::PopStyleColor();
            ImGui::SameLine(Px(34)); ImGui::BeginGroup();
            ImGui::PushFont(F_med); ImGui::TextUnformatted(it.title); ImGui::PopFont();
            ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(it.desc); ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, Px(2)));
        }
    }
    EndCard();
    ImGui::Dummy(ImVec2(0, Px(2)));
    SectionLabel("Hotkeys");
    BeginCard("edkeys", 0, 20, 14);
    {
        static int capturing = -1;
        struct { const char* name; const char* desc; int* key; int def; } rows[] = {
            { "Select tool", "Just click to select, no handles", &g_edKeys.select, ImGuiKey_Q },
            { "Move tool", "Arrows to drag along X / Y / Z", &g_edKeys.move, ImGuiKey_W },
            { "Rotate tool", "Rings to turn around each axis", &g_edKeys.rotate, ImGuiKey_E },
            { "Scale tool", "Boxes to stretch, centre box = uniform", &g_edKeys.scale, ImGuiKey_R },
            { "Delete selected", "Press twice to confirm", &g_edKeys.del, ImGuiKey_Delete },
            { "Hide / show selected", "", &g_edKeys.hide, ImGuiKey_H },
            { "Deselect", "", &g_edKeys.deselect, ImGuiKey_Escape },
            { "Snap to ground", "Drop the selected object onto what's below it", &g_edKeys.snapGround, ImGuiKey_End },
            { "Put on another object", "Then click where it should go (floor, table, wall...)", &g_edKeys.snapOnto, ImGuiKey_G },
            { "Walk", "Player movement. Also works while playing", &g_edKeys.walk, ImGuiKey_F6 },
            { "Fly", "Player flies. Also works while playing", &g_edKeys.fly, ImGuiKey_F7 },
            { "Ghost", "Fly through walls. Also works while playing", &g_edKeys.ghost, ImGuiKey_F8 } };
        int n = (int)(sizeof rows / sizeof rows[0]);
        // waiting for a key?
        if (capturing >= 0) {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                ImGuiKey key = (ImGuiKey)k;
                if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
                if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
                    key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper ||
                    (key >= ImGuiKey_ReservedForModCtrl && key <= ImGuiKey_ReservedForModSuper)) continue;
                if (!ImGui::IsKeyPressed(key, false)) continue;
                if (key == ImGuiKey_F2) { Log(L"F2 opens and closes the editor, so it can't be used for this.", LOG_WARN); capturing = -1; break; }
                for (int r = 0; r < n; r++) if (r != capturing && *rows[r].key == k) *rows[r].key = 0;   // move it, don't duplicate
                *rows[capturing].key = k;
                SaveSettings();
                Log(U2W(std::string(rows[capturing].name) + " is now " + ImGui::GetKeyName(key)), LOG_OK);
                capturing = -1;
                break;
            }
        }
        float keyW = Px(130), btnW = Px(96);
        for (int r = 0; r < n; r++) {
            ImGui::PushID(r);
            ImGui::BeginGroup();
            ImGui::PushFont(F_med); ImGui::TextUnformatted(rows[r].name); ImGui::PopFont();
            if (rows[r].desc[0]) { ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted(rows[r].desc); ImGui::PopStyleColor(); ImGui::PopFont(); }
            ImGui::EndGroup();
            float y = ImGui::GetItemRectMin().y, h = ImGui::GetItemRectSize().y;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - keyW - btnW - Px(10));
            float by = y + (h - ImGui::GetFrameHeight()) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, by));
            const char* kn = capturing == r ? "Press a key..." : (*rows[r].key ? ImGui::GetKeyName((ImGuiKey)*rows[r].key) : "(none)");
            ImGui::PushStyleColor(ImGuiCol_Button, capturing == r ? Alpha(P.accent, 0.25f) : P.card2);
            ImGui::Button(kn, ImVec2(keyW, 0));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, by));
            if (capturing == r) { if (ImGui::Button("Cancel", ImVec2(btnW, 0))) capturing = -1; }
            else if (ImGui::Button("Change", ImVec2(btnW, 0))) capturing = r;
            ImGui::PopID();
            if (r + 1 < n) ImGui::Separator();
        }
        ImGui::Dummy(ImVec2(0, Px(2)));
        if (ImGui::Button(ICON_ROTATE "  Reset to defaults")) { for (auto& rw : rows) *rw.key = rw.def; capturing = -1; SaveSettings(); }
        ImGui::SameLine(); ImGui::AlignTextToFramePadding();
        ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextUnformatted("Hold Ctrl while dragging to snap (10 units / 15\xC2\xB0 / 0.1x).");
        ImGui::PopStyleColor(); ImGui::PopFont();
    }
    EndCard();

    ImGui::Dummy(ImVec2(0, Px(2)));
    SectionLabel("Good to know");
    BeginCard("edtips", 0, 20, 14);
    ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("The editor draws over the game, so the game must be in windowed or borderless (\"windowed fullscreen\") mode - exclusive fullscreen hides it.");
    ImGui::TextUnformatted("While the editor is open it has the keyboard and mouse; press F2 again to go back to playing.");
    ImGui::TextUnformatted("If the editor's cursor doesn't move properly in a particular game, turn off \"Keep the game running while editing\".");
    ImGui::TextUnformatted("Changes are made to the running game only - nothing is saved to your files, and reloading the level undoes them.");
    ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
    EndCard();
    ImGui::EndChild();
}

// ================================================================ frame
// imported models are kept per game: follow the chosen game
static void FollowGameForModels() {
    static std::wstring last = L"?";
    std::wstring cur = TargetExe();
    if (cur == last) return;
    last = cur;
    ModelSetGame(cur.empty() ? L"" : GameDisplayName());
    g_impLoaded = false;
}
static void DrawUI() {
    FollowGameForModels();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos); ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PushFont(F_body);
    Sidebar();
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(30), Px(26)));
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    UpdateBanner();
    switch (g_page) {
    case PG_PLAY: PagePlay(); break;
    case PG_MODS: PageMods(); break;
    case PG_LEVELS: PageLevels(); break;
    case PG_FREECAM: PageFreecam(); break;
    case PG_EDITOR: PageEditor(); break;
    case PG_PLAYER: PagePlayer(); break;
    case PG_CHEATS: PageCheats(); break;
    case PG_LOOK: PageLook(); break;
    case PG_MODELS: PageModels(); break;
    case PG_LOG: PageLog(); break;
    }
    ImGui::EndChild();
    Popups();
    ImGui::PopFont();
    ImGui::End();
}

// The rainbow outline: colours run round the edge of the window. It only moves while the launcher is the window
// in front (so it never costs the game anything while you play); otherwise it stays still.
static void DrawRgbBorder() {
    if (!g_rgbBorder) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
    static double phase = 0; static double last = ImGui::GetTime();
    double now = ImGui::GetTime();
    bool front = GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd);
    if (front && g_rgbSpeed > 0) { phase += (now - last) * 0.18 * g_rgbSpeed / 100.0; g_animating = true; }
    last = now;
    float w = Px((float)g_rgbWidth), h = w * 0.5f;
    ImVec2 a(vp->Pos.x + h, vp->Pos.y + h), b(vp->Pos.x + vp->Size.x - h, vp->Pos.y + vp->Size.y - h);
    float W = b.x - a.x, H = b.y - a.y, per = 2 * (W + H);
    if (per <= 0) return;
    auto pt = [&](float d) {        // a point on the edge, going clockwise from the top-left corner
        d = fmodf(d, per); if (d < 0) d += per;
        if (d < W) return ImVec2(a.x + d, a.y); d -= W;
        if (d < H) return ImVec2(b.x, a.y + d); d -= H;
        if (d < W) return ImVec2(b.x - d, b.y); d -= W;
        return ImVec2(a.x, b.y - d);
    };
    auto col = [&](float d, float alpha) {
        float hue = fmodf((float)(d / per - phase), 1.f); if (hue < 0) hue += 1;
        float r, g, bl; ImGui::ColorConvertHSVtoRGB(hue, 0.85f, 1.f, r, g, bl);
        return ImGui::GetColorU32(ImVec4(r, g, bl, alpha));
    };
    const int N = 240;
    float step = per / N;
    for (int pass = 0; pass < 2; pass++) {       // a soft glow, then the line itself
        float th = pass ? w : w * 2.6f, al = pass ? 1.f : 0.22f;
        for (int i = 0; i < N; i++) {
            float d0 = i * step, d1 = d0 + step;
            // split at the corners so the line goes round them
            float corners[4] = { W, W + H, 2 * W + H, per };
            float e = d1;
            for (float c : corners) if (c > d0 + 0.01f && c < d1) { e = c; break; }
            dl->AddLine(pt(d0), pt(e), col(d0, al), th);
            if (e < d1) dl->AddLine(pt(e), pt(d1), col(e, al), th);
        }
    }
}

static void RenderFrame(bool vsync = true) {
    if (g_resizeW) { CleanupRT(); g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0); g_resizeW = g_resizeH = 0; CreateRT(); }
    if (g_fontsDirty) { BuildFonts(); OvBuildFonts(); g_fontsDirty = false; }
    if (g_styleDirty) { ApplyStyle(); ApplyOverlayStyle(); g_styleDirty = false; }
    g_animating = false;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUI();
    DrawRgbBorder();
    ImGui::Render();
    float clr[4] = { P.bg.x, P.bg.y, P.bg.z, 1 };
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(vsync ? 1 : 0, 0);

    auto jobs = std::move(g_deferred); g_deferred.clear();
    for (auto& j : jobs) j();
    if (!jobs.empty()) g_framesLeft = 3;
    if (g_reallyClose) DestroyWindow(g_hwnd);
}

static void UpdateDpi() {
    typedef UINT(WINAPI * GetDpiForWindow_t)(HWND);
    static auto fn = (GetDpiForWindow_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT dpi = fn ? fn(g_hwnd) : 96;
    g_dpiScale = dpi / 96.f;
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
        g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp);
        if (g_inSizeMove && g_swap) RenderFrame();
        return 0;
    case WM_ENTERSIZEMOVE: g_inSizeMove = true; return 0;
    case WM_EXITSIZEMOVE: g_inSizeMove = false; return 0;
    case WM_GETMINMAXINFO: { auto mm = (MINMAXINFO*)lp; mm->ptMinTrackSize = { (LONG)(900 * g_dpiScale), (LONG)(640 * g_dpiScale) }; return 0; }
    case WM_DPICHANGED: {
        UpdateDpi(); g_fontsDirty = g_styleDirty = true;
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND: if ((wp & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_CLOSE:
        if (g_autoInject && !g_reallyClose) { g_askClose = true; g_framesLeft = 3; return 0; }
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // debug privilege lets us open games running under other accounts when elevated
    HANDLE tok;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1;
        if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) { tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED; AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr); }
        CloseHandle(tok);
    }

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"UE4SSLauncher3", nullptr };
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"UE4SS Launcher", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, inst, nullptr);
    UpdateDpi();
    {   // size & centre for this monitor's DPI
        int w = (int)(1120 * g_dpiScale), h = (int)(780 * g_dpiScale);
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        w = std::min(w, (int)(wa.right - wa.left)); h = std::min(h, (int)(wa.bottom - wa.top));
        SetWindowPos(g_hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER);
    }

    SetupDataDir(inst);
    LoadSettings();
    g_runtimeMissing = RuntimeMissing();
    if (g_runtimeMissing) Log(L"The Microsoft Visual C++ runtime is missing. UE4SS needs it to load.", LOG_WARN);
    Log(L"Ready. Choose your game, then click Launch game with mods.");
    if (!g_gameExe.empty()) Log(L"Game: " + GameDisplayName());
    if (g_autoInject) Log(L"Waiting for " + NameOf(TargetExe()) + L" to start...");

    if (!CreateDevice(g_hwnd)) {
        MessageBoxW(nullptr, L"Couldn't start DirectX 11, which this launcher uses to draw its window.", L"UE4SS Launcher", MB_ICONERROR);
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    LoadLogo();
    g_mainCtx = ImGui::GetCurrentContext();
    ApplyStyle(); g_styleDirty = false;
    if (!OvCreate(inst)) Log(L"Couldn't create the in-game editor.", LOG_WARN);
    ApplyStyle(); g_styleDirty = false;

    BridgeInit();
    WriteFreecamConfig(g_fc);
    RefreshProcs(); RefreshMods(); ReloadLevels();
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    bool running = true;
    while (running) {
        // Sleep until something happens; keep ticking while animating or watching for the game.
        if (g_framesLeft <= 0 && !g_animating && !g_ovVisible)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, (IsWatching() || g_pendingCmd) ? 250 : 500, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) running = false;
            TranslateMessage(&m); DispatchMessageW(&m);
            g_framesLeft = 3;
        }
        if (!running) break;
        if (g_f2Toggle.exchange(false)) { if (g_ovVisible) OvHide(); else OvShow(); g_framesLeft = 3; }
        if (g_page == PG_CHEATS && g_bridge.connected) {   // Cheats page: keep it marked as open, redraw when the game's answer arrives
            static ULONGLONG lastCh = 0; static int pakState = 0;
            if (GetTickCount64() - lastCh > 300) {
                lastCh = GetTickCount64();
                if (g_cheatsSeen) g_cheatsSeen = GetTickCount64();
                if (ReadEdCheats(g_cheats)) g_framesLeft = 3;
                if (pakState != g_devPakState) { pakState = g_devPakState; g_framesLeft = 3; }
            }
        }
        {   // Player page: ask the game for live player info, and redraw when it changes
            static bool watching = false;
            bool want = (g_page == PG_PLAYER || g_page == PG_EDITOR) && g_bridge.connected && !g_ovVisible;
            if (want != watching) { BridgeSend("ed_watch", want ? "1" : "0"); watching = want; }
            static ULONGLONG lastPl = 0;
            if (want && GetTickCount64() - lastPl > 200) {
                lastPl = GetTickCount64();
                std::string before = g_ed.pawn; float bl = g_ed.loc[0], bw = g_ed.walk; bool bg = g_ed.god; int bm = g_ed.mode; bool bgh = g_ed.ghost;
                OvPoll();
                if (before != g_ed.pawn || bl != g_ed.loc[0] || bw != g_ed.walk || bg != g_ed.god || bm != g_ed.mode || bgh != g_ed.ghost) g_framesLeft = 3;
            }
        }
        if (int mv = g_moveReq.exchange(0)) {   // walk / fly / ghost hotkey while playing
            static const char* modes[] = { "", "walk", "fly", "ghost" };
            BridgeSend("ed_move", modes[mv]);
            Log(mv == 1 ? L"Walking" : mv == 2 ? L"Flying" : L"Ghost: flying through walls", LOG_OK);
        }
        BridgeFlush();
        UpdTick();
        { static ULONGLONG startAt = GetTickCount64(); static bool checked = false;   // start-up check, once the window is up
          if (!checked && GetTickCount64() - startAt > 1500) { checked = true; if (g_updAuto) UpdCheck(g_updChannel, true); } }
        { UpdInfo ui = UpdGet(); static int lastPh = -1; if (ui.phase != UPD_IDLE || ui.phase != lastPh) g_framesLeft = 3; lastPh = ui.phase; }
        ULONGLONG now = GetTickCount64();
        if (now - g_lastWatch >= 500) { g_lastWatch = now; WatchTick(); }
        if (now - g_lastBridgePoll >= 500) {
            g_lastBridgePoll = now;
            bool was = g_bridge.connected; std::string m = g_bridge.currentMap; int pend = g_pendingCmd;
            PollBridge();
            if (was != g_bridge.connected || m != g_bridge.currentMap || pend != g_pendingCmd) g_framesLeft = 3;
        }
        if (g_page == PG_PLAY && now - g_lastProcRefresh >= 3000) RefreshProcs();
        if (g_modsDirty.exchange(false)) { RefreshMods(); g_framesLeft = 3; }
        if (g_ovVisible) {
            RenderOverlay();                                   // paced by the overlay's vsync
            static int skip = 0;                               // keep the launcher window updating, just less often
            if (!IsIconic(g_hwnd) && (g_framesLeft > 0 || ++skip % 15 == 0)) { RenderFrame(false); if (g_framesLeft > 0) g_framesLeft--; }
            continue;
        }
        if (IsIconic(g_hwnd)) continue;
        RenderFrame();
        if (g_framesLeft > 0) g_framesLeft--;
    }

    SaveSettings();
    // if anything below hangs (a stuck driver or hook), the launcher still goes away within a few seconds
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD { Sleep(4000); TerminateProcess(GetCurrentProcess(), 0); return 0; }, nullptr, 0, nullptr);
    OvHide(false);
    OvShutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    CoUninitialize();
    // End the process right here. Background work (hooks, picture readers, downloads) must never keep a closed
    // launcher alive in the background, where it would still answer F2 and keep its file locked.
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
