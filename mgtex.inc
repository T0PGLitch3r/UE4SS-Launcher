// ---- Texture library and Texture Editor
// Pictures you import (PNG, JPG, TGA, BMP, HDR) are kept in LauncherBridge\texture library: the original in
// "sources", your settings in <name>.uetex, and the processed result in <name>.png (what material graphs use).
// Processing is real and non-destructive: flips, brightness / contrast / saturation / hue / gamma, invert, normal
// map fixes (flip green, rebuild blue), grayscale and alpha packing, resizing, making a normal map from a height map.
// The settings also say how material graphs should sample it (type, filtering, tiling, offset, rotation).

enum { TX_BASECOLOR, TX_NORMAL, TX_ROUGH, TX_METAL, TX_AO, TX_HEIGHT, TX_EMISSIVE, TX_MASKS, TX_OTHER, TX_TYPES };
static const char* g_txTypeName[TX_TYPES] = { "Base Color", "Normal Map", "Roughness", "Metallic", "Ambient Occlusion", "Height", "Emissive", "Masks (packed)", "Other" };
enum { TXC_DEFAULT, TXC_NORMAL, TXC_MASKS, TXC_GRAY, TXC_HDR, TXC_ALPHA, TXC_COUNT };
static const char* g_txCompName[TXC_COUNT] = { "Default (colour, sRGB)", "Normal map (RG, blue rebuilt)", "Masks (linear, no sRGB)", "Grayscale", "HDR (keeps light values)", "Alpha only" };
struct TxSet {
    int type = TX_BASECOLOR; bool srgb = true; int comp = TXC_DEFAULT; int filter = 0; int maxSize = 0;
    float tile[2] = { 1, 1 }, off[2] = { 0, 0 }, rot = 0;
    float bright = 0, contrast = 1, sat = 1, hue = 0, gamma = 1, exposure = 0;
    bool invert = false, flipG = false, flipX = false, flipY = false;
};
struct TxAsset { std::string name; std::wstring src, out, meta; TxSet s; int w = 0, h = 0; bool hdr = false; std::string srcFmt; };
static std::vector<TxAsset> g_tx; static bool g_txLoaded = false; static int g_txSel = -1;
static char g_txFilter[64] = "";
static std::wstring TxDir() { return DirOf(ModelsRoot()) + L"\\texture library"; }
static int TxSampler(int type) { return type == TX_NORMAL ? 1 : (type == TX_BASECOLOR || type == TX_EMISSIVE) ? 0 : 2; }
static void TxDefaults(TxSet& s) {
    s.srgb = s.type == TX_BASECOLOR || s.type == TX_EMISSIVE || s.type == TX_OTHER;
    s.comp = s.type == TX_NORMAL ? TXC_NORMAL : (s.type == TX_ROUGH || s.type == TX_METAL || s.type == TX_AO || s.type == TX_HEIGHT) ? TXC_GRAY : s.type == TX_MASKS ? TXC_MASKS : TXC_DEFAULT;
}
// a type guessed from the file name (T_Brick_N, rock_roughness, ...)
static int TxGuessType(const std::string& name) {
    std::string l = W2U(Lower(U2W(name)));
    auto ends = [&](const char* s) { size_t n = strlen(s); return l.size() >= n && l.compare(l.size() - n, n, s) == 0; };
    auto has = [&](const char* s) { return l.find(s) != std::string::npos; };
    if (has("normal") || ends("_n") || ends("_nm") || ends("_nrm") || ends("_nor")) return TX_NORMAL;
    if (has("rough") || ends("_r")) return TX_ROUGH;
    if (has("metal") || ends("_m")) return TX_METAL;
    if (has("occlusion") || ends("_ao")) return TX_AO;
    if (has("height") || has("displace") || has("bump") || ends("_h") || ends("_disp")) return TX_HEIGHT;
    if (has("emiss") || has("glow") || ends("_e")) return TX_EMISSIVE;
    if (ends("_orm") || ends("_mra") || ends("_rma") || ends("_arm") || has("mask")) return TX_MASKS;
    return TX_BASECOLOR;
}
static std::string TxSetLine(const TxSet& s) {
    char b[512];
    snprintf(b, sizeof b, "set\t%d\t%d\t%d\t%d\t%d\t%g,%g,%g,%g,%g\t%g,%g,%g,%g,%g,%g\t%d,%d,%d,%d",
             s.type, s.srgb ? 1 : 0, s.comp, s.filter, s.maxSize, s.tile[0], s.tile[1], s.off[0], s.off[1], s.rot,
             s.bright, s.contrast, s.sat, s.hue, s.gamma, s.exposure, s.invert ? 1 : 0, s.flipG ? 1 : 0, s.flipX ? 1 : 0, s.flipY ? 1 : 0);
    return b;
}
static bool TxParseSet(const std::vector<std::string>& t, TxSet& s) {
    if (t.size() < 9 || t[0] != "set") return false;
    s.type = std::clamp(atoi(t[1].c_str()), 0, TX_TYPES - 1); s.srgb = atoi(t[2].c_str()) != 0; s.comp = std::clamp(atoi(t[3].c_str()), 0, TXC_COUNT - 1);
    s.filter = std::clamp(atoi(t[4].c_str()), 0, 2); s.maxSize = atoi(t[5].c_str());
    sscanf(t[6].c_str(), "%f,%f,%f,%f,%f", &s.tile[0], &s.tile[1], &s.off[0], &s.off[1], &s.rot);
    sscanf(t[7].c_str(), "%f,%f,%f,%f,%f,%f", &s.bright, &s.contrast, &s.sat, &s.hue, &s.gamma, &s.exposure);
    int a = 0, b = 0, c = 0, d = 0; sscanf(t[8].c_str(), "%d,%d,%d,%d", &a, &b, &c, &d); s.invert = a; s.flipG = b; s.flipX = c; s.flipY = d;
    return true;
}
static void TxSaveMeta(const TxAsset& a) {
    std::string o = "UETEX 1\nsrc\t" + W2U(a.src.substr(a.src.find_last_of(L"\\/") + 1)) + "\n" + TxSetLine(a.s) + "\n";
    HANDLE h = CreateFileW(a.meta.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, o.data(), (DWORD)o.size(), &w, nullptr); CloseHandle(h); }
}
static void TxReload() {
    std::wstring sel = g_txSel >= 0 && g_txSel < (int)g_tx.size() ? g_tx[g_txSel].meta : L"";
    g_tx.clear(); g_txLoaded = true; g_txSel = -1;
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((TxDir() + L"\\*.uetex").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        TxAsset a; std::wstring base = fd.cFileName; base = base.substr(0, base.size() - 6);
        a.name = W2U(base); a.meta = TxDir() + L"\\" + fd.cFileName; a.out = TxDir() + L"\\" + base + L".png";
        std::string d; if (!MeReadAll(a.meta, d)) continue;
        std::istringstream ss(d); std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto t = MeTabs(line);
            if (t.size() >= 2 && t[0] == "src") a.src = TxDir() + L"\\sources\\" + U2W(t[1]);
            else TxParseSet(t, a.s);
        }
        size_t dot = a.src.find_last_of(L'.'); a.srcFmt = dot == std::wstring::npos ? "" : W2U(Lower(a.src.substr(dot + 1)));
        a.hdr = a.srcFmt == "hdr";
        g_tx.push_back(a);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(g_tx.begin(), g_tx.end(), [](const TxAsset& a, const TxAsset& b) { return W2U(Lower(U2W(a.name))) < W2U(Lower(U2W(b.name))); });
    for (int i = 0; i < (int)g_tx.size(); i++) if (g_tx[i].meta == sel) g_txSel = i;
}
static int TxFind(const std::string& value) {     // the library texture a graph's "file:" value is
    if (value.rfind("file:", 0) != 0) return -1;
    std::wstring p = Lower(U2W(value.substr(5)));
    for (int i = 0; i < (int)g_tx.size(); i++) if (Lower(g_tx[i].out) == p) return i;
    return -1;
}

// ---- processing (in the background: big pictures take a moment)
static float TxToLin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float TxToSrgb(float c) { c = std::clamp(c, 0.f, 1.f); return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }
static void TxHueSat(float& r, float& g, float& b, float hueDeg, float sat) {
    float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn, h = 0, s = mx > 0 ? d / mx : 0, v = mx;
    if (d > 1e-6f) { if (mx == r) h = fmodf((g - b) / d, 6.f); else if (mx == g) h = (b - r) / d + 2; else h = (r - g) / d + 4; h *= 60; if (h < 0) h += 360; }
    h = fmodf(h + hueDeg + 360.f, 360.f); s = std::clamp(s * sat, 0.f, 1.f);
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60, 2.f) - 1)), m = v - c, rr = 0, gg = 0, bb = 0;
    if (h < 60) { rr = c; gg = x; } else if (h < 120) { rr = x; gg = c; } else if (h < 180) { gg = c; bb = x; } else if (h < 240) { gg = x; bb = c; } else if (h < 300) { rr = x; bb = c; } else { rr = c; bb = x; }
    r = rr + m; g = gg + m; b = bb + m;
}
// the processed picture (floats, as stored: sRGB values for colour types, plain values for the rest)
static bool TxProcess(const TxAsset& a, std::vector<float>& px, int& W, int& H, std::string& err) {
    bool hdr = false;
    if (!ImageLoadAny(a.src, px, W, H, hdr, err)) return false;
    const TxSet& s = a.s;
    // HDR source: exposure, then (for the 8-bit result) colour types are encoded like any picture
    if (hdr) {
        float k = powf(2.f, s.exposure);
        for (size_t i = 0; i < px.size(); i++) if (i % 4 != 3) { float v = px[i] * k; px[i] = s.comp == TXC_HDR ? v : (s.srgb ? TxToSrgb(v) : std::clamp(v, 0.f, 1.f)); }
    }
    // resize to fit the maximum size (averaging, so it stays smooth)
    if (s.maxSize > 0 && std::max(W, H) > s.maxSize) {
        float sc = (float)s.maxSize / std::max(W, H);
        int nw = std::max(1, (int)lroundf(W * sc)), nh = std::max(1, (int)lroundf(H * sc));
        std::vector<float> o((size_t)nw * nh * 4, 0);
        for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
            int x0 = x * W / nw, x1 = std::max(x0 + 1, (x + 1) * W / nw), y0 = y * H / nh, y1 = std::max(y0 + 1, (y + 1) * H / nh);
            float acc[4] = { 0, 0, 0, 0 }; int n = 0;
            for (int yy = y0; yy < y1 && yy < H; yy++) for (int xx = x0; xx < x1 && xx < W; xx++) { for (int k = 0; k < 4; k++) acc[k] += px[((size_t)yy * W + xx) * 4 + k]; n++; }
            for (int k = 0; k < 4; k++) o[((size_t)y * nw + x) * 4 + k] = n ? acc[k] / n : 0;
        }
        px.swap(o); W = nw; H = nh;
    }
    // flips
    if (s.flipX) for (int y = 0; y < H; y++) for (int x = 0; x < W / 2; x++) for (int k = 0; k < 4; k++) std::swap(px[((size_t)y * W + x) * 4 + k], px[((size_t)y * W + (W - 1 - x)) * 4 + k]);
    if (s.flipY) for (int y = 0; y < H / 2; y++) for (int x = 0; x < W; x++) for (int k = 0; k < 4; k++) std::swap(px[((size_t)y * W + x) * 4 + k], px[((size_t)(H - 1 - y) * W + x) * 4 + k]);
    // adjustments
    bool adj = s.bright != 0 || s.contrast != 1 || s.sat != 1 || s.hue != 0 || s.gamma != 1 || s.invert;
    for (size_t i = 0, n = (size_t)W * H; i < n; i++) {
        float* p = &px[i * 4];
        if (adj && s.comp != TXC_NORMAL) {
            if (s.hue != 0 || s.sat != 1) TxHueSat(p[0], p[1], p[2], s.hue, s.sat);
            for (int k = 0; k < 3; k++) {
                float v = p[k];
                if (s.gamma != 1) v = powf(std::max(v, 0.f), 1.f / s.gamma);
                v = (v - 0.5f) * s.contrast + 0.5f + s.bright;
                if (s.invert) v = 1 - v;
                p[k] = s.comp == TXC_HDR ? std::max(v, 0.f) : std::clamp(v, 0.f, 1.f);
            }
        }
        if (s.flipG) p[1] = 1 - p[1];
        switch (s.comp) {
        case TXC_NORMAL: { float x = p[0] * 2 - 1, y = p[1] * 2 - 1; float z = sqrtf(std::max(0.f, 1 - x * x - y * y)); p[2] = z * 0.5f + 0.5f; p[3] = 1; break; }
        case TXC_GRAY: { float l = s.srgb ? TxToSrgb(0.2126f * TxToLin(p[0]) + 0.7152f * TxToLin(p[1]) + 0.0722f * TxToLin(p[2])) : 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; p[0] = p[1] = p[2] = l; p[3] = 1; break; }
        case TXC_ALPHA: p[0] = p[1] = p[2] = p[3]; p[3] = 1; break;
        default: break;
        }
    }
    return true;
}
// a normal map from a height map: the slope of the brightness, as a tangent-space normal (Unreal's, green down)
static void TxHeightToNormal(const std::vector<float>& h, int W, int H, float strength, std::vector<float>& out) {
    out.assign((size_t)W * H * 4, 1.f);
    auto at = [&](int x, int y) { x = (x % W + W) % W; y = (y % H + H) % H; const float* p = &h[((size_t)y * W + x) * 4]; return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; };
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float dx = (at(x + 1, y) - at(x - 1, y)) * strength, dy = (at(x, y + 1) - at(x, y - 1)) * strength;
        float nx = -dx, ny = dy, nz = 1, l = sqrtf(nx * nx + ny * ny + nz * nz);
        float* o = &out[((size_t)y * W + x) * 4]; o[0] = nx / l * 0.5f + 0.5f; o[1] = ny / l * 0.5f + 0.5f; o[2] = nz / l * 0.5f + 0.5f; o[3] = 1;
    }
}
static std::mutex g_txMx; static std::atomic<int> g_txBusy{ 0 };
static std::vector<std::function<void()>> g_txDone;
static ULONGLONG g_txDirtyAt = 0; static int g_txDirty = -1;
static void TxDisplayRebuild();
static void TxRunProcess(int i) {
    if (i < 0 || i >= (int)g_tx.size()) return;
    TxAsset a = g_tx[i]; TxSaveMeta(a);
    g_txBusy++;
    std::thread([a]() {
        std::vector<float> px; int W = 0, H = 0; std::string err;
        bool ok = TxProcess(a, px, W, H, err);
        if (ok && a.s.comp == TXC_HDR) {                 // the 8-bit picture graphs use: clamped (and encoded for colour types)
            std::string e2; ImageWrite(a.out.substr(0, a.out.size() - 4) + L".hdr", px, W, H, 3, 4, e2);
            for (size_t k = 0; k < px.size(); k++) if (k % 4 != 3) px[k] = a.s.srgb ? TxToSrgb(px[k]) : std::clamp(px[k], 0.f, 1.f);
        }
        if (ok) ok = ImageWrite(a.out, px, W, H, 0, 4, err);
        std::lock_guard<std::mutex> lk(g_txMx);
        std::wstring meta = a.meta;
        g_txDone.push_back([ok, err, W, H, meta, out = a.out]() {
            for (auto& t : g_tx) if (t.meta == meta) { t.w = W; t.h = H; }
            if (!ok) g_toasts.push_back({ "Texture: " + err, true, GetTickCount64() });
            MsForgetTexture("file:" + W2U(out));
            g_mePrevKey.clear();
            TxDisplayRebuild();
        });
        g_txBusy--; g_framesLeft = 3;
    }).detach();
}
static void TxChanged(int i) { g_txDirty = i; g_txDirtyAt = GetTickCount64(); }
// import a picture into the library (a copy of it; the original stays where it was)
static void TxImport(const std::wstring& file) {
    MakeDirs(TxDir() + L"\\sources");
    std::wstring fn = file.substr(file.find_last_of(L"\\/") + 1);
    size_t dot = fn.find_last_of(L'.');
    std::wstring base = dot == std::wstring::npos ? fn : fn.substr(0, dot), ext = dot == std::wstring::npos ? L"" : fn.substr(dot);
    for (auto& c : base) if (wcschr(L"\\/:*?\"<>|\t", c)) c = L'_';
    std::wstring name = base; for (int k = 2; FileExists(TxDir() + L"\\" + name + L".uetex"); k++) name = base + L" (" + std::to_wstring(k) + L")";
    TxAsset a; a.name = W2U(name); a.meta = TxDir() + L"\\" + name + L".uetex"; a.out = TxDir() + L"\\" + name + L".png";
    a.src = TxDir() + L"\\sources\\" + name + ext;
    if (!CopyFileW(file.c_str(), a.src.c_str(), FALSE)) { g_toasts.push_back({ "Couldn't copy " + W2U(file), true, GetTickCount64() }); return; }
    a.s.type = TxGuessType(a.name); TxDefaults(a.s);
    a.srcFmt = W2U(Lower(ext.size() > 1 ? ext.substr(1) : L"")); a.hdr = a.srcFmt == "hdr";
    if (a.hdr) a.s.comp = a.s.type == TX_BASECOLOR || a.s.type == TX_EMISSIVE ? TXC_HDR : a.s.comp;
    TxSaveMeta(a);
    g_tx.push_back(a); g_txSel = (int)g_tx.size() - 1;
    TxRunProcess(g_txSel);
    TxReload(); for (int i = 0; i < (int)g_tx.size(); i++) if (g_tx[i].meta == a.meta) g_txSel = i;
}
static void TxImportPick() {
    OvBrowse(L"Pictures (*.png, *.jpg, *.tga, *.bmp, *.hdr)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.hdr\0", L"Import a texture", ImpStartDir(), false, L"", nullptr, [](std::wstring f) { if (!f.empty()) TxImport(f); });
}
// export the processed texture as PNG / TGA / JPG / HDR / BMP
static void TxExport(int i) {
    if (i < 0 || i >= (int)g_tx.size()) return;
    TxAsset a = g_tx[i];
    OvBrowse(L"PNG (*.png)\0*.png\0TGA (*.tga)\0*.tga\0JPG (*.jpg)\0*.jpg\0HDR (*.hdr)\0*.hdr\0BMP (*.bmp)\0*.bmp\0", L"Export the texture", L"", true, U2W(a.name), L"png", [a](std::wstring f) {
        if (f.empty()) return;
        std::wstring l = Lower(f); int fmt = 0;
        if (l.size() > 4) { std::wstring e = l.substr(l.size() - 4); fmt = e == L".tga" ? 1 : e == L".jpg" ? 2 : e == L".hdr" ? 3 : e == L".bmp" ? 4 : 0; }
        std::thread([a, f, fmt]() {
            std::vector<float> px; int W = 0, H = 0; std::string err;
            bool ok = TxProcess(a, px, W, H, err);
            if (ok && fmt != 3 && a.s.comp == TXC_HDR) for (size_t k = 0; k < px.size(); k++) if (k % 4 != 3) px[k] = a.s.srgb ? TxToSrgb(px[k]) : std::clamp(px[k], 0.f, 1.f);
            if (ok) ok = ImageWrite(f, px, W, H, fmt, a.s.comp == TXC_GRAY || a.s.comp == TXC_ALPHA ? 1 : 4, err);
            std::lock_guard<std::mutex> lk(g_txMx);
            g_txDone.push_back([ok, err, f]() { g_toasts.push_back({ ok ? "Exported " + W2U(f) : "Export failed: " + err, !ok, GetTickCount64() }); });
            g_framesLeft = 3;
        }).detach();
    });
}
// a new library texture: a normal map made from this one (as a height map)
static void TxMakeNormalFrom(int i, float strength) {
    if (i < 0 || i >= (int)g_tx.size()) return;
    TxAsset a = g_tx[i];
    std::thread([a, strength]() {
        std::vector<float> px, nm; int W = 0, H = 0; std::string err;
        TxAsset hs = a; hs.s.comp = TXC_MASKS; hs.s.srgb = false;
        bool ok = TxProcess(hs, px, W, H, err);
        std::wstring tmp;
        if (ok) { TxHeightToNormal(px, W, H, strength, nm); tmp = TxDir() + L"\\sources\\" + U2W(a.name) + L"_N_from_height.png"; ok = ImageWrite(tmp, nm, W, H, 0, 4, err); }
        std::lock_guard<std::mutex> lk(g_txMx);
        g_txDone.push_back([ok, err, tmp]() {
            if (!ok) { g_toasts.push_back({ "Couldn't make the normal map: " + err, true, GetTickCount64() }); return; }
            TxImport(tmp);
            if (g_txSel >= 0) { g_tx[g_txSel].s.type = TX_NORMAL; TxDefaults(g_tx[g_txSel].s); TxSaveMeta(g_tx[g_txSel]); TxRunProcess(g_txSel); }
            DeleteFileW(tmp.c_str());
            g_toasts.push_back({ "Made a normal map from the height map", false, GetTickCount64() });
        });
        g_framesLeft = 3;
    }).detach();
}

// ---- what the Texture Editor shows: the processed picture, tiled / turned / filtered as set, with channels picked
static struct TxView { ID3D11ShaderResourceView* srv = nullptr; int forSel = -1; std::wstring forFile; ULONGLONG stamp = 0; bool ch[4] = { true, true, true, false }; bool tiled = true; float zoom = 1, panX = 0, panY = 0; std::string key; } g_txv;
static void TxDisplayRebuild() { g_txv.key.clear(); }
static void TxDisplayUpdate() {
    if (g_txSel < 0 || g_txSel >= (int)g_tx.size()) return;
    const TxAsset& a = g_tx[g_txSel];
    WIN32_FILE_ATTRIBUTE_DATA fa{}; GetFileAttributesExW(a.out.c_str(), GetFileExInfoStandard, &fa);
    char k[400]; snprintf(k, sizeof k, "%s|%llu|%d%d%d%d|%d|%s", W2U(a.out).c_str(), ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime,
                          g_txv.ch[0], g_txv.ch[1], g_txv.ch[2], g_txv.ch[3], g_txv.tiled ? 1 : 0, TxSetLine(a.s).c_str());
    if (g_txv.key == k) return;
    g_txv.key = k;
    MgImg img; std::string err;
    img.ok = PictureRGBA(L"", "file:" + W2U(a.out), 2048, img.px, img.w, img.h, err) && img.w > 0;
    if (!img.ok) { MsRel(g_txv.srv); return; }
    if (g_tx[g_txSel].w == 0 || g_tx[g_txSel].s.maxSize == 0) { g_tx[g_txSel].w = img.w; g_tx[g_txSel].h = img.h; }
    const int S = 512;
    std::vector<unsigned char> o((size_t)S * S * 4);
    MgN xf; xf.tile[0] = g_txv.tiled ? a.s.tile[0] : 1; xf.tile[1] = g_txv.tiled ? a.s.tile[1] : 1; xf.off[0] = g_txv.tiled ? a.s.off[0] : 0; xf.off[1] = g_txv.tiled ? a.s.off[1] : 0; xf.rot = g_txv.tiled ? a.s.rot : 0;
    if (g_mgSrgbLut[255] == 0) for (int i = 0; i < 256; i++) g_mgSrgbLut[i] = (float)(i / 255.0 <= 0.04045 ? i / 255.0 / 12.92 : pow((i / 255.0 + 0.055) / 1.055, 2.4));
    int onlyOne = -1, count = 0; for (int c = 0; c < 4; c++) if (g_txv.ch[c]) { count++; onlyOne = c; }
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        float u = (x + 0.5f) / S, v = (y + 0.5f) / S; MgUvXf(xf, u, v);
        float c[4]; MgSample(img, u, v, 2, c, a.s.filter);            // (as stored: no decoding for the view)
        unsigned char* p = &o[((size_t)y * S + x) * 4];
        if (count == 1) { unsigned char g = (unsigned char)lroundf(std::clamp(c[onlyOne], 0.f, 1.f) * 255); p[0] = p[1] = p[2] = g; p[3] = 255; continue; }
        for (int k2 = 0; k2 < 3; k2++) p[k2] = g_txv.ch[k2] ? (unsigned char)lroundf(std::clamp(c[k2], 0.f, 1.f) * 255) : 0;
        if (g_txv.ch[3]) { float al = std::clamp(c[3], 0.f, 1.f); bool chk = ((x / 16) + (y / 16)) & 1; float bg = chk ? 0.35f : 0.22f; for (int k2 = 0; k2 < 3; k2++) p[k2] = (unsigned char)lroundf((p[k2] / 255.f * al + bg * (1 - al)) * 255); }
        p[3] = 255;
    }
    MsRel(g_txv.srv);
    D3D11_TEXTURE2D_DESC d{}; d.Width = d.Height = S; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ o.data(), S * 4, 0 }; ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, &sd, &t))) { g_dev->CreateShaderResourceView(t, nullptr, &g_txv.srv); t->Release(); }
}
static void TxPump() {
    if (!g_txLoaded) TxReload();
    std::vector<std::function<void()>> q; { std::lock_guard<std::mutex> lk(g_txMx); q.swap(g_txDone); }
    for (auto& f : q) f();
    if (g_txDirty >= 0 && GetTickCount64() - g_txDirtyAt > 300 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { TxRunProcess(g_txDirty); g_txDirty = -1; }
    if (g_txDirty >= 0 || g_txBusy > 0) g_framesLeft = std::max(g_framesLeft, 2);
}

// ---- the Texture Editor tab
static void TxEditor() {
    static ULONGLONG lastShown = 0;                      // (coming back to the tab: read the folder again, files may have changed)
    if (GetTickCount64() - lastShown > 1000) TxReload();
    lastShown = GetTickCount64();
    TxPump();
    float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    float listW = std::clamp(W * 0.2f, Px(200), Px(280)), propW = std::clamp(W * 0.26f, Px(280), Px(380));
    // library list
    ImGui::BeginChild("##txlist", ImVec2(listW, H), ImGuiChildFlags_Borders);
    if (AccentButton(ICON_FILE_IMPORT "  Import", ImVec2(-Px(36), 0))) TxImportPick();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("PNG, JPG, TGA, BMP or HDR");
    ImGui::SameLine(0, Px(4));
    if (ImGui::Button(ICON_FOLDER_OPEN "##txdir", ImVec2(-1, 0))) { MakeDirs(TxDir()); OpenFolder(TxDir()); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open the library folder");
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##txf", ICON_SEARCH "  Search", g_txFilter, sizeof g_txFilter);
    std::string f = W2U(Lower(U2W(g_txFilter)));
    if (g_tx.empty()) { ImGui::PushTextWrapPos(0); EdLabel("No textures yet. Import pictures here, then drag them into a material graph."); ImGui::PopTextWrapPos(); }
    for (int i = 0; i < (int)g_tx.size(); i++) {
        if (!f.empty() && W2U(Lower(U2W(g_tx[i].name))).find(f) == std::string::npos) continue;
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos(); float th = Px(40);
        if (ImGui::Selectable("##tx", g_txSel == i, 0, ImVec2(0, th))) { g_txSel = i; TxDisplayRebuild(); }
        if (ImGui::BeginDragDropSource()) { std::string v = "file:" + W2U(g_tx[i].out); ImGui::SetDragDropPayload("METEX", v.data(), v.size()); ImGui::TextUnformatted(g_tx[i].name.c_str()); ImGui::EndDragDropSource(); }
        if (ImGui::BeginPopupContextItem("##txm")) {
            if (ImGui::MenuItem("Show in folder")) OpenFolder(TxDir());
            if (ImGui::MenuItem(ICON_TRASH "  Remove from the library")) {
                MakeDirs(TxDir() + L"\\_removed");
                std::wstring b = TxDir() + L"\\_removed\\" + g_tx[i].meta.substr(g_tx[i].meta.find_last_of(L"\\/") + 1);
                MoveFileExW(g_tx[i].meta.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING);
                TxReload(); ImGui::EndPopup(); ImGui::PopID(); break;
            }
            ImGui::EndPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (ID3D11ShaderResourceView* s = MatThumbFor(g_tx[i].out)) dl->AddImageRounded((ImTextureID)s, ImVec2(p.x + Px(2), p.y + Px(2)), ImVec2(p.x + th - Px(2), p.y + th - Px(2)), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, Px(3));
        else dl->AddRectFilled(ImVec2(p.x + Px(2), p.y + Px(2)), ImVec2(p.x + th - Px(2), p.y + th - Px(2)), IM_COL32(40, 40, 46, 255), Px(3));
        dl->AddText(ImVec2(p.x + th + Px(6), p.y + Px(3)), ImGui::GetColorU32(ImGuiCol_Text), g_tx[i].name.c_str());
        ImGui::PushFont(F_small);
        dl->AddText(ImVec2(p.x + th + Px(6), p.y + Px(21)), ImGui::GetColorU32(P.dim), (std::string(g_txTypeName[g_tx[i].s.type]) + (g_tx[i].hdr ? "  \xC2\xB7  HDR" : "")).c_str());
        ImGui::PopFont();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, Px(4));
    // the picture
    ImGui::BeginChild("##txview", ImVec2(W - listW - propW - Px(8), H), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (g_txSel < 0 || g_txSel >= (int)g_tx.size()) { EdLabel("Pick a texture on the left, or import one."); ImGui::EndChild(); ImGui::SameLine(0, Px(4)); ImGui::BeginChild("##txprops", ImVec2(propW, H), ImGuiChildFlags_Borders); ImGui::EndChild(); return; }
    TxAsset& a = g_tx[g_txSel];
    TxDisplayUpdate();
    {
        const char* cn[4] = { "R", "G", "B", "A" }; ImU32 cc[4] = { IM_COL32(220, 70, 60, 255), IM_COL32(80, 200, 90, 255), IM_COL32(80, 120, 240, 255), IM_COL32(170, 170, 176, 255) };
        for (int c = 0; c < 4; c++) {
            if (c) ImGui::SameLine(0, Px(2));
            ImGui::PushStyleColor(ImGuiCol_Button, g_txv.ch[c] ? ImGui::ColorConvertU32ToFloat4(cc[c]) : ImVec4(0.18f, 0.18f, 0.2f, 1));
            if (ImGui::Button(cn[c], ImVec2(Px(28), 0))) { g_txv.ch[c] = !g_txv.ch[c]; bool any = false; for (bool b : g_txv.ch) any |= b; if (!any) g_txv.ch[c] = true; }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the %s channel (on its own: as grey)", cn[c]);
        }
        ImGui::SameLine(0, Px(12)); ImGui::Checkbox("Tiling / offset / rotation", &g_txv.tiled);
        ImGui::SameLine(); if (ImGui::Button(ICON_EXPAND "##txfit")) { g_txv.zoom = 1; g_txv.panX = g_txv.panY = 0; }
        if (g_txBusy > 0 || g_txDirty >= 0) { ImGui::SameLine(); EdLabel(ICON_HOURGLASS " Processing..."); }
    }
    ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##txpic", av, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) g_txv.zoom = std::clamp(g_txv.zoom * (io.MouseWheel > 0 ? 1.15f : 1 / 1.15f), 0.1f, 16.f);
    if (ImGui::IsItemActive()) { g_txv.panX += io.MouseDelta.x; g_txv.panY += io.MouseDelta.y; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(o, ImVec2(o.x + av.x, o.y + av.y), true);
    float side = std::min(av.x, av.y) * 0.95f * g_txv.zoom;
    float aspect = a.w > 0 && a.h > 0 ? (float)a.w / a.h : 1;
    float pw = aspect >= 1 ? side : side * aspect, ph = aspect >= 1 ? side / aspect : side;
    ImVec2 c(o.x + av.x / 2 + g_txv.panX, o.y + av.y / 2 + g_txv.panY);
    // checker behind (alpha)
    dl->AddRectFilled(ImVec2(c.x - pw / 2, c.y - ph / 2), ImVec2(c.x + pw / 2, c.y + ph / 2), IM_COL32(50, 50, 56, 255));
    if (g_txv.srv) dl->AddImage((ImTextureID)g_txv.srv, ImVec2(c.x - pw / 2, c.y - ph / 2), ImVec2(c.x + pw / 2, c.y + ph / 2));
    dl->AddRect(ImVec2(c.x - pw / 2, c.y - ph / 2), ImVec2(c.x + pw / 2, c.y + ph / 2), IM_COL32(80, 80, 88, 255));
    dl->PopClipRect();
    ImGui::PushFont(F_small);
    char info[160]; snprintf(info, sizeof info, "%d x %d  \xC2\xB7  %s source%s  \xC2\xB7  wheel: zoom, drag: move", a.w, a.h, a.srcFmt.c_str(), a.hdr ? " (high dynamic range)" : "");
    dl->AddText(ImVec2(o.x + Px(8), o.y + av.y - ImGui::GetTextLineHeight() - Px(4)), IM_COL32(150, 150, 158, 255), info);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::SameLine(0, Px(4));
    // settings
    ImGui::BeginChild("##txprops", ImVec2(propW, H), ImGuiChildFlags_Borders);
    g_meLabelW = std::min(Px(130), propW * 0.42f);
    TxSet& s = a.s; bool ch = false;
    ImGui::PushFont(F_h2); ImGui::TextWrapped("%s", a.name.c_str()); ImGui::PopFont();
    if (AccentButton(ICON_PLUS "  Use in the material graph", ImVec2(-1, 0)) && M.open) { M.dragNew = "tex:file:" + W2U(a.out); M.tab = 0; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adds a Texture Sample with this texture (and its settings) to the graph. You can also drag it from the list.");
    SectionLabel("Texture");
    MeProp("Type", "What the picture is for: sets sRGB, compression and how graphs sample it");
    if (ImGui::Combo("##ttype", &s.type, g_txTypeName, TX_TYPES)) { TxDefaults(s); ch = true; }
    MeProp("sRGB", "On for colour pictures; off for data (normals, roughness, masks)"); ch |= ImGui::Checkbox("##tsrgb", &s.srgb);
    MeProp("Compression", "How the result is stored (like Unreal's Compression Settings)"); ch |= ImGui::Combo("##tcomp", &s.comp, g_txCompName, TXC_COUNT);
    const char* sizes[] = { "Original", "4096", "2048", "1024", "512", "256" }; const int sv[] = { 0, 4096, 2048, 1024, 512, 256 };
    int si = 0; for (int k = 0; k < 6; k++) if (sv[k] == s.maxSize) si = k;
    MeProp("Maximum Size"); if (ImGui::Combo("##tmax", &si, sizes, 6)) { s.maxSize = sv[si]; ch = true; }
    SectionLabel("Sampling (in material graphs)");
    const char* fl2[] = { "Bilinear", "Nearest (pixelated)", "Anisotropic" };
    bool samp = false;
    MeProp("Filter"); samp |= ImGui::Combo("##tfil", &s.filter, fl2, 3);
    MeProp("Tiling"); samp |= ImGui::DragFloat2("##ttile", s.tile, 0.02f, -256, 256, "%.2f");
    MeProp("Offset"); samp |= ImGui::DragFloat2("##toff", s.off, 0.005f, -16, 16, "%.3f");
    MeProp("Rotation"); samp |= ImGui::DragFloat("##trot", &s.rot, 0.5f, -360, 360, "%.1f\xC2\xB0");
    if (samp) { TxSaveMeta(a); TxDisplayRebuild(); }
    SectionLabel("Adjustments");
    if (a.hdr) { MeProp("Exposure", "HDR: stops brighter or darker"); ch |= ImGui::DragFloat("##texp", &s.exposure, 0.02f, -10, 10, "%.2f"); }
    ImGui::BeginDisabled(s.comp == TXC_NORMAL);
    MeProp("Brightness"); ch |= ImGui::SliderFloat("##tbr", &s.bright, -1, 1, "%.2f");
    MeProp("Contrast"); ch |= ImGui::SliderFloat("##tco", &s.contrast, 0, 3, "%.2f");
    MeProp("Saturation"); ch |= ImGui::SliderFloat("##tsa", &s.sat, 0, 2, "%.2f");
    MeProp("Hue"); ch |= ImGui::SliderFloat("##thu", &s.hue, -180, 180, "%.0f\xC2\xB0");
    MeProp("Gamma"); ch |= ImGui::SliderFloat("##tga", &s.gamma, 0.2f, 5, "%.2f");
    MeProp("Invert"); ch |= ImGui::Checkbox("##tinv", &s.invert);
    ImGui::EndDisabled();
    MeProp("Flip Green", "Normal maps: OpenGL-style (green up) to Unreal's DirectX-style, or back"); ch |= ImGui::Checkbox("##tfg", &s.flipG);
    MeProp("Flip X"); ch |= ImGui::Checkbox("##tfx", &s.flipX);
    MeProp("Flip Y"); ch |= ImGui::Checkbox("##tfy", &s.flipY);
    if (ImGui::Button(ICON_ERASER "  Reset adjustments", ImVec2(-1, 0))) { TxSet d; d.type = s.type; TxDefaults(d); d.filter = s.filter; memcpy(d.tile, s.tile, sizeof d.tile); memcpy(d.off, s.off, sizeof d.off); d.rot = s.rot; d.maxSize = s.maxSize; s = d; ch = true; }
    SectionLabel("Make");
    static float nStrength = 4.f;
    ImGui::SetNextItemWidth(Px(80)); ImGui::DragFloat("##nstr", &nStrength, 0.1f, 0.1f, 64, "%.1f"); ImGui::SameLine();
    if (ImGui::Button("Normal map from this (height)", ImVec2(-1, 0))) TxMakeNormalFrom(g_txSel, nStrength);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Treats this picture's brightness as height and makes a new normal map texture (strength on the left)");
    if (ImGui::Button(ICON_FILE_EXPORT "  Export...", ImVec2(-1, 0))) TxExport(g_txSel);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the processed texture as PNG, TGA, JPG, HDR or BMP");
    if (ch) { TxChanged(g_txSel); TxDisplayRebuild(); }
    ImGui::EndChild();
}

// ---- the texture library in the material editor's palette, and the picker for a Texture Sample node
static void MeTextureLibraryItems() {
    if (!g_txLoaded) TxReload();
    if (g_tx.empty()) return;
    std::string f = W2U(Lower(U2W(M.palFilter)));
    bool any = false; for (auto& t : g_tx) if (f.empty() || W2U(Lower(U2W(t.name))).find(f) != std::string::npos) any = true;
    if (!any) return;
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (!ImGui::CollapsingHeader("Texture Library")) return;
    for (int i = 0; i < (int)g_tx.size(); i++) {
        if (!f.empty() && W2U(Lower(U2W(g_tx[i].name))).find(f) == std::string::npos) continue;
        ImGui::PushID(1000 + i);
        ImVec2 p = ImGui::GetCursorScreenPos(); float th = Px(26);
        ImGui::Selectable("##lt", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, th));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) M.dragNew = "tex:file:" + W2U(g_tx[i].out);
        if (ImGui::BeginDragDropSource()) { std::string v = "file:" + W2U(g_tx[i].out); ImGui::SetDragDropPayload("METEX", v.data(), v.size()); ImGui::TextUnformatted(g_tx[i].name.c_str()); ImGui::EndDragDropSource(); }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (ID3D11ShaderResourceView* s = MatThumbFor(g_tx[i].out)) dl->AddImage((ImTextureID)s, ImVec2(p.x + Px(2), p.y + Px(1)), ImVec2(p.x + th - Px(1), p.y + th - Px(1)));
        dl->AddText(ImVec2(p.x + th + Px(6), p.y + (th - ImGui::GetTextLineHeight()) / 2), ImGui::GetColorU32(ImGuiCol_Text), g_tx[i].name.c_str());
        ImGui::PopID();
    }
}
static void MeApplyTexSettings(int nodeId) {
    MeNode* n = MeFind(nodeId); if (!n) return;
    if (!g_txLoaded) TxReload();
    int i = TxFind(n->value); if (i < 0) return;
    const TxSet& s = g_tx[i].s;
    n->sampler = TxSampler(s.type); n->filter = s.filter;
    memcpy(n->tile, s.tile, sizeof n->tile); memcpy(n->off, s.off, sizeof n->off); n->rot = s.rot;
}
static int g_txPickNode = -1; static bool g_txPickOpen = false; static std::string g_meInstTexParam;   // (-2: a material instance's texture parameter)
static void MeOpenTexLibraryPicker(int node) { g_txPickNode = node; g_txPickOpen = true; if (!g_txLoaded) TxReload(); }
static void MeTexLibraryPopup() {
    if (g_txPickOpen) { ImGui::OpenPopup("##txpick"); g_txPickOpen = false; }
    if (!ImGui::BeginPopup("##txpick")) return;
    EdLabel("Texture library");
    if (g_tx.empty()) EdLabel("(empty: import textures on the Textures tab)");
    for (int i = 0; i < (int)g_tx.size(); i++) {
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos(); float th = Px(32);
        if (ImGui::Selectable("##pk", false, 0, ImVec2(Px(320), th))) {
            if (g_txPickNode == -2) { g_meOverride[g_meInstTexParam] = "file:" + W2U(g_tx[i].out); MeStdChanged(); }
            else if (MeNode* n = MeFind(g_txPickNode)) { n->value = "file:" + W2U(g_tx[i].out); MeApplyTexSettings(n->id); MeNodeChanged(*n, true); MeStdChanged(); }
            ImGui::CloseCurrentPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (ID3D11ShaderResourceView* s = MatThumbFor(g_tx[i].out)) dl->AddImage((ImTextureID)s, ImVec2(p.x, p.y), ImVec2(p.x + th, p.y + th));
        dl->AddText(ImVec2(p.x + th + Px(6), p.y + Px(2)), ImGui::GetColorU32(ImGuiCol_Text), g_tx[i].name.c_str());
        ImGui::PushFont(F_small); dl->AddText(ImVec2(p.x + th + Px(6), p.y + Px(17)), ImGui::GetColorU32(P.dim), g_txTypeName[g_tx[i].s.type]); ImGui::PopFont();
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::Selectable(ICON_FILE_IMPORT "  Import a picture into the library...")) TxImportPick();
    ImGui::EndPopup();
}

// an equirectangular HDRI for the preview's sky
static bool MsLoadHdr(const std::wstring& f, std::vector<float>& px, int& W, int& H, std::string& err) {
    bool hdr = false;
    if (!ImageLoadAny(f, px, W, H, hdr, err)) return false;
    if (!hdr) for (size_t i = 0; i < px.size(); i++) if (i % 4 != 3) px[i] = TxToLin(px[i]);      // an ordinary picture as a sky: decode it
    // big panoramas are made smaller (the preview blurs it anyway)
    while (W > 2048) {
        int nw = W / 2, nh = std::max(1, H / 2); std::vector<float> o((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int k = 0; k < 4; k++) {
            float s = 0; for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) s += px[((size_t)std::min(H - 1, y * 2 + dy) * W + (x * 2 + dx)) * 4 + k];
            o[((size_t)y * nw + x) * 4 + k] = s / 4;
        }
        px.swap(o); W = nw; H = nh;
    }
    return true;
}
