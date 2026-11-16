// ---- Material editor (in the in-game editor window)
// A node graph like Unreal's material editor, for the materials of the selected object. The output node is the
// material: its inputs are the settings the material has (pictures, colours, numbers) plus the material itself.
// Nodes feed them: a Texture Sample (any texture the game has loaded, or a picture file), a Colour, a Number, or
// a Material (swap the whole material). Re-wiring or changing a node changes the object in the game straight away.
// A shipped game can't compile new materials, so only what the material exposes can be changed: there are no
// maths nodes (they would need Unreal's shader compiler). This is said in the editor itself.

#include <sstream>
enum { MEK_NONE = 0, MEK_TEX = 1, MEK_VEC = 2, MEK_SCA = 3, MEK_MAT = 4, MEK_OP = 5, MEK_STD = 6, MEK_COMMENT = 7 };
// Unreal's own material inputs on the output node: what's wired into them is worked out by the launcher (the game
// can't compile a material) and baked into textures, which go into the material's texture slots
enum { STD_BASE, STD_METAL, STD_SPEC, STD_ROUGH, STD_EMIS, STD_OPAC, STD_NORMAL, STD_AO, STD_OPMASK, STD_WPO, STD_DISP, STD_N };
static const char* g_meStd[STD_N] = { "Base Color", "Metallic", "Specular", "Roughness", "Emissive Color", "Opacity", "Normal", "Ambient Occlusion",
                                      "Opacity Mask", "World Position Offset", "Displacement" };

// maths nodes (MEK_OP)
// (new ones go on the end: saved graphs keep the numbers)
enum { OP_TEXCOORD, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_LERP, OP_ONEMINUS, OP_POW, OP_CLAMP, OP_DESAT, OP_MASK, OP_APPEND, OP_LAYERBLEND,
       OP_SATURATE, OP_FRESNEL, OP_NORMALSTR, OP_PANNER, OP_ROTATOR, OP_TIME, OP_SIN, OP_COS, OP_ABS, OP_FRAC, OP_FLOOR, OP_MIN, OP_MAX,
       OP_DOT, OP_NORMALIZE, OP_BUMPOFFSET, OP_VNORMAL, OP_CAMVEC, OP_WORLDPOS, OP_SQRT, OP_IF, OP_COUNT };
static const char* g_meOpName[OP_COUNT] = { "Texture Coordinate", "Add", "Subtract", "Multiply", "Divide", "Lerp", "One Minus", "Power", "Clamp", "Desaturation",
    "Component Mask", "Append", "Layer Blend", "Saturate", "Fresnel", "Normal Strength", "Panner", "Rotator", "Time", "Sine", "Cosine", "Abs", "Frac", "Floor",
    "Min", "Max", "Dot", "Normalize", "Bump Offset", "Vertex Normal WS", "Camera Vector WS", "Absolute World Position", "Square Root", "If" };
static const char* g_meOpHelp[OP_COUNT] = {
    "UV coordinates, times the tiling", "A + B", "A - B", "A x B", "A / B", "Blends A to B by Alpha (0 = A, 1 = B)", "1 - X", "Base to the power of Exp",
    "Keeps the input between Min and Max", "Takes the colour out (Fraction 1 = grey)", "Keeps some channels (R, G, B, A)", "Joins A and B into one vector",
    "Blends material layers by weight", "Clamps to 0..1", "Brighter where the surface turns away from the camera (rim light)",
    "Makes a tangent-space normal map stronger or flatter", "Moves UVs over time (scrolling textures)", "Turns UVs around a centre over time",
    "Seconds since the preview started", "Sine (one wave per Period)", "Cosine (one wave per Period)", "Absolute value", "Fractional part", "Rounds down",
    "The smaller of A and B", "The bigger of A and B", "Dot product of A and B", "Makes a vector length 1", "Parallax: shifts UVs by a height map so it looks deep",
    "The surface's normal in world space", "Direction from the surface to the camera", "The pixel's position in the world",
    "Square root", "A > B ? A>B : (A == B ? A==B : A<B)" };
struct MeParam { int kind = 0; std::string name, value, orig; };
struct MeSlot { int idx = 0; std::string comp; int slot = 0; std::string mat, orig, slotName, base; std::vector<MeParam> params; };
struct MeNode {
    int id = 0; int kind = 0; float x = 0, y = 0; std::string value; bool output = false;
    int op = 0; float c[4] = { 0, 0, 0, 0 };                  // maths: which, and the values of inputs with nothing plugged in
    int sampler = 0;                                          // texture: 0 colour, 1 normal map, 2 plain numbers (masks)
    std::vector<std::pair<std::string, float>> layers;        // layer blend: name, preview weight
    std::string name;                                         // a parameter's name (Scalar / Vector / Texture Parameter); a comment's text
    float tile[2] = { 1, 1 }, off[2] = { 0, 0 }, rot = 0;     // texture: UV tiling, offset, rotation (degrees)
    int filter = 0;                                           // texture: 0 bilinear, 1 nearest, 2 anisotropic
    float w = 0, h = 0; unsigned col = 0;                     // comment box: size and colour
};
static bool MeIsParam(const MeNode& n) { return !n.name.empty() && (n.kind == MEK_SCA || n.kind == MEK_VEC || n.kind == MEK_TEX); }
template <class T> static void MsRel(T*& p) { if (p) { p->Release(); p = nullptr; } }
// material instance parameter overrides: a parameter node's value, or what the instance sets it to
static std::map<std::string, std::string> g_meOverride;
static std::string MeParamValue(const MeNode& n) {
    if (MeIsParam(n)) { auto it = g_meOverride.find(n.name); if (it != g_meOverride.end()) return it->second; }
    return n.value;
}
// the material's own properties (on the output node's Details, like Unreal's)
enum { BLEND_OPAQUE, BLEND_MASKED, BLEND_TRANSLUCENT, BLEND_ADDITIVE };
struct MeProps { int blend = BLEND_OPAQUE; bool twoSided = false; bool unlit = false; float clip = 0.333f; float dispScale = 0.05f; float wpoScale = 1.f; };
struct MeCustom {
    MeProps props; bool hasProps = false;
    std::vector<MeNode> nodes; std::map<long long, std::pair<int, int>> nin; std::map<int, std::pair<int, int>> std;
    // wires from the material's own settings (rebuilt every time, so kept by the setting's name)
    std::map<long long, std::pair<std::string, int>> ninp; std::map<int, std::pair<std::string, int>> stdp;
    bool empty() const { return nodes.empty() && stdp.empty(); }
};
struct MeDrag { int node = -1; float offx = 0, offy = 0; };

static struct MatEd {
    bool open = false;
    std::string actor, name, err, model;
    std::vector<MeSlot> slots; int cur = 0; bool loaded = false; ULONGLONG askedAt = 0;
    // graph of the current slot
    std::vector<MeNode> nodes; std::map<int, int> links;      // output pin -> node id
    std::vector<MeParam> pins;                                // the output node's inputs (pin 0 = the material itself)
    int nextId = 1, sel = -1, builtFor = -1;
    float panX = 0, panY = 0, zoom = 1; bool fitted = false;
    MeDrag drag; int linkFrom = -1; bool panning = false; ImVec2 rclickAt; bool rmoved = false;
    // sending
    std::map<std::string, std::pair<std::string, ULONGLONG>> pend;   // pin -> value waiting (drags are sent ~20 times a second)
    ULONGLONG lastChange = 0; bool prevWanted = false; int prevSeq = 0; std::string prevPng; ULONGLONG prevAsked = 0;
    // window size before the editor opened
    RECT oldRect{}; bool resized = false;
    // pickers
    int picker = 0; int pickNode = -1; char filter[128] = ""; std::vector<std::string> list; ULONGLONG listStamp = 0; bool listAsked = false;
    // Unreal inputs: node inputs, which output of a node feeds a pin, baking
    std::map<long long, std::pair<int, int>> nin;              // node id * 64 + input -> (source node id, its output)
    std::map<int, int> linkOut;                                // output-node pin -> which output of its node
    int stdStart = 0;                                          // the first Unreal input among the pins
    std::string builtKey; std::map<std::string, MeCustom> saved;
    bool live = true; int bakeRes = 1024; bool stdDirty = false; ULONGLONG stdChangedAt = 0; std::atomic<bool> baking{ false };
    std::string bakeNote;
    // editing
    std::set<int> selSet;                                      // every selected node (sel is the one the Details show)
    bool boxing = false; ImVec2 boxFrom; std::vector<std::pair<int, ImVec2>> dragGroup;
    int resizing = -1; bool dragMoved = false;
    MeProps props;
    // a material asset or instance opened on its own (not one of an object's materials)
    std::wstring asset;                                        // .uemat file being edited (empty = an object's material)
    bool dirty = false;                                        // unsaved changes to the asset
    int tab = 0;                                               // 0 graph, 1 textures, 2 instance
    char palFilter[96] = ""; std::string dragNew;              // palette search, and a node being dragged in from it
    int errNode = -1;                                          // node the compiler complains about
} M;
static long long MeInKey(int node, int in) { return (long long)node * 64 + in; }
static int g_meLinkOut = 0;          // which output of M.linkFrom is being dragged

// ---- files from the game
static bool MeFileStamp(const std::wstring& p, ULONGLONG& stamp) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return false;
    ULONGLONG s = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime) ^ fa.nFileSizeLow;
    if (s == stamp) return false;
    stamp = s; return true;
}
static bool MeReadAll(const std::wstring& p, std::string& out) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0; bool ok = sz.QuadPart == 0 || ReadFile(f, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(f); out.resize(rd);
    return ok;
}
static std::vector<std::string> MeTabs(const std::string& s) {
    std::vector<std::string> t; size_t a = 0;
    for (;;) { size_t b = s.find('\t', a); t.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
    return t;
}
static std::string MeShort(const std::string& path) {
    std::string s = path; size_t sl = s.find_last_of("/\\:"); if (sl != std::string::npos) s = s.substr(sl + 1);
    size_t d = s.find('.'); if (d != std::string::npos && d + 1 < s.size()) s = s.substr(d + 1);
    return s.empty() ? path : s;
}
static std::string MeFileName(const std::string& v) { std::string s = v.substr(5); size_t sl = s.find_last_of("/\\"); return sl == std::string::npos ? s : s.substr(sl + 1); }
static const char* MeKindName(int k) { return k == MEK_TEX ? "Texture" : k == MEK_VEC ? "Colour" : k == MEK_SCA ? "Number" : k == MEK_MAT ? "Material" : "?"; }
static const char* MeKindCode(int k) { return k == MEK_TEX ? "tex" : k == MEK_VEC ? "vec" : k == MEK_SCA ? "sca" : "mat"; }
static ImU32 MeKindCol(int k, float a = 1) {
    ImVec4 c = k == MEK_TEX ? Hex(0x7FB2E5) : k == MEK_VEC ? Hex(0xE5C35A) : k == MEK_SCA ? Hex(0x8FD18A) : Hex(0xE8E8E8);
    c.w = a; return ImGui::GetColorU32(c);
}
static bool MeColour(const std::string& v, float c[4]) {
    c[3] = 1; return sscanf(v.c_str(), "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) >= 3;
}

static void MeAskGraph() {
    DeleteFileW((BridgeDir() + L"\\matgraph.txt").c_str());
    M.loaded = false; M.askedAt = GetTickCount64();
    Ed("ed_matread", M.actor);
}
static void MePollGraph() {
    if (!M.asset.empty()) return;            // (a material asset isn't read from the game)
    static ULONGLONG stamp = 0;
    std::wstring p = BridgeDir() + L"\\matgraph.txt";
    if (!MeFileStamp(p, stamp)) return;
    std::string d; if (!MeReadAll(p, d) || d.find("end=1") == std::string::npos) { stamp = 0; return; }
    std::vector<MeSlot> slots; std::string forPath, name, err, model;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('='); if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "for") forPath = v;
        else if (k == "name") name = v;
        else if (k == "model") model = v;
        else if (k == "err") err = v;
        else if (k == "slot") {
            auto t = MeTabs(v); if (t.size() < 7) continue;
            MeSlot s; s.idx = atoi(t[0].c_str()); s.comp = t[1]; s.slot = atoi(t[2].c_str()); s.mat = t[3]; s.orig = t[4]; s.slotName = t[5]; s.base = t[6];
            slots.push_back(s);
        } else if (k == "tex" || k == "vec" || k == "sca") {
            auto t = MeTabs(v); if (t.size() < 4 || slots.empty()) continue;
            MeParam pm; pm.kind = k == "tex" ? MEK_TEX : k == "vec" ? MEK_VEC : MEK_SCA; pm.name = t[1]; pm.value = t[2]; pm.orig = t[3];
            for (auto& s : slots) if (s.idx == atoi(t[0].c_str())) s.params.push_back(pm);
        }
    }
    if (forPath != M.actor) return;
    M.slots = slots; M.name = name; M.err = err; M.model = model; M.loaded = true;
    if (M.cur >= (int)M.slots.size()) M.cur = 0;
    M.builtFor = -1;                     // rebuild the graph from what the game says now
    g_framesLeft = 3;
}

static void MeArrange();
// ---- the graph of one slot: the output node, and one node feeding each of its settings
static MeNode* MeFind(int id) { for (auto& n : M.nodes) if (n.id == id) return &n; return nullptr; }
static MeNode* MeOutput() { for (auto& n : M.nodes) if (n.output) return &n; return nullptr; }
// ---- pins of a node (inputs left, outputs right)
static std::vector<std::string> MeNodeIns(const MeNode& n) {
    if (n.kind == MEK_TEX) return { "UVs" };
    if (n.kind != MEK_OP) return {};
    switch (n.op) {
    case OP_TEXCOORD: case OP_TIME: case OP_VNORMAL: case OP_CAMVEC: case OP_WORLDPOS: return {};
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_APPEND: case OP_MIN: case OP_MAX: case OP_DOT: return { "A", "B" };
    case OP_LERP: return { "A", "B", "Alpha" };
    case OP_ONEMINUS: case OP_MASK: case OP_SATURATE: case OP_ABS: case OP_FRAC: case OP_FLOOR: case OP_NORMALIZE: case OP_SQRT: return { "Input" };
    case OP_SIN: case OP_COS: return { "Input" };
    case OP_POW: return { "Base", "Exp" };
    case OP_CLAMP: return { "Input", "Min", "Max" };
    case OP_DESAT: return { "Input", "Fraction" };
    case OP_FRESNEL: return { "Exponent", "Base Reflect Fraction" };
    case OP_NORMALSTR: return { "Normal", "Strength" };
    case OP_PANNER: return { "Coordinate", "Time" };
    case OP_ROTATOR: return { "Coordinate", "Time" };
    case OP_BUMPOFFSET: return { "Coordinate", "Height", "Height Ratio" };
    case OP_IF: return { "A", "B", "A > B", "A == B", "A < B" };
    case OP_LAYERBLEND: { std::vector<std::string> v; for (auto& l : n.layers) { v.push_back("Layer " + l.first); v.push_back("Weight " + l.first); } return v; }
    }
    return {};
}
static std::vector<std::string> MeNodeOuts(const MeNode& n) {
    if (n.output) return {};
    if (n.kind == MEK_TEX || n.kind == MEK_VEC) return { "RGB", "R", "G", "B", "A" };
    if (n.kind == MEK_SCA) return { "Value" };
    if (n.kind == MEK_MAT) return { "Material" };
    if (n.kind == MEK_COMMENT) return {};
    if (n.kind == MEK_OP && n.op == OP_TEXCOORD) return { "UV" };
    if (n.kind == MEK_OP && (n.op == OP_PANNER || n.op == OP_ROTATOR || n.op == OP_BUMPOFFSET)) return { "UV" };
    if (n.kind == MEK_OP && n.op == OP_TIME) return { "Time" };
    if (n.kind == MEK_OP && n.op == OP_MASK) { std::string t; const char* cn = "RGBA"; for (int k = 0; k < 4; k++) if (n.c[k] > 0.5f) t += cn[k]; return { t.empty() ? "Result" : t }; }
    return { "Result" };
}
static void MeOpDefaults(MeNode& n) {
    float d[OP_COUNT][4] = { { 1, 1, 0, 0 }, { 0, 1, 0, 0 }, { 1, 1, 0, 0 }, { 0, 1, 0, 0 }, { 1, 2, 0, 0 }, { 0, 1, 0.5f, 0 }, { 0, 0, 0, 0 },
                             { 0, 2, 0, 0 }, { 0, 0, 1, 0 }, { 0, 1, 0, 0 }, { 1, 1, 1, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
                             /* saturate */ { 0, 0, 0, 0 }, /* fresnel: exp, base reflect */ { 5, 0.04f, 0, 0 }, /* normal strength */ { 0, 1, 0, 0 },
                             /* panner: speed x, y */ { 0.1f, 0, 0, 0 }, /* rotator: centre x, y, speed */ { 0.5f, 0.5f, 0.25f, 0 }, /* time: speed */ { 1, 0, 0, 0 },
                             /* sin, cos: period */ { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
                             /* min, max */ { 0, 1, 0, 0 }, { 0, 1, 0, 0 }, /* dot */ { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
                             /* bump offset: ratio, reference plane */ { 0.05f, 0.5f, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
                             /* if: A, B, then values */ { 0, 0, 0, 0 } };
    memcpy(n.c, d[n.op], sizeof n.c);
    if (n.op == OP_LAYERBLEND && n.layers.empty()) n.layers = { { "Mat1", 1.f }, { "Mat2", 0.f } };
}
static bool MeIsStd(int pin) { return pin >= M.stdStart && pin < (int)M.pins.size(); }
static bool MeIsCustom(const MeNode& n) {   // anything but the output and the nodes feeding the material's own settings
    if (n.output) return false;
    for (auto& l : M.links) if (l.second == n.id && !MeIsStd(l.first)) return false;
    return true;
}

// ---- your own nodes are kept: when the graph is read again, for each material slot, and on disk
static std::wstring MeGraphDir() { return DirOf(ModelsRoot()) + L"\\material graphs"; }
static std::string MeKeyFor(int cur) {
    if (cur < 0 || cur >= (int)M.slots.size()) return "";
    return M.actor + "|" + M.slots[cur].comp + "|" + std::to_string(M.slots[cur].slot);
}
static std::wstring MeGraphFile(const std::string& key) {
    uint32_t h = 2166136261u; for (unsigned char c : key) { h ^= c; h *= 16777619u; }
    wchar_t b[16]; swprintf(b, 16, L"%08x", h);
    return MeGraphDir() + L"\\" + b + L".uegraph";
}
// text that can hold tabs and new lines (comment text) kept on one line of a file
static std::string MeEsc(const std::string& t) { std::string o; for (char ch : t) { if (ch == '\\') o += "\\\\"; else if (ch == '\t') o += "\\t"; else if (ch == '\n') o += "\\n"; else if (ch != '\r') o += ch; } return o; }
static std::string MeUnesc(const std::string& t) {
    std::string o; for (size_t i = 0; i < t.size(); i++) { if (t[i] == '\\' && i + 1 < t.size()) { char x = t[++i]; o += x == 't' ? '\t' : x == 'n' ? '\n' : x; } else o += t[i]; } return o;
}
static std::string MeNodeLine(const MeNode& n) {
    char b[256]; snprintf(b, sizeof b, "n\t%d\t%d\t%d\t%.1f\t%.1f\t%d\t%g,%g,%g,%g\t", n.id, n.kind, n.op, n.x, n.y, n.sampler, n.c[0], n.c[1], n.c[2], n.c[3]);
    std::string ly; for (auto& l : n.layers) { char w[32]; snprintf(w, sizeof w, "%g", l.second); ly += (ly.empty() ? "" : ";") + l.first + ":" + w; }
    char x[200]; snprintf(x, sizeof x, "\t%g,%g,%g,%g,%g\t%d\t%g,%g,%u", n.tile[0], n.tile[1], n.off[0], n.off[1], n.rot, n.filter, n.w, n.h, n.col);
    return b + n.value + "\t" + ly + "\t" + MeEsc(n.name) + x + "\n";
}
static bool MeParseNode(const std::vector<std::string>& t, MeNode& n) {
    if (t.size() < 9 || t[0] != "n") return false;
    n.id = atoi(t[1].c_str()); n.kind = atoi(t[2].c_str()); n.op = atoi(t[3].c_str()); n.x = (float)atof(t[4].c_str()); n.y = (float)atof(t[5].c_str());
    n.sampler = atoi(t[6].c_str()); sscanf(t[7].c_str(), "%f,%f,%f,%f", &n.c[0], &n.c[1], &n.c[2], &n.c[3]); n.value = t[8];
    if (t.size() > 9 && !t[9].empty()) { std::istringstream ls(t[9]); std::string it; while (std::getline(ls, it, ';')) { size_t k = it.rfind(':'); if (k != std::string::npos) n.layers.push_back({ it.substr(0, k), (float)atof(it.c_str() + k + 1) }); } }
    if (t.size() > 10) n.name = MeUnesc(t[10]);
    if (t.size() > 11) sscanf(t[11].c_str(), "%f,%f,%f,%f,%f", &n.tile[0], &n.tile[1], &n.off[0], &n.off[1], &n.rot);
    if (t.size() > 12) n.filter = atoi(t[12].c_str());
    if (t.size() > 13) sscanf(t[13].c_str(), "%f,%f,%u", &n.w, &n.h, &n.col);
    if (n.op < 0 || n.op >= OP_COUNT) n.op = 0;
    return true;
}
static std::string MePropsLine(const MeProps& p) {
    char b[160]; snprintf(b, sizeof b, "mp\t%d\t%d\t%d\t%g\t%g\t%g\n", p.blend, p.twoSided ? 1 : 0, p.unlit ? 1 : 0, p.clip, p.dispScale, p.wpoScale);
    return b;
}
static bool MeParseProps(const std::vector<std::string>& t, MeProps& p) {
    if (t.size() < 6 || t[0] != "mp") return false;
    p.blend = atoi(t[1].c_str()); p.twoSided = atoi(t[2].c_str()) != 0; p.unlit = atoi(t[3].c_str()) != 0; p.clip = (float)atof(t[4].c_str()); p.dispScale = (float)atof(t[5].c_str());
    if (t.size() > 6) p.wpoScale = (float)atof(t[6].c_str());
    return true;
}
static MeCustom MeTakeCustom() {
    MeCustom c; c.props = M.props; c.hasProps = true;
    std::set<int> ids;
    for (auto& n : M.nodes) if (MeIsCustom(n)) { c.nodes.push_back(n); ids.insert(n.id); }
    for (auto& kv : M.nin) if (ids.count((int)(kv.first / 64)) && ids.count(kv.second.first)) c.nin[kv.first] = kv.second;
    for (auto& l : M.links) if (MeIsStd(l.first) && ids.count(l.second)) c.std[l.first - M.stdStart] = { l.second, M.linkOut.count(l.first) ? M.linkOut[l.first] : 0 };
    std::map<int, std::string> pkey;                                   // the material's own setting nodes, by name
    for (int i = 0; i < M.stdStart && i < (int)M.pins.size(); i++) { auto it = M.links.find(i); if (it != M.links.end()) pkey[it->second] = std::to_string(M.pins[i].kind) + "|" + M.pins[i].name; }
    for (auto& l : M.links) if (MeIsStd(l.first) && pkey.count(l.second)) c.stdp[l.first - M.stdStart] = { pkey[l.second], M.linkOut.count(l.first) ? M.linkOut[l.first] : 0 };
    for (auto& kv : M.nin) if (ids.count((int)(kv.first / 64)) && pkey.count(kv.second.first)) c.ninp[kv.first] = { pkey[kv.second.first], kv.second.second };
    return c;
}
static void MeSaveGraphFile(const std::string& key, const MeCustom& c) {
    if (key.empty()) return;
    std::string o = "UEGRAPH 1\nkey\t" + key + "\n";
    if (c.hasProps) o += MePropsLine(c.props);
    for (auto& n : c.nodes) o += MeNodeLine(n);
    for (auto& kv : c.nin) o += "i\t" + std::to_string(kv.first / 64) + "\t" + std::to_string(kv.first % 64) + "\t" + std::to_string(kv.second.first) + "\t" + std::to_string(kv.second.second) + "\n";
    for (auto& kv : c.std) o += "s\t" + std::to_string(kv.first) + "\t" + std::to_string(kv.second.first) + "\t" + std::to_string(kv.second.second) + "\n";
    for (auto& kv : c.stdp) o += "sp\t" + std::to_string(kv.first) + "\t" + std::to_string(kv.second.second) + "\t" + kv.second.first + "\n";
    for (auto& kv : c.ninp) o += "ip\t" + std::to_string(kv.first / 64) + "\t" + std::to_string(kv.first % 64) + "\t" + std::to_string(kv.second.second) + "\t" + kv.second.first + "\n";
    MakeDirs(MeGraphDir());
    HANDLE h = CreateFileW(MeGraphFile(key).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, o.data(), (DWORD)o.size(), &w, nullptr); CloseHandle(h); }
}
static bool MeLoadGraphFile(const std::string& key, MeCustom& c) {
    std::string d; if (!MeReadAll(MeGraphFile(key), d) || d.rfind("UEGRAPH", 0) != 0) return false;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto t = MeTabs(line); if (t.empty()) continue;
        MeNode n;
        if (MeParseNode(t, n)) c.nodes.push_back(n);
        else if (MeParseProps(t, c.props)) c.hasProps = true;
        else if (t[0] == "i" && t.size() >= 5) c.nin[MeInKey(atoi(t[1].c_str()), atoi(t[2].c_str()))] = { atoi(t[3].c_str()), atoi(t[4].c_str()) };
        else if (t[0] == "s" && t.size() >= 4) c.std[atoi(t[1].c_str())] = { atoi(t[2].c_str()), atoi(t[3].c_str()) };
        else if (t[0] == "sp" && t.size() >= 4) c.stdp[atoi(t[1].c_str())] = { t[3], atoi(t[2].c_str()) };
        else if (t[0] == "ip" && t.size() >= 5) c.ninp[MeInKey(atoi(t[1].c_str()), atoi(t[2].c_str()))] = { t[4], atoi(t[3].c_str()) };
    }
    return !c.empty() || c.hasProps;
}

static void MeHistoryReset();
static void MeBuild() {
    if (!M.builtKey.empty()) { M.saved[M.builtKey] = MeTakeCustom(); MeSaveGraphFile(M.builtKey, M.saved[M.builtKey]); }   // keep your own nodes for this slot
    M.nodes.clear(); M.links.clear(); M.linkOut.clear(); M.nin.clear(); M.pins.clear(); M.sel = -1; M.pend.clear(); M.selSet.clear();
    M.builtFor = M.cur; M.builtKey = MeKeyFor(M.cur);
    MeHistoryReset();
    if (M.cur >= (int)M.slots.size()) return;
    const MeSlot& s = M.slots[M.cur];
    MeParam mp; mp.kind = MEK_MAT; mp.name = "Material"; mp.value = s.mat; mp.orig = s.orig;
    M.pins.push_back(mp);
    std::vector<MeParam> ps = s.params;
    std::stable_sort(ps.begin(), ps.end(), [](const MeParam& a, const MeParam& b) { return a.kind < b.kind; });
    for (auto& p : ps) M.pins.push_back(p);
    M.stdStart = (int)M.pins.size();
    for (int k = 0; k < STD_N; k++) { MeParam sp; sp.kind = MEK_STD; sp.name = g_meStd[k]; M.pins.push_back(sp); }
    MeNode out; out.id = M.nextId++; out.output = true; out.x = 0; out.y = 0;
    M.nodes.push_back(out);
    for (int i = 0; i < M.stdStart; i++) {
        MeNode n; n.id = M.nextId++; n.kind = M.pins[i].kind; n.value = M.pins[i].value;
        // a texture the graph made itself stands for the material's own one (so baking again starts from the
        // original, not from its last result)
        if (n.kind == MEK_TEX && n.value.find("baked materials") != std::string::npos && !M.pins[i].orig.empty() && M.pins[i].orig.find("baked materials") == std::string::npos) n.value = M.pins[i].orig;
        if (n.kind == MEK_TEX) { std::string l = W2U(Lower(U2W(M.pins[i].name))); if (l.find("normal") != std::string::npos || l == "n" || l == "nm") n.sampler = 1; }
        M.nodes.push_back(n);
        M.links[i] = n.id;
    }
    MeArrange();
    // your own nodes for this slot: from this session, or from disk
    MeCustom c; bool have = false;
    auto it = M.saved.find(M.builtKey);
    if (it != M.saved.end()) { c = it->second; have = !c.empty(); }
    else have = MeLoadGraphFile(M.builtKey, c);
    M.props = MeProps();
    if (c.hasProps) M.props = c.props;
    if (have) {
        std::map<int, int> idMap;                                           // (fresh ids, so they never clash)
        for (auto& n : c.nodes) { MeNode k = n; k.id = M.nextId++; idMap[n.id] = k.id; M.nodes.push_back(k); }
        for (auto& kv : c.nin) { int a = (int)(kv.first / 64), in = (int)(kv.first % 64); if (idMap.count(a) && idMap.count(kv.second.first)) M.nin[MeInKey(idMap[a], in)] = { idMap[kv.second.first], kv.second.second }; }
        for (auto& kv : c.std) if (idMap.count(kv.second.first) && kv.first < STD_N) { M.links[M.stdStart + kv.first] = idMap[kv.second.first]; M.linkOut[M.stdStart + kv.first] = kv.second.second; }
        std::map<std::string, int> byName;                                 // the material's own setting nodes, by name
        for (int i = 0; i < M.stdStart; i++) { auto l = M.links.find(i); if (l != M.links.end()) byName[std::to_string(M.pins[i].kind) + "|" + M.pins[i].name] = l->second; }
        for (auto& kv : c.stdp) if (byName.count(kv.second.first) && kv.first < STD_N) { M.links[M.stdStart + kv.first] = byName[kv.second.first]; M.linkOut[M.stdStart + kv.first] = kv.second.second; }
        for (auto& kv : c.ninp) { int a = (int)(kv.first / 64), in = (int)(kv.first % 64); if (idMap.count(a) && byName.count(kv.second.first)) M.nin[MeInKey(idMap[a], in)] = { byName[kv.second.first], kv.second.second }; }
    }
}

// ---- sending changes to the game
static void MeStdChanged() { M.stdDirty = true; M.stdChangedAt = GetTickCount64(); }
static bool MeStdUsed() { for (auto& l : M.links) if (MeIsStd(l.first)) return true; return false; }
static void MeSend(int pin, const std::string& value, bool now) {
    if (M.cur >= (int)M.slots.size() || pin < 0 || pin >= (int)M.pins.size()) return;
    if (MeIsStd(pin)) { MeStdChanged(); return; }                       // (Unreal inputs are baked, not sent)
    const MeSlot& s = M.slots[M.cur]; const MeParam& p = M.pins[pin];
    std::string key = std::to_string(pin);
    std::string arg = M.actor + "\t" + s.comp + "\t" + std::to_string(s.slot) + "\t" + MeKindCode(p.kind) + "\t" + (p.kind == MEK_MAT ? "" : p.name) + "\t" + value;
    M.lastChange = GetTickCount64(); M.prevWanted = true;
    auto it = M.pend.find(key);
    if (!now && it != M.pend.end() && GetTickCount64() - it->second.second < 50) { it->second.first = arg; return; }
    Ed("ed_matset", arg);
    M.pend[key] = { "", GetTickCount64() };
    if (p.kind == MEK_MAT) { M.pend.clear(); MeAskGraph(); }    // a different material has different settings: read them again
}
static void MeBakeStep();
static void MeFlush() {
    for (auto& kv : M.pend) if (!kv.second.first.empty() && GetTickCount64() - kv.second.second >= 50) {
        Ed("ed_matset", kv.second.first); kv.second = { "", GetTickCount64() };
    }
    MeBakeStep();
    // a new preview a moment after the last change
    if (M.prevWanted && GetTickCount64() - M.lastChange > 350 && M.cur < (int)M.slots.size()) {
        M.prevWanted = false;
        std::wstring dir = ThumbDir();
        if (!g_thumbDirSent || g_thumbDirFor != dir) {
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            std::string d = W2U(dir); for (auto& c : d) if (c == '\\') c = '/';
            Ed("ed_thumbdir", d); g_thumbDirSent = true; g_thumbDirFor = dir;
        }
        const MeSlot& s = M.slots[M.cur];
        std::string id = "matprev_" + std::to_string(++M.prevSeq % 8);
        DeleteFileW((dir + U2W(id) + L".png").c_str());
        Ed("ed_matprev", id + "\t" + M.actor + "\t" + s.comp + "\t" + std::to_string(s.slot));
        M.prevPng = W2U(dir) + id + ".png"; M.prevAsked = GetTickCount64();
    }
}
// a node's value changed: every setting it feeds gets it (and the Unreal inputs are baked again)
static void MeNodeChanged(const MeNode& n, bool now) {
    for (auto& l : M.links) if (l.second == n.id && !MeIsStd(l.first)) MeSend(l.first, n.value, now);
    if (MeStdUsed()) MeStdChanged();
}
// can this node's output go into this pin of the output node?
static bool MeCanLink(int pin, const MeNode& n, int out) {
    if (pin < 0 || pin >= (int)M.pins.size()) return false;
    if (MeIsStd(pin)) return n.kind != MEK_MAT;
    return n.kind == M.pins[pin].kind && out == 0;
}
static void MeLink(int pin, int nodeId, int out = 0) {
    MeNode* n = MeFind(nodeId); if (!n || !MeCanLink(pin, *n, out)) return;
    M.links[pin] = nodeId; M.linkOut[pin] = out;
    MeSend(pin, n->value, true);
}
static void MeUnlink(int pin) {
    if (!M.links.count(pin)) return;
    M.links.erase(pin); M.linkOut.erase(pin);
    if (MeIsStd(pin)) { MeStdChanged(); return; }
    MeSend(pin, M.pins[pin].kind == MEK_MAT ? "" : M.pins[pin].orig, true);   // an unfed setting goes back to how the material had it
}
// node inputs (maths, layer blends, a texture's UVs)
static bool MeDependsOn(int node, int on, int depth = 0) {     // does node's result use `on`?
    if (node == on) return true;
    if (depth > 64) return true;
    for (auto& kv : M.nin) if ((int)(kv.first / 64) == node && MeDependsOn(kv.second.first, on, depth + 1)) return true;
    return false;
}
static bool MeLinkIn(int node, int in, int src, int out) {
    MeNode* s = MeFind(src); MeNode* d = MeFind(node);
    if (!s || !d || s->kind == MEK_MAT || s->output || MeDependsOn(src, node)) return false;
    M.nin[MeInKey(node, in)] = { src, out };
    MeStdChanged();
    return true;
}
static void MeUnlinkIn(int node, int in) { if (M.nin.erase(MeInKey(node, in))) MeStdChanged(); }
static void MeDeleteNode(int id) {
    MeNode* n = MeFind(id); if (!n || n->output) return;
    std::vector<int> fed; for (auto& l : M.links) if (l.second == id) fed.push_back(l.first);
    for (int p : fed) MeUnlink(p);
    for (auto it = M.nin.begin(); it != M.nin.end();) { if ((int)(it->first / 64) == id || it->second.first == id) { it = M.nin.erase(it); MeStdChanged(); } else ++it; }
    M.nodes.erase(std::remove_if(M.nodes.begin(), M.nodes.end(), [&](const MeNode& x) { return x.id == id; }), M.nodes.end());
    if (M.sel == id) M.sel = -1;
}
static int MeAddNode(int kind, float x, float y, const std::string& value, int op = 0) {
    MeNode n; n.id = M.nextId++; n.kind = kind; n.x = x; n.y = y; n.value = value; n.op = op;
    if (n.value.empty()) n.value = kind == MEK_VEC ? "1.0000,1.0000,1.0000,1.0000" : kind == MEK_SCA ? "0.5000" : "";
    if (kind == MEK_OP) MeOpDefaults(n);
    M.nodes.push_back(n); M.sel = n.id;
    return n.id;
}

static std::wstring MeSafeName(std::string n); static void MeExMsg(const std::string& t, bool err);
// ---- baking the Unreal inputs: the graph is worked out for every pixel (in the background), written as textures,
// and the game puts them into the material's texture slots by what each slot is for (colour, normal map, packed
// metal / roughness / occlusion...). A material with no such slots is swapped for a game material that has them.
struct MgImg { int w = 0, h = 0; std::vector<unsigned char> px; bool ok = false; };
struct MgV { float v[4] = { 0, 0, 0, 0 }; int n = 1; };
struct MgN { int kind = 0, op = 0, sampler = 0; float c[4] = { 0, 0, 0, 0 }; float col[4] = { 0, 0, 0, 0 }; std::vector<float> lw;
             std::vector<std::pair<int, int>> in; std::string tex; std::shared_ptr<MgImg> img;
             float tile[2] = { 1, 1 }, off[2] = { 0, 0 }, rot = 0; int filter = 0; int id = 0; };
struct MgProg { std::vector<MgN> n; int std[STD_N]; int stdOut[STD_N]; std::wstring paks; float time = 0; };
static std::map<std::string, std::shared_ptr<MgImg>> g_mgImgs; static std::mutex g_mgImgMx;
static std::mutex g_mgDoneMx; static std::string g_mgDoneArg, g_mgDoneErr;
static float g_mgSrgbLut[256];
static std::shared_ptr<MgImg> MgLoadImg(const std::string& value, const std::wstring& paks) {
    { std::lock_guard<std::mutex> lk(g_mgImgMx); auto it = g_mgImgs.find(value); if (it != g_mgImgs.end()) return it->second; }
    auto im = std::make_shared<MgImg>(); std::string err;
    im->ok = !value.empty() && PictureRGBA(paks, value, 2048, im->px, im->w, im->h, err) && im->w > 0;
    std::lock_guard<std::mutex> lk(g_mgImgMx); g_mgImgs[value] = im;
    return im;
}
static MgV MgSwz(const MgV& v, const MgN& n, int out) {
    MgV r;
    if (n.kind == MEK_TEX || n.kind == MEK_VEC) {
        if (out == 0) { r = v; r.n = 3; return r; }
        r.v[0] = r.v[1] = r.v[2] = r.v[3] = v.v[out - 1]; r.n = 1; return r;
    }
    return v;
}
static inline float MgC(const MgV& a, int k) { return a.n == 1 ? a.v[0] : (k < a.n ? a.v[k] : 0.f); }
static MgV MgBin(const MgV& a, const MgV& b, int op) {
    MgV r; r.n = std::max(a.n, b.n);
    for (int k = 0; k < 4; k++) {
        float x = MgC(a, k), y = MgC(b, k);
        r.v[k] = op == OP_ADD ? x + y : op == OP_SUB ? x - y : op == OP_MUL ? x * y : (std::fabs(y) > 1e-6f ? x / y : 0.f);
    }
    return r;
}
// a texture node's own UV transform: tiling and offset, then turned about the middle
static inline void MgUvXf(const MgN& n, float& u, float& v) {
    u = u * n.tile[0] + n.off[0]; v = v * n.tile[1] + n.off[1];
    if (n.rot != 0) { float a = n.rot * 3.14159265f / 180.f, cs = cosf(a), sn = sinf(a), x = u - 0.5f, y = v - 0.5f; u = x * cs - y * sn + 0.5f; v = x * sn + y * cs + 0.5f; }
}
static void MgSample(const MgImg& im, float u, float v, int sampler, float out[4], int filter = 0) {
    if (!im.ok) { out[0] = out[1] = out[2] = 0.5f; out[3] = 1; if (sampler == 1) { out[0] = out[1] = 0; out[2] = 1; } return; }
    if (filter == 1) {                           // nearest
        int xx = (int)floorf((u - floorf(u)) * im.w), yy = (int)floorf((v - floorf(v)) * im.h);
        xx = std::clamp(xx, 0, im.w - 1); yy = std::clamp(yy, 0, im.h - 1);
        const unsigned char* p = &im.px[((size_t)yy * im.w + xx) * 4];
        for (int k = 0; k < 4; k++) out[k] = (sampler == 0 && k < 3) ? g_mgSrgbLut[p[k]] : p[k] / 255.f;
        if (sampler == 1) { float nx = out[0] * 2 - 1, ny = out[1] * 2 - 1; out[0] = nx; out[1] = ny; out[2] = sqrtf(std::max(0.f, 1 - nx * nx - ny * ny)); }
        return;
    }
    float x = (u - floorf(u)) * im.w - 0.5f, y = (v - floorf(v)) * im.h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y); float fx = x - x0, fy = y - y0;
    auto px = [&](int xx, int yy) { xx = ((xx % im.w) + im.w) % im.w; yy = ((yy % im.h) + im.h) % im.h; return &im.px[((size_t)yy * im.w + xx) * 4]; };
    const unsigned char *a = px(x0, y0), *b = px(x0 + 1, y0), *c = px(x0, y0 + 1), *d = px(x0 + 1, y0 + 1);
    for (int k = 0; k < 4; k++) {
        auto cv = [&](const unsigned char* p) { return (sampler == 0 && k < 3) ? g_mgSrgbLut[p[k]] : p[k] / 255.f; };
        out[k] = (cv(a) * (1 - fx) + cv(b) * fx) * (1 - fy) + (cv(c) * (1 - fx) + cv(d) * fx) * fy;
    }
    if (sampler == 1) {                          // a normal map: -1..1, the third part worked out (game normal maps often keep only two)
        float nx = out[0] * 2 - 1, ny = out[1] * 2 - 1;
        out[0] = nx; out[1] = ny; out[2] = sqrtf(std::max(0.f, 1 - nx * nx - ny * ny));
    }
}
// one pixel: every node in order (sources come before the nodes that use them)
static void MgEvalPixel(const MgProg& P, float u, float v, std::vector<MgV>& val) {
    for (size_t i = 0; i < P.n.size(); i++) {
        const MgN& n = P.n[i];
        auto in = [&](int k, float def) -> MgV { MgV d; d.v[0] = d.v[1] = d.v[2] = d.v[3] = def; d.n = 1;
            if (k >= (int)n.in.size() || n.in[k].first < 0) return d; return MgSwz(val[n.in[k].first], P.n[n.in[k].first], n.in[k].second); };
        MgV r;
        switch (n.kind) {
        case MEK_TEX: {
            float uu = u, vv = v;
            if (!n.in.empty() && n.in[0].first >= 0) { MgV t = in(0, 0); uu = MgC(t, 0); vv = MgC(t, 1); }
            MgUvXf(n, uu, vv);
            if (n.img) MgSample(*n.img, uu, vv, n.sampler, r.v, n.filter); r.n = 4; break;
        }
        case MEK_VEC: memcpy(r.v, n.col, sizeof r.v); r.n = 4; break;
        case MEK_SCA: r.v[0] = r.v[1] = r.v[2] = r.v[3] = n.col[0]; r.n = 1; break;
        case MEK_OP:
            switch (n.op) {
            case OP_TEXCOORD: r.v[0] = u * n.c[0]; r.v[1] = v * n.c[1]; r.n = 2; break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: r = MgBin(in(0, n.c[0]), in(1, n.c[1]), n.op); break;
            case OP_LERP: { MgV a = in(0, n.c[0]), b = in(1, n.c[1]), t = in(2, n.c[2]); r.n = std::max(a.n, b.n);
                for (int k = 0; k < 4; k++) { float x = MgC(a, k), y = MgC(b, k), w = MgC(t, t.n == 1 ? 0 : k); r.v[k] = x + (y - x) * w; } break; }
            case OP_ONEMINUS: { MgV a = in(0, 0); r.n = a.n; for (int k = 0; k < 4; k++) r.v[k] = 1 - MgC(a, k); break; }
            case OP_POW: { MgV a = in(0, n.c[0]), e = in(1, n.c[1]); r.n = std::max(a.n, e.n); for (int k = 0; k < 4; k++) r.v[k] = powf(std::max(0.f, MgC(a, k)), MgC(e, k)); break; }
            case OP_CLAMP: { MgV a = in(0, n.c[0]), lo = in(1, n.c[1]), hi = in(2, n.c[2]); r.n = a.n; for (int k = 0; k < 4; k++) r.v[k] = std::min(std::max(MgC(a, k), MgC(lo, k)), MgC(hi, k)); break; }
            case OP_DESAT: { MgV a = in(0, 0), f = in(1, n.c[1]); float l = MgC(a, 0) * 0.3f + MgC(a, 1) * 0.59f + MgC(a, 2) * 0.11f; r.n = a.n;
                for (int k = 0; k < 4; k++) r.v[k] = MgC(a, k) + (l - MgC(a, k)) * MgC(f, 0); break; }
            case OP_MASK: { MgV a = in(0, 0); int m = 0; for (int k = 0; k < 4; k++) if (n.c[k] > 0.5f) r.v[m++] = MgC(a, k); r.n = std::max(1, m); break; }
            case OP_APPEND: { MgV a = in(0, 0), b = in(1, 0); int m = 0; for (int k = 0; k < a.n && m < 4; k++) r.v[m++] = a.v[k]; for (int k = 0; k < b.n && m < 4; k++) r.v[m++] = b.v[k]; r.n = std::max(1, m); break; }
            case OP_LAYERBLEND: {               // each layer weighted (its weight input, or its preview weight), the weights adding up to 1
                float sum = 0, acc[4] = { 0, 0, 0, 0 }; int nn = 1;
                for (size_t l = 0; l < n.lw.size(); l++) {
                    float w = (2 * l + 1 < n.in.size() && n.in[2 * l + 1].first >= 0) ? std::clamp(MgC(in((int)(2 * l + 1), 0), 0), 0.f, 1.f) : n.lw[l];
                    if (w <= 0) continue;
                    MgV L = in((int)(2 * l), 0); nn = std::max(nn, L.n);
                    for (int k = 0; k < 4; k++) acc[k] += MgC(L, k) * w;
                    sum += w;
                }
                for (int k = 0; k < 4; k++) r.v[k] = sum > 0 ? acc[k] / sum : 0;
                r.n = nn; break;
            }
            // (things that depend on the camera or the world have no meaning in a flat texture: they're baked
            // as if seen straight on, at the start of time; the compiler says so)
            case OP_SATURATE: { MgV a = in(0, 0); r.n = a.n; for (int k = 0; k < 4; k++) r.v[k] = std::clamp(MgC(a, k), 0.f, 1.f); break; }
            case OP_ABS: { MgV a = in(0, 0); r.n = a.n; for (int k = 0; k < 4; k++) r.v[k] = std::fabs(MgC(a, k)); break; }
            case OP_FRAC: { MgV a = in(0, 0); r.n = a.n; for (int k = 0; k < 4; k++) { float x = MgC(a, k); r.v[k] = x - floorf(x); } break; }
            case OP_FLOOR: { MgV a = in(0, 0); r.n = a.n; for (int k = 0; k < 4; k++) r.v[k] = floorf(MgC(a, k)); break; }
            case OP_SQRT: { MgV a = in(0, 0); r.n = a.n; for (int k = 0; k < 4; k++) r.v[k] = sqrtf(std::max(0.f, MgC(a, k))); break; }
            case OP_SIN: case OP_COS: { MgV a = in(0, 0); r.n = a.n; float f = n.c[0] != 0 ? 6.2831853f / n.c[0] : 1.f;
                for (int k = 0; k < 4; k++) r.v[k] = n.op == OP_SIN ? sinf(MgC(a, k) * f) : cosf(MgC(a, k) * f); break; }
            case OP_MIN: case OP_MAX: { MgV a = in(0, n.c[0]), b = in(1, n.c[1]); r.n = std::max(a.n, b.n);
                for (int k = 0; k < 4; k++) r.v[k] = n.op == OP_MIN ? std::min(MgC(a, k), MgC(b, k)) : std::max(MgC(a, k), MgC(b, k)); break; }
            case OP_DOT: { MgV a = in(0, 0), b = in(1, 0); int m = std::max(a.n, b.n); float d = 0; for (int k = 0; k < m; k++) d += MgC(a, k) * MgC(b, k);
                r.v[0] = r.v[1] = r.v[2] = r.v[3] = d; r.n = 1; break; }
            case OP_NORMALIZE: { MgV a = in(0, 0); r.n = a.n; float l = 0; for (int k = 0; k < a.n; k++) l += MgC(a, k) * MgC(a, k); l = sqrtf(l);
                for (int k = 0; k < 4; k++) r.v[k] = l > 1e-8f ? MgC(a, k) / l : 0; break; }
            case OP_FRESNEL: { MgV b = in(1, n.c[1]); r.v[0] = r.v[1] = r.v[2] = r.v[3] = MgC(b, 0); r.n = 1; break; }   // straight on: just the base reflection
            case OP_NORMALSTR: { MgV a = in(0, 0), st = in(1, n.c[1]);
                float x = MgC(a, 0), y = MgC(a, 1), z = a.n >= 3 ? MgC(a, 2) : 1; if (n.in.empty() || n.in[0].first < 0) { x = 0; y = 0; z = 1; }
                float sc = MgC(st, 0); x *= sc; y *= sc; float l = sqrtf(x * x + y * y + z * z); if (l < 1e-8f) l = 1;
                r.v[0] = x / l; r.v[1] = y / l; r.v[2] = z / l; r.v[3] = 0; r.n = 3; break; }
            case OP_TIME: r.v[0] = r.v[1] = r.v[2] = r.v[3] = P.time * n.c[0]; r.n = 1; break;
            case OP_PANNER: case OP_ROTATOR: case OP_BUMPOFFSET: {
                float uu = u, vv = v;
                if (!n.in.empty() && n.in[0].first >= 0) { MgV t = in(0, 0); uu = MgC(t, 0); vv = MgC(t, 1); }
                float tm = (n.in.size() > 1 && n.in[1].first >= 0 && n.op != OP_BUMPOFFSET) ? MgC(in(1, 0), 0) : P.time;
                if (n.op == OP_PANNER) { uu += n.c[0] * tm; vv += n.c[1] * tm; }
                else if (n.op == OP_ROTATOR) { float a = n.c[2] * tm, cs = cosf(a), sn = sinf(a), x = uu - n.c[0], y = vv - n.c[1]; uu = x * cs - y * sn + n.c[0]; vv = x * sn + y * cs + n.c[1]; }
                r.v[0] = uu; r.v[1] = vv; r.n = 2; break; }
            case OP_VNORMAL: case OP_CAMVEC: r.v[0] = 0; r.v[1] = 0; r.v[2] = 1; r.n = 3; break;
            case OP_WORLDPOS: r.v[0] = u * 100; r.v[1] = v * 100; r.v[2] = 0; r.n = 3; break;
            case OP_IF: { MgV a = in(0, 0), b = in(1, 0), gt = in(2, 0), eq = in(3, 0), lt = in(4, 0);
                float x = MgC(a, 0), y = MgC(b, 0); const MgV& pick = x > y ? gt : (std::fabs(x - y) < 1e-5f ? eq : lt);
                r = pick; break; }
            }
            break;
        }
        val[i] = r;
    }
}
static double MgToSrgb(double c) { c = std::clamp(c, 0.0, 1.0); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055; }
// everything wired into the Unreal inputs, as a list the background thread can work through
// a node (and everything feeding it) put on the program's list, sources first; returns its place
static int MgAddNode(MgProg& P, std::map<int, int>& idx, int id) {
    auto it = idx.find(id); if (it != idx.end()) return it->second < 0 ? -1 : it->second;
    MeNode* n = MeFind(id); if (!n || n->kind == MEK_COMMENT || n->kind == MEK_MAT || n->output) return -1;
    idx[id] = -2;                                // (being added: a loop can't happen, links refuse them)
    MgN g; g.kind = n->kind; g.op = n->op; g.sampler = n->sampler; memcpy(g.c, n->c, sizeof g.c); g.id = n->id;
    g.tile[0] = n->tile[0]; g.tile[1] = n->tile[1]; g.off[0] = n->off[0]; g.off[1] = n->off[1]; g.rot = n->rot; g.filter = n->filter;
    if (n->kind == MEK_VEC) MeColour(MeParamValue(*n), g.col);
    if (n->kind == MEK_SCA) g.col[0] = (float)atof(MeParamValue(*n).c_str());
    if (n->kind == MEK_TEX) g.tex = MeParamValue(*n);
    for (auto& l : n->layers) g.lw.push_back(l.second);
    int ni = (int)MeNodeIns(*n).size();
    for (int k = 0; k < ni; k++) {
        auto it2 = M.nin.find(MeInKey(id, k));
        g.in.push_back(it2 == M.nin.end() ? std::make_pair(-1, 0) : std::make_pair(MgAddNode(P, idx, it2->second.first), it2->second.second));
    }
    P.n.push_back(g);
    return idx[id] = (int)P.n.size() - 1;
}
static bool MgCompile(MgProg& P, std::string& made) {
    std::map<int, int> idx;                      // node id -> place in P.n
    auto add = [&](int id) { return MgAddNode(P, idx, id); };
    made.clear(); P.paks = PaksDir();
    for (int k = 0; k < STD_N; k++) {
        P.std[k] = -1; P.stdOut[k] = 0;
        auto it = M.links.find(M.stdStart + k);
        if (it == M.links.end()) continue;
        P.std[k] = add(it->second); P.stdOut[k] = M.linkOut.count(M.stdStart + k) ? M.linkOut[M.stdStart + k] : 0;
    }
    auto has = [&](int k) { return P.std[k] >= 0; };
    if (has(STD_BASE)) made += "bc,";
    if (has(STD_NORMAL)) made += "n,";
    if (has(STD_METAL) || has(STD_ROUGH) || has(STD_AO) || has(STD_SPEC)) made += "pack,";
    if (has(STD_EMIS)) made += "em,";
    if (has(STD_OPAC) || has(STD_OPMASK)) made += "op,";
    return !made.empty();
}
static void MgBake(MgProg P, std::string made, int res, std::wstring prefix, std::string argHead) {
    for (int i = 0; i < 256; i++) g_mgSrgbLut[i] = (float)(i / 255.0 <= 0.04045 ? i / 255.0 / 12.92 : pow((i / 255.0 + 0.055) / 1.055, 2.4));
    for (auto& n : P.n) if (n.kind == MEK_TEX) n.img = MgLoadImg(n.tex, P.paks);      // (game textures are read from its files: can take a moment)
    int W = res, H = res;
    std::vector<unsigned char> bc((size_t)W * H * 4), nm(bc.size()), orm(bc.size()), mra(bc.size()), rma(bc.size()), ro(bc.size()), me(bc.size()), ao(bc.size()), em(bc.size()), op(bc.size());
    unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < threads; t++) th.emplace_back([&, t]() {
        std::vector<MgV> val(P.n.size());
        auto get = [&](int k, float d0, float d1, float d2, int want) -> MgV {
            MgV r; r.v[0] = d0; r.v[1] = d1; r.v[2] = d2; r.v[3] = 1; r.n = want;
            if (P.std[k] < 0) return r;
            MgV v = MgSwz(val[P.std[k]], P.n[P.std[k]], P.stdOut[k]);
            for (int c = 0; c < 3; c++) r.v[c] = MgC(v, c);
            return r;
        };
        // (pictures read as plain numbers by the game come out right: it decodes them as colour, so they're stored pre-encoded)
        auto lin = [](float x) { return (unsigned char)lround(MgToSrgb(x) * 255); };
        auto col = [](float x) { return (unsigned char)lround(MgToSrgb(x) * 255); };
        for (int y = (int)t; y < H; y += (int)threads) for (int x = 0; x < W; x++) {
            MgEvalPixel(P, (x + 0.5f) / W, (y + 0.5f) / H, val);
            MgV b = get(STD_BASE, 0, 0, 0, 3), mt = get(STD_METAL, 0, 0, 0, 1), rg = get(STD_ROUGH, 0.5f, 0.5f, 0.5f, 1), oc = get(STD_AO, 1, 1, 1, 1);
            MgV e = get(STD_EMIS, 0, 0, 0, 3), o = get(P.std[STD_OPAC] < 0 && P.std[STD_OPMASK] >= 0 ? STD_OPMASK : STD_OPAC, 1, 1, 1, 1), nn = get(STD_NORMAL, 0, 0, 1, 3);
            size_t i = ((size_t)y * W + x) * 4;
            float m = std::clamp(mt.v[0], 0.f, 1.f), r = std::clamp(rg.v[0], 0.f, 1.f), a = std::clamp(oc.v[0], 0.f, 1.f), opa = std::clamp(o.v[0], 0.f, 1.f);
            for (int c = 0; c < 3; c++) { bc[i + c] = col(b.v[c]); em[i + c] = col(e.v[c]); }
            bc[i + 3] = (unsigned char)lround(opa * 255);
            float l = sqrtf(nn.v[0] * nn.v[0] + nn.v[1] * nn.v[1] + nn.v[2] * nn.v[2]); if (l < 1e-5f) { nn.v[0] = nn.v[1] = 0; nn.v[2] = l = 1; }
            for (int c = 0; c < 3; c++) nm[i + c] = lin(nn.v[c] / l * 0.5f + 0.5f);
            orm[i] = lin(a); orm[i + 1] = lin(r); orm[i + 2] = lin(m);
            mra[i] = lin(m); mra[i + 1] = lin(r); mra[i + 2] = lin(a);
            rma[i] = lin(r); rma[i + 1] = lin(m); rma[i + 2] = lin(a);
            ro[i] = ro[i + 1] = ro[i + 2] = lin(r); me[i] = me[i + 1] = me[i + 2] = lin(m); ao[i] = ao[i + 1] = ao[i + 2] = lin(a);
            op[i] = op[i + 1] = op[i + 2] = lin(opa);
            for (auto* im : { &nm, &orm, &mra, &rma, &ro, &me, &ao, &em, &op }) (*im)[i + 3] = 255;
        }
    });
    for (auto& t : th) t.join();
    std::string err;
    struct { const wchar_t* sfx; std::vector<unsigned char>* px; const char* need; } outs[] = {
        { L"_basecolor", &bc, "bc," }, { L"_normal", &nm, "n," }, { L"_orm", &orm, "pack," }, { L"_mra", &mra, "pack," }, { L"_rma", &rma, "pack," },
        { L"_rough", &ro, "pack," }, { L"_metal", &me, "pack," }, { L"_ao", &ao, "pack," }, { L"_emissive", &em, "em," }, { L"_opacity", &op, "op," } };
    for (auto& o : outs) if (made.find(o.need) != std::string::npos && !WritePngRGBA(prefix + o.sfx + L".png", *o.px, W, H)) err = "Couldn't save the baked textures.";
    std::string p = W2U(prefix);
    if (argHead.empty()) { MeExMsg(err.empty() ? "Exported baked textures: " + p + "_*.png" : err, !err.empty()); return; }   // (an export, not for the game)
    std::lock_guard<std::mutex> lk(g_mgDoneMx);
    if (err.empty()) g_mgDoneArg = argHead + "\t" + p + "\t" + made; else g_mgDoneErr = err;
    g_framesLeft = 3;
}
// once a frame: bake again a moment after the Unreal inputs change (Live Update), and hand finished bakes to the game
static void MeStartBake() {
    if (M.baking || M.cur >= (int)M.slots.size()) return;
    MgProg P; std::string made;
    if (!MgCompile(P, made)) { M.stdDirty = false; return; }
    const MeSlot& s = M.slots[M.cur];
    std::wstring dir = DirOf(ModelsRoot()) + L"\\baked materials";
    MakeDirs(dir);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t stamp[64]; swprintf(stamp, 64, L"_%04d%02d%02d-%02d%02d%02d-%03d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring prefix = dir + L"\\" + MeSafeName(MeShort(s.mat)) + stamp;     // (a new name each time: the game keeps textures by file name)
    std::string head = M.actor + "\t" + s.comp + "\t" + std::to_string(s.slot);
    M.stdDirty = false; M.baking = true; M.bakeNote = "Baking...";
    MeSaveGraphFile(M.builtKey, MeTakeCustom());
    int res = M.bakeRes;
    std::thread([P, made, res, prefix, head]() { MgBake(P, made, res, prefix, head); M.baking = false; }).detach();
}
static void MeBakeStep() {
    std::string arg, err;
    { std::lock_guard<std::mutex> lk(g_mgDoneMx); arg.swap(g_mgDoneArg); err.swap(g_mgDoneErr); }
    if (!arg.empty()) { Ed("ed_matbake", arg); M.bakeNote = "Baked and sent to the game"; M.prevWanted = true; M.lastChange = GetTickCount64(); }
    if (!err.empty()) { M.bakeNote = err; MeExMsg(err, true); }
    if (M.stdDirty && M.live && !M.baking && GetTickCount64() - M.stdChangedAt > 700 && ImGui::IsMouseDown(ImGuiMouseButton_Left) == false) MeStartBake();
}
// a starting graph like the material's own: its textures, each wired into the Unreal input it's for
static void MeUseItsTextures() {
    MeNode* out = MeOutput(); if (!out || M.cur >= (int)M.slots.size()) return;
    float x = out->x - 520, y = 1e9f;                // a new column left of everything, from the top
    for (auto& n : M.nodes) { x = std::min(x, n.x - 260); y = std::min(y, n.y); }
    for (auto& p : M.slots[M.cur].params) {
        if (p.kind != MEK_TEX || p.value.empty()) continue;
        std::string nm = W2U(Lower(U2W(p.name))), tv = W2U(Lower(U2W(MeShort(p.value))));
        auto ends = [&](const char* sfx) { size_t L = strlen(sfx); return tv.size() >= L && tv.compare(tv.size() - L, L, sfx) == 0; };
        auto has = [&](const char* w) { return nm.find(w) != std::string::npos; };
        // which channel goes where: -1 nothing; out 0 = RGB, 1..4 = R G B A
        int base = -1, metal = -1, rough = -1, ao = -1, normal = -1, emis = -1, opac = -1; int sampler = 0;
        if (has("normal") || nm == "n" || nm == "nm" || nm == "nrm" || ends("_n") || ends("_nm") || ends("_nrm") || ends("_norm") || ends("_normal")) { normal = 0; sampler = 1; }
        else if (nm == "mro" || nm == "mra" || nm == "mrao" || ends("_mro") || ends("_mra")) { metal = 1; rough = 2; ao = 3; sampler = 2; }
        else if (nm == "orm" || nm == "arm" || ends("_orm") || ends("_arm")) { ao = 1; rough = 2; metal = 3; sampler = 2; }
        else if (nm == "rma" || nm == "rmo" || ends("_rma")) { rough = 1; metal = 2; ao = 3; sampler = 2; }
        else if (has("rough")) { rough = 1; sampler = 2; }
        else if (has("metal")) { metal = 1; sampler = 2; }
        else if (has("occlusion") || nm == "ao" || ends("_ao")) { ao = 1; sampler = 2; }
        else if (has("emiss") || has("glow") || nm.rfind("si", 0) == 0) emis = 0;
        else if (has("opacity") || has("alpha")) { opac = 1; sampler = 2; }
        else base = 0;
        int id = MeAddNode(MEK_TEX, x, y, p.value);
        if (MeNode* n = MeFind(id)) n->sampler = sampler;
        auto wire = [&](int k, int o) { if (o >= 0 && !M.links.count(M.stdStart + k)) { M.links[M.stdStart + k] = id; M.linkOut[M.stdStart + k] = o; } };
        wire(STD_BASE, base); wire(STD_METAL, metal); wire(STD_ROUGH, rough); wire(STD_AO, ao); wire(STD_NORMAL, normal); wire(STD_EMIS, emis); wire(STD_OPAC, opac);
        y += 170;
    }
    MeStdChanged();
}


#include "mgshader.inc"
#include "mgedit.inc"

// ---- pictures of game textures (read from the game's files in the background)
struct MeTexPic { int state = 0; ID3D11ShaderResourceView* srv = nullptr; std::vector<unsigned char> px; int w = 0, h = 0; };
static std::map<std::string, MeTexPic> g_meTex; static std::mutex g_meTexMx; static std::vector<std::string> g_meTexQ; static bool g_meTexWorker = false;
static ID3D11ShaderResourceView* MeTexturePic(const std::string& value) {
    if (value.empty()) return nullptr;
    if (value.rfind("file:", 0) == 0) return MatThumbFor(U2W(value.substr(5)));
    std::lock_guard<std::mutex> lk(g_meTexMx);
    MeTexPic& t = g_meTex[value];
    if (t.state == 0) {
        t.state = 1; g_meTexQ.push_back(value);
        if (!g_meTexWorker) {
            g_meTexWorker = true;
            AesSyncNow();
            std::wstring paks = PaksDir();
            std::thread([paks] {
                for (;;) {
                    std::string path;
                    { std::lock_guard<std::mutex> lk2(g_meTexMx); if (g_meTexQ.empty()) { g_meTexWorker = false; return; } path = g_meTexQ.back(); g_meTexQ.pop_back(); }   // newest first: what's on screen now
                    std::vector<unsigned char> px; int w = 0, h = 0; std::string err;
                    bool ok = !paks.empty() && GameTexturePreview(paks, path, 128, px, w, h, err);
                    std::lock_guard<std::mutex> lk2(g_meTexMx);
                    MeTexPic& tp = g_meTex[path];
                    if (ok) { for (size_t i = 3; i < px.size(); i += 4) px[i] = 255; tp.px.swap(px); tp.w = w; tp.h = h; tp.state = 2; } else tp.state = 4;
                    g_framesLeft = 3;
                }
            }).detach();
        }
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
static void MeTexBox(ImDrawList* dl, const std::string& value, ImVec2 a, ImVec2 b, float round) {
    ID3D11ShaderResourceView* srv = MeTexturePic(value);
    if (srv) dl->AddImageRounded((ImTextureID)srv, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, round);
    else {
        dl->AddRectFilled(a, b, IM_COL32(40, 40, 46, 255), round);
        // checker, so "no picture" doesn't look like a black texture
        float c = (b.x - a.x) / 4;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if ((x + y) & 1)
            dl->AddRectFilled(ImVec2(a.x + x * c, a.y + y * c), ImVec2(a.x + (x + 1) * c, a.y + (y + 1) * c), IM_COL32(58, 58, 66, 255));
    }
}

// ---- "Export texture": a texture node's picture saved as a file in LauncherBridge\exported textures
static std::mutex g_meExMx; static std::vector<std::pair<std::string, bool>> g_meExMsgs;
static void MeExMsg(const std::string& t, bool err) { std::lock_guard<std::mutex> lk(g_meExMx); g_meExMsgs.push_back({ t, err }); g_framesLeft = 3; }
static std::wstring MeExportDir() {
    std::wstring d = DirOf(ModelsRoot()) + L"\\exported textures";
    SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    return d;
}
static std::wstring MeSafeName(std::string n) {
    for (auto& c : n) if (strchr("\\/:*?\"<>|", c) || (unsigned char)c < 32) c = '_';
    if (n.empty()) n = "texture";
    return U2W(n);
}
// a name that isn't taken yet (name.png, name (2).png, ...); returned without the extension
static std::wstring MeFreeName(const std::wstring& dir, const std::wstring& base, const std::wstring& ext) {
    std::wstring p = dir + L"\\" + base;
    for (int i = 2; FileExists(p + ext); i++) p = dir + L"\\" + base + L" (" + std::to_wstring(i) + L")";
    return p;
}
static void MeExportTexture(const std::string& value, const std::string& feeds) {
    if (value.empty()) { MeExMsg("That node has no texture to export.", true); return; }
    std::wstring dir = MeExportDir();
    if (value.rfind("file:", 0) == 0) {                                   // a picture file: copy it
        std::wstring src = U2W(value.substr(5));
        std::wstring fn = src.substr(src.find_last_of(L"\\/") + 1);
        size_t dot = fn.find_last_of(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"" : fn.substr(dot), base = dot == std::wstring::npos ? fn : fn.substr(0, dot);
        std::wstring dest = MeFreeName(dir, base, ext) + ext;
        if (CopyFileW(src.c_str(), dest.c_str(), TRUE)) MeExMsg("Exported texture: " + W2U(dest.substr(dir.size() + 1)), false);
        else MeExMsg("Couldn't copy " + W2U(src), true);
        return;
    }
    if (value.rfind("/Engine/Transient", 0) == 0) {                      // made in the game from a file (an imported model's own picture)
        std::wstring base = MeSafeName((M.model.empty() ? M.name : M.model) + (feeds.empty() ? "" : "_" + feeds));
        std::wstring dest = MeFreeName(dir, base, L".png");
        std::string d = W2U(dest);
        Ed("ed_texcopy", value + "\t" + d);
        return;
    }
    // one of the game's own textures: read it at full size from the game's files
    std::wstring dest = MeFreeName(dir, MeSafeName(MeShort(value)), L".png") + L".png";
    AesSyncNow();
    std::wstring paks = PaksDir();
    std::thread([paks, value, dest, dir] {
        int w = 0, h = 0; std::string err;
        if (!paks.empty() && GameTextureExport(paks, value, dest, w, h, err))
            MeExMsg("Exported texture: " + W2U(dest.substr(dir.size() + 1)) + "  (" + std::to_string(w) + " x " + std::to_string(h) + ")", false);
        else MeExMsg("Couldn't export " + MeShort(value) + (err.empty() ? "" : ": " + err), true);
    }).detach();
}
static std::string MeFeeds(int nodeId) {
    std::string f; for (auto& l : M.links) if (l.second == nodeId && M.pins[l.first].kind == MEK_TEX) f += (f.empty() ? "" : "_") + M.pins[l.first].name;
    return f;
}
static void MeOpenExportDir() { std::wstring d = MeExportDir(); ShellExecuteW(nullptr, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ---- opening / closing
static std::wstring g_meInst;                                  // the .uemi being edited (with its parent open as M.asset)
static void MeSaveAsset(); static void MeSaveInstance();
static void MatEdClose();
static void MatEdOpen(const std::string& actor) {
    if (M.open && !M.asset.empty()) MatEdClose();
    M.asset.clear(); M.dirty = false; g_meInst.clear(); g_meOverride.clear(); M.tab = 0; M.selSet.clear(); M.props = MeProps(); M.builtKey.clear();
    M.open = true; M.actor = actor; M.name.clear(); M.err.clear(); M.slots.clear(); M.cur = 0; M.builtFor = -1;
    M.nodes.clear(); M.links.clear(); M.pins.clear(); M.pend.clear(); M.prevPng.clear(); M.prevWanted = true; M.lastChange = 0;
    MeAskGraph();
    // a big window for the graph (put back as it was when the editor closes)
    if (g_edWnd && !M.resized) {
        GetWindowRect(g_edWnd, &M.oldRect);
        HMONITOR mon = MonitorFromWindow(g_edWnd, MONITOR_DEFAULTTONEAREST); MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(mon, &mi);
        RECT wa = mi.rcWork;
        int w = std::min<int>(wa.right - wa.left - 40, (int)Px(1400)), h = std::min<int>(wa.bottom - wa.top - 40, (int)Px(860));
        int x = std::max<int>(wa.left + 20, std::min<int>(M.oldRect.left, wa.right - w - 20)), y = std::max<int>(wa.top + 20, std::min<int>(M.oldRect.top, wa.bottom - h - 20));
        if (w > M.oldRect.right - M.oldRect.left || h > M.oldRect.bottom - M.oldRect.top) {
            SetWindowPos(g_edWnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            M.resized = true;
        }
    }
}
static void MatEdClose() {
    if (!M.asset.empty() && M.dirty) MeSaveAsset();               // (a material asset is saved when it's closed)
    if (!M.asset.empty() && !g_meInst.empty()) MeSaveInstance();
    if (M.asset.empty() && !M.builtKey.empty()) MeSaveGraphFile(M.builtKey, MeTakeCustom());
    M.asset.clear(); g_meInst.clear(); g_meOverride.clear(); M.dirty = false;
    M.open = false; M.picker = 0;
    for (auto& kv : M.pend) if (!kv.second.first.empty()) Ed("ed_matset", kv.second.first);
    M.pend.clear();
    if (M.resized && g_edWnd) {
        SetWindowPos(g_edWnd, nullptr, M.oldRect.left, M.oldRect.top, M.oldRect.right - M.oldRect.left, M.oldRect.bottom - M.oldRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        M.resized = false;
    }
}

// ---- pickers: a texture the game has loaded, or a material
static void MeAskList(int which) {
    std::wstring f = BridgeDir() + (which == 1 ? L"\\textures.txt" : L"\\materiallist.txt");
    DeleteFileW(f.c_str());
    M.list.clear(); M.listStamp = 0; M.listAsked = true;
    Ed(which == 1 ? "ed_texlist" : "ed_matlist");
}
static void MeOpenPicker(int which, int node) {
    M.picker = which; M.pickNode = node; M.filter[0] = 0;
    MeAskList(which);          // (the popup is opened from the editor's main window, see MatEdDraw)
}
static void MePickerPopup() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - Px(80), Px(760)), std::min(vp->Size.y - Px(80), Px(620))), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##mepick", nullptr, ImGuiWindowFlags_NoTitleBar)) { if (M.picker && !ImGui::IsPopupOpen("##mepick")) M.picker = 0; return; }
    bool tex = M.picker == 1;
    ImGui::PushFont(F_h2); ImGui::TextUnformatted(tex ? "Pick a texture" : "Pick a material"); ImGui::PopFont();
    EdLabel(tex ? "Every texture the game has loaded right now. Go near things in the game to load more."
                : "Every material the game has loaded right now. Go near things in the game to load more.");
    if (M.listAsked) {
        std::wstring f = BridgeDir() + (tex ? L"\\textures.txt" : L"\\materiallist.txt");
        if (MeFileStamp(f, M.listStamp)) {
            std::string d;
            if (MeReadAll(f, d)) {
                M.list.clear(); std::istringstream ss(d); std::string line;
                while (std::getline(ss, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); if (!line.empty()) M.list.push_back(line); }
                M.listAsked = false;
            }
        }
    }
    ImGui::SetNextItemWidth(-Px(110));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##mf", ICON_SEARCH "  Search by name", M.filter, sizeof M.filter);
    ImGui::SameLine();
    if (ImGui::Button(ICON_ROTATE " Refresh", ImVec2(-1, 0))) MeAskList(M.picker);
    std::string f = W2U(Lower(U2W(M.filter)));
    std::vector<int> shown;
    for (int i = 0; i < (int)M.list.size(); i++) {
        if (!f.empty()) { std::string l = W2U(Lower(U2W(M.list[i].substr(0, M.list[i].find('\t'))))); if (l.find(f) == std::string::npos) continue; }
        shown.push_back(i);
    }
    float footer = ImGui::GetFrameHeightWithSpacing() + Px(6);
    ImGui::BeginChild("##ml", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (M.listAsked && M.list.empty()) EdLabel("Asking the game...");
    else if (shown.empty()) EdLabel("Nothing matches.");
    float rowH = tex ? Px(52) : ImGui::GetTextLineHeightWithSpacing() + Px(4);
    ImGuiListClipper clip; clip.Begin((int)shown.size(), rowH);
    std::string chosen;
    while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
        auto t = MeTabs(M.list[shown[r]]);
        const std::string& path = t[0];
        ImGui::PushID(shown[r]);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH - Px(4)))) chosen = path;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float tx = p0.x + Px(4);
        if (tex) { MeTexBox(dl, path, ImVec2(p0.x + Px(4), p0.y + Px(2)), ImVec2(p0.x + Px(46), p0.y + Px(44)), Px(3)); tx = p0.x + Px(56); }
        dl->AddText(ImVec2(tx, p0.y + (tex ? Px(6) : Px(1))), ImGui::GetColorU32(ImGuiCol_Text), MeShort(path).c_str());
        if (tex) {
            ImGui::PushFont(F_small);
            std::string sub = path; if (t.size() >= 3 && t[1] != "0") sub += "   " + t[1] + " x " + t[2];
            dl->AddText(ImVec2(tx, p0.y + Px(26)), ImGui::GetColorU32(P.dim), sub.c_str());
            ImGui::PopFont();
        } else {
            ImGui::PushFont(F_small);
            float nx = tx + ImGui::CalcTextSize(MeShort(path).c_str()).x + Px(24);
            dl->AddText(ImVec2(nx, p0.y + Px(3)), ImGui::GetColorU32(P.dim), (path + (t.size() > 1 && t[1] == "m" ? "   (base material)" : "")).c_str());
            ImGui::PopFont();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PushFont(F_small); EdLabel((std::to_string(shown.size()) + " shown").c_str()); ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Px(90));
    if (ImGui::Button("Cancel", ImVec2(Px(90), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { M.picker = 0; ImGui::CloseCurrentPopup(); }
    if (!chosen.empty()) {
        if (MeNode* n = MeFind(M.pickNode)) { n->value = chosen; MeNodeChanged(*n, true); }
        M.picker = 0; ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
static void MePickFile(int node) {
    OvBrowse(L"Pictures (*.png, *.jpg, *.tga, *.bmp)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0", L"Pick a picture for the texture", ImpStartDir(), false, L"", nullptr, [node](std::wstring f) {
        if (f.empty()) return;
        if (MeNode* n = MeFind(node)) { n->value = "file:" + W2U(f); MeNodeChanged(*n, true); }
    });
}

// ---- drawing the graph
static int g_meWireToNew = -1, g_meWireToNewOut = 0;      // a wire let go on empty graph: wired into the node picked next
static void MeApplyTexSettings(int nodeId);
static float MeZ(float v) { return v * M.zoom * S(); }
static ImVec2 MeToScreen(ImVec2 org, float x, float y) { return ImVec2(org.x + M.panX + MeZ(x), org.y + M.panY + MeZ(y)); }
static ImVec2 MeToGraph(ImVec2 org, ImVec2 m) { return ImVec2((m.x - org.x - M.panX) / (M.zoom * S()), (m.y - org.y - M.panY) / (M.zoom * S())); }
static const float ME_HEAD = 30.f, ME_ROW = 22.f, ME_STDGAP = 26.f;
static float MeNodeW(const MeNode& n) {
    if (n.kind == MEK_COMMENT) return n.w;
    return n.output ? 240.f : n.kind == MEK_TEX ? 210.f : n.kind == MEK_MAT ? 230.f : n.kind == MEK_OP && n.op == OP_LAYERBLEND ? 190.f : 170.f;
}
static int MeRows(const MeNode& n) { return (int)std::max(MeNodeIns(n).size(), MeNodeOuts(n).size()); }
static bool MeHasPreview(const MeNode& n) { return g_meNodePreviews && n.kind == MEK_OP; }
static float MeExtra(const MeNode& n) {      // the node's own content below its pins
    float e = 4.f;
    if (n.kind == MEK_TEX) e = 92.f;
    else if (n.kind == MEK_VEC || n.kind == MEK_MAT) e = 34.f;
    else if (n.kind == MEK_SCA) e = 30.f;
    else if (n.kind == MEK_OP && (n.op == OP_TEXCOORD || n.op == OP_POW || n.op == OP_CLAMP || n.op == OP_DESAT || n.op == OP_PANNER || n.op == OP_ROTATOR || n.op == OP_FRESNEL || n.op == OP_SIN || n.op == OP_COS)) e = 24.f;
    if (MeHasPreview(n)) e += 72.f;
    return e;
}
static float MeNodeH(const MeNode& n) {
    if (n.kind == MEK_COMMENT) return n.h;
    if (n.output) return 30.f + 24.f * std::max<int>(1, (int)M.pins.size()) + 8.f + (M.stdStart < (int)M.pins.size() && M.stdStart > 0 ? ME_STDGAP : 0);
    return ME_HEAD + ME_ROW * MeRows(n) + MeExtra(n) + 6.f;
}
static ImVec2 MeOutPin(ImVec2 org, const MeNode& n, int k = 0) { return MeToScreen(org, n.x + MeNodeW(n), n.y + ME_HEAD + ME_ROW * k + ME_ROW / 2 + 3); }
static ImVec2 MeNodeInPin(ImVec2 org, const MeNode& n, int k) { return MeToScreen(org, n.x, n.y + ME_HEAD + ME_ROW * k + ME_ROW / 2 + 3); }
static ImVec2 MeInPin(ImVec2 org, const MeNode& out, int i) { return MeToScreen(org, out.x, out.y + 30.f + 24.f * i + 12.f + (i >= M.stdStart && M.stdStart > 0 ? ME_STDGAP : 0)); }
static ImU32 MeOutCol(const MeNode& n, int k, float a = 1) {
    if ((n.kind == MEK_TEX || n.kind == MEK_VEC) && k >= 1) { ImU32 c[4] = { IM_COL32(235, 70, 60, 255), IM_COL32(80, 220, 90, 255), IM_COL32(70, 120, 250, 255), IM_COL32(170, 170, 176, 255) }; ImVec4 v = ImGui::ColorConvertU32ToFloat4(c[k - 1]); v.w = a; return ImGui::GetColorU32(v); }
    if (n.kind == MEK_OP) return ImGui::GetColorU32(ImVec4(0.92f, 0.92f, 0.94f, a));
    return MeKindCol(n.kind, a);
}
static void MeBezier(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float th) {
    float dx = std::max(MeZ(40), std::fabs(b.x - a.x) * 0.5f);
    dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, col, th);
}
static std::string MeNodeTitle(const MeNode& n) {
    if (n.output) return !M.asset.empty() ? W2U(M.asset.substr(M.asset.find_last_of(L"\\/") + 1)) : M.cur < (int)M.slots.size() ? MeShort(M.slots[M.cur].mat) : "Material";
    if (n.kind == MEK_COMMENT) return n.name;
    if (n.kind == MEK_OP) return g_meOpName[n.op];
    if (MeIsParam(n)) return std::string(n.kind == MEK_TEX ? "Texture Param" : n.kind == MEK_VEC ? "Vector Param" : "Scalar Param") + "  \xC2\xB7  " + n.name;
    std::string fed;        // the setting(s) it feeds, like Unreal's parameter nodes show their name
    for (auto& l : M.links) if (l.second == n.id && !MeIsStd(l.first)) { fed += (fed.empty() ? "" : ", ") + M.pins[l.first].name; }
    std::string t = n.kind == MEK_TEX ? "Texture Sample" : n.kind == MEK_VEC ? (fed.empty() ? "Constant Vector" : "Colour") : n.kind == MEK_SCA ? (fed.empty() ? "Constant" : "Number") : "Material";
    if (n.kind == MEK_MAT) return t;
    return fed.empty() ? t : t + "  \xC2\xB7  " + fed;
}
static bool MeOutUsed(const MeNode& n, int k) {
    for (auto& l : M.links) if (l.second == n.id && (M.linkOut.count(l.first) ? M.linkOut.at(l.first) : 0) == k) return true;
    for (auto& kv : M.nin) if (kv.second.first == n.id && kv.second.second == k) return true;
    return false;
}
static std::string MeNodeError(int id) {
    for (auto& e : g_ms.src.errs) if (e.first == id) return e.second;
    return "";
}
static void MeDrawComment(ImDrawList* dl, ImVec2 org, const MeNode& n, ImFont* font, float fs) {
    ImVec2 a = MeToScreen(org, n.x, n.y), b = MeToScreen(org, n.x + n.w, n.y + n.h);
    ImVec4 c = Hex(n.col ? n.col : g_meCommentCols[0]);
    ImU32 body = ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.22f)), head = ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.85f));
    float r = MeZ(4), hh = MeZ(30);
    dl->AddRectFilled(a, b, body, r);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + hh), head, r, ImDrawFlags_RoundCornersTop);
    dl->AddRect(a, b, MeSelected(n.id) ? IM_COL32(240, 170, 40, 255) : ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.9f)), r, 0, MeSelected(n.id) ? MeZ(2.5f) : MeZ(1));
    dl->PushClipRect(a, ImVec2(b.x - MeZ(6), a.y + hh), true);
    dl->AddText(font, fs * 1.1f, ImVec2(a.x + MeZ(10), a.y + (hh - fs * 1.1f) / 2), IM_COL32(245, 245, 248, 255), n.name.c_str());
    dl->PopClipRect();
    // resize handle
    float g = MeZ(12);
    dl->AddTriangleFilled(ImVec2(b.x - g, b.y - MeZ(2)), ImVec2(b.x - MeZ(2), b.y - g), ImVec2(b.x - MeZ(2), b.y - MeZ(2)), ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.9f)));
}
static void MeDrawNode(ImDrawList* dl, ImVec2 org, const MeNode& n, ImFont* font, float fs) {
    if (n.kind == MEK_COMMENT) return;
    ImVec2 a = MeToScreen(org, n.x, n.y), b = MeToScreen(org, n.x + MeNodeW(n), n.y + MeNodeH(n));
    float r = MeZ(6), head = MeZ(30);
    bool sel = MeSelected(n.id), bad = !MeNodeError(n.id).empty();
    dl->AddRectFilled(ImVec2(a.x + MeZ(3), a.y + MeZ(4)), ImVec2(b.x + MeZ(3), b.y + MeZ(4)), IM_COL32(0, 0, 0, 90), r);   // shadow
    dl->AddRectFilled(a, b, IM_COL32(30, 31, 36, 245), r);
    ImU32 hc = n.output ? IM_COL32(84, 74, 64, 255) : MeIsParam(n) ? IM_COL32(40, 104, 64, 255) : n.kind == MEK_TEX ? IM_COL32(34, 82, 128, 255) : n.kind == MEK_VEC ? IM_COL32(110, 92, 34, 255)
             : n.kind == MEK_SCA ? IM_COL32(48, 96, 52, 255) : n.kind == MEK_OP ? (n.op == OP_LAYERBLEND ? IM_COL32(74, 92, 54, 255) : IM_COL32(58, 74, 66, 255)) : IM_COL32(86, 60, 110, 255);
    if (bad) hc = IM_COL32(150, 40, 36, 255);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + head), hc, r, ImDrawFlags_RoundCornersTop);
    dl->AddRect(a, b, sel ? IM_COL32(240, 170, 40, 255) : bad ? IM_COL32(230, 70, 60, 255) : IM_COL32(12, 12, 14, 255), r, 0, sel || bad ? MeZ(2.5f) : MeZ(1));
    std::string title = MeNodeTitle(n);
    dl->PushClipRect(a, ImVec2(b.x - MeZ(6), a.y + head), true);
    dl->AddText(font, fs, ImVec2(a.x + MeZ(10), a.y + (head - fs) / 2), IM_COL32(235, 235, 240, 255), title.c_str());
    dl->PopClipRect();
    float small = fs * 0.86f;
    if (n.output) {
        for (int i = 0; i < (int)M.pins.size(); i++) {
            const MeParam& p = M.pins[i];
            ImVec2 pp = MeInPin(org, n, i);
            if (i == M.stdStart && M.stdStart > 0) {      // Unreal's inputs, after the material's own settings
                float ly = pp.y - MeZ(24) / 2 - MeZ(ME_STDGAP) / 2;
                dl->AddLine(ImVec2(a.x + MeZ(8), ly), ImVec2(b.x - MeZ(8), ly), IM_COL32(70, 70, 78, 255), MeZ(1));
                dl->AddText(font, small * 0.85f, ImVec2(a.x + MeZ(10), ly + MeZ(3)), IM_COL32(150, 150, 158, 255), "Unreal inputs (baked for the game)");
            }
            bool fed = M.links.count(i) > 0;
            // inputs the material's blend mode / shading doesn't use are greyed, like Unreal does
            bool unused = false;
            if (MeIsStd(i)) {
                int k = i - M.stdStart;
                if (k == STD_OPAC && M.props.blend != BLEND_TRANSLUCENT && M.props.blend != BLEND_ADDITIVE) unused = true;
                if (k == STD_OPMASK && M.props.blend != BLEND_MASKED) unused = true;
                if (M.props.unlit && k != STD_EMIS && k != STD_OPAC && k != STD_OPMASK && k != STD_WPO && k != STD_DISP) unused = true;
            }
            ImU32 pc = p.kind == MEK_STD ? (unused ? IM_COL32(110, 110, 116, 255) : IM_COL32(235, 235, 240, 255)) : MeKindCol(p.kind);
            if (fed) dl->AddCircleFilled(pp, MeZ(5), pc);
            else dl->AddCircle(pp, MeZ(5), pc, 0, MeZ(1.6f));
            std::string label = p.kind == MEK_MAT ? "Material" : p.name;
            dl->PushClipRect(ImVec2(a.x, pp.y - fs), ImVec2(b.x - MeZ(6), pp.y + fs), true);
            dl->AddText(font, small, ImVec2(pp.x + MeZ(12), pp.y - small / 2), unused ? IM_COL32(110, 110, 116, 255) : fed ? IM_COL32(225, 225, 230, 255) : IM_COL32(150, 150, 158, 255), label.c_str());
            dl->PopClipRect();
        }
        return;
    }
    // pins: inputs on the left, outputs on the right
    auto ins = MeNodeIns(n); auto outs = MeNodeOuts(n);
    for (int k = 0; k < (int)ins.size(); k++) {
        ImVec2 pp = MeNodeInPin(org, n, k);
        bool fed = M.nin.count(MeInKey(n.id, k)) > 0;
        if (fed) dl->AddCircleFilled(pp, MeZ(4.5f), IM_COL32(235, 235, 240, 255)); else dl->AddCircle(pp, MeZ(4.5f), IM_COL32(200, 200, 206, 255), 0, MeZ(1.5f));
        dl->AddText(font, small, ImVec2(pp.x + MeZ(10), pp.y - small / 2), fed ? IM_COL32(225, 225, 230, 255) : IM_COL32(150, 150, 158, 255), ins[k].c_str());
    }
    for (int k = 0; k < (int)outs.size(); k++) {
        ImVec2 op = MeOutPin(org, n, k);
        ImU32 pc = MeOutCol(n, k);
        if (MeOutUsed(n, k)) dl->AddCircleFilled(op, MeZ(5), pc); else dl->AddCircle(op, MeZ(5), pc, 0, MeZ(1.6f));
        ImVec2 ts = font->CalcTextSizeA(small, FLT_MAX, 0, outs[k].c_str());
        dl->AddText(font, small, ImVec2(op.x - MeZ(11) - ts.x, op.y - small / 2), IM_COL32(210, 210, 216, 255), outs[k].c_str());
    }
    float bx = a.x + MeZ(10), by = a.y + head + MeZ(ME_ROW * MeRows(n)) + MeZ(4);
    dl->PushClipRect(ImVec2(a.x, a.y + head), b, true);
    std::string val = MeParamValue(n);
    if (n.kind == MEK_TEX) {
        float s = MeZ(80);
        MeTexBox(dl, val, ImVec2(bx, by), ImVec2(bx + s, by + s), MeZ(3));
        std::string nm = val.rfind("file:", 0) == 0 ? MeFileName(val) : val.empty() ? "(none)" : MeShort(val);
        dl->AddText(font, small * 0.92f, ImVec2(bx + s + MeZ(8), by + MeZ(30)), IM_COL32(170, 170, 178, 255), nm.c_str(), nullptr, MeZ(MeNodeW(n) - 108));
        if (n.sampler) dl->AddText(font, small * 0.8f, ImVec2(bx + s + MeZ(8), by + MeZ(10)), IM_COL32(130, 130, 140, 255), n.sampler == 1 ? "normal map" : "plain numbers");
        if (n.tile[0] != 1 || n.tile[1] != 1 || n.rot != 0) { char t[64]; snprintf(t, sizeof t, "x%g,%g  %g\xC2\xB0", n.tile[0], n.tile[1], n.rot); dl->AddText(font, small * 0.8f, ImVec2(bx + s + MeZ(8), by + MeZ(56)), IM_COL32(130, 130, 140, 255), t); }
    } else if (n.kind == MEK_VEC) {
        float c[4]; MeColour(val, c);
        ImU32 col = ImGui::GetColorU32(ImVec4(std::min(1.f, c[0]), std::min(1.f, c[1]), std::min(1.f, c[2]), 1));
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + MeZ(36), by + MeZ(20)), col, MeZ(3));
        char t[64]; snprintf(t, sizeof t, "%.2f %.2f %.2f", c[0], c[1], c[2]);
        dl->AddText(font, small * 0.92f, ImVec2(bx + MeZ(44), by + MeZ(2)), IM_COL32(170, 170, 178, 255), t);
    } else if (n.kind == MEK_SCA) {
        char t[32]; snprintf(t, sizeof t, "%.3f", atof(val.c_str()));
        dl->AddText(font, fs, ImVec2(bx, by), IM_COL32(220, 220, 226, 255), t);
    } else if (n.kind == MEK_MAT) {
        dl->AddText(font, small, ImVec2(bx, by), IM_COL32(200, 200, 208, 255), MeShort(n.value).c_str());
    } else if (n.kind == MEK_OP) {
        char t[64] = "";
        if (n.op == OP_TEXCOORD) snprintf(t, sizeof t, "Tiling %g x %g", n.c[0], n.c[1]);
        else if (n.op == OP_POW && !M.nin.count(MeInKey(n.id, 1))) snprintf(t, sizeof t, "Exp %g", n.c[1]);
        else if (n.op == OP_CLAMP) snprintf(t, sizeof t, "%g .. %g", n.c[1], n.c[2]);
        else if (n.op == OP_DESAT && !M.nin.count(MeInKey(n.id, 1))) snprintf(t, sizeof t, "Fraction %g", n.c[1]);
        else if (n.op == OP_PANNER) snprintf(t, sizeof t, "Speed %g, %g", n.c[0], n.c[1]);
        else if (n.op == OP_ROTATOR) snprintf(t, sizeof t, "Speed %g", n.c[2]);
        else if (n.op == OP_FRESNEL) snprintf(t, sizeof t, "Exp %g  Base %g", n.c[0], n.c[1]);
        else if (n.op == OP_SIN || n.op == OP_COS) snprintf(t, sizeof t, "Period %g", n.c[0]);
        if (t[0]) { dl->AddText(font, small * 0.92f, ImVec2(bx, by), IM_COL32(160, 160, 168, 255), t); by += MeZ(24); }
        if (MeHasPreview(n)) {
            float s = MeZ(64);
            if (ID3D11ShaderResourceView* pv = MePreviewOf(n.id)) dl->AddImageRounded((ImTextureID)pv, ImVec2(bx, by + MeZ(2)), ImVec2(bx + s, by + MeZ(2) + s), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, MeZ(3));
            else dl->AddRectFilled(ImVec2(bx, by + MeZ(2)), ImVec2(bx + s, by + MeZ(2) + s), IM_COL32(40, 40, 46, 255), MeZ(3));
        }
    }
    dl->PopClipRect();
}

// which node / pin is under the mouse
static int MeHitNode(ImVec2 org, ImVec2 m) {
    for (int i = (int)M.nodes.size() - 1; i >= 0; i--) {
        const MeNode& n = M.nodes[i]; if (n.kind == MEK_COMMENT) continue;
        ImVec2 a = MeToScreen(org, n.x, n.y), b = MeToScreen(org, n.x + MeNodeW(n), n.y + MeNodeH(n));
        if (m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y) return n.id;
    }
    // comments: by their title bar
    for (int i = (int)M.nodes.size() - 1; i >= 0; i--) {
        const MeNode& n = M.nodes[i]; if (n.kind != MEK_COMMENT) continue;
        ImVec2 a = MeToScreen(org, n.x, n.y), b = MeToScreen(org, n.x + n.w, n.y + 30);
        if (m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y) return n.id;
    }
    return -1;
}
static int MeHitCommentCorner(ImVec2 org, ImVec2 m) {
    for (int i = (int)M.nodes.size() - 1; i >= 0; i--) {
        const MeNode& n = M.nodes[i]; if (n.kind != MEK_COMMENT) continue;
        ImVec2 b = MeToScreen(org, n.x + n.w, n.y + n.h);
        if (m.x <= b.x && m.y <= b.y && m.x > b.x - MeZ(16) && m.y > b.y - MeZ(16)) return n.id;
    }
    return -1;
}
// pins are easy to grab: the dot and its label, and never smaller than a few pixels however far you zoom out
static float MePinR() { return std::max(MeZ(11), Px(8)); }
static int MeHitOutPin(ImVec2 org, ImVec2 m, int* outIdx = nullptr) {
    for (int k = (int)M.nodes.size() - 1; k >= 0; k--) {
        const MeNode& n = M.nodes[k]; if (n.output || n.kind == MEK_COMMENT) continue;
        int no = (int)MeNodeOuts(n).size();
        for (int o = 0; o < no; o++) {
            ImVec2 p = MeOutPin(org, n, o);
            bool hit = Vlen(Vsub(p, m)) < MePinR() || (std::fabs(m.y - p.y) < std::max(MeZ(ME_ROW / 2), Px(5)) && m.x > p.x - MeZ(50) && m.x <= p.x + MePinR() * 0.6f);
            if (hit) { if (outIdx) *outIdx = o; return n.id; }
        }
    }
    return -1;
}
// a node's input pin under the mouse (maths, layer blend, a texture's UVs)
static int MeHitNodeIn(ImVec2 org, ImVec2 m, int* inIdx) {
    for (int k = (int)M.nodes.size() - 1; k >= 0; k--) {
        const MeNode& n = M.nodes[k]; if (n.output || n.kind == MEK_COMMENT) continue;
        int ni = (int)MeNodeIns(n).size();
        for (int i = 0; i < ni; i++) {
            ImVec2 p = MeNodeInPin(org, n, i);
            bool hit = Vlen(Vsub(p, m)) < MePinR() || (std::fabs(m.y - p.y) < std::max(MeZ(ME_ROW / 2), Px(5)) && m.x >= p.x - MePinR() * 0.6f && m.x < p.x + MeZ(60));
            if (hit) { *inIdx = i; return n.id; }
        }
    }
    return -1;
}
static int MeHitInPin(ImVec2 org, ImVec2 m) {
    MeNode* out = MeOutput(); if (!out) return -1;
    float row = MeZ(24);
    for (int i = 0; i < (int)M.pins.size(); i++) {
        ImVec2 p = MeInPin(org, *out, i);
        if (std::fabs(p.y - m.y) < std::max(row / 2, Px(5)) && m.x > p.x - MePinR() && m.x < p.x + MeZ(MeNodeW(*out))) return i;
    }
    return -1;
}
static void MeFit(ImVec2 size) {
    if (M.nodes.empty()) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (auto& n : M.nodes) { x0 = std::min(x0, n.x); y0 = std::min(y0, n.y); x1 = std::max(x1, n.x + MeNodeW(n)); y1 = std::max(y1, n.y + MeNodeH(n)); }
    float gw = x1 - x0 + 80, gh = y1 - y0 + 80;
    M.zoom = std::clamp(std::min(size.x / (gw * S()), size.y / (gh * S())), 0.35f, 1.25f);
    M.panX = size.x / 2 - MeZ((x0 + x1) / 2); M.panY = size.y / 2 - MeZ((y0 + y1) / 2);
    if (MeZ(gh) > size.y) M.panY = MeZ(40) - MeZ(y0);   // tall graphs: start at the top
    M.fitted = true;
}
// Nodes in columns to the left of the output (in the order of its inputs, so wires don't cross much); a column
// is about as tall as the output node, so the whole graph stays wide rather than very tall.
static void MeArrange() {
    MeNode* out = MeOutput(); if (!out) return;
    out->x = 0; out->y = 0;
    float colH = std::max(MeNodeH(*out), 460.f), top = -20, y = top, right = -70, colW = 0;
    std::set<int> done; std::vector<MeNode*> order;
    for (int i = 0; i < (int)M.pins.size(); i++) {
        auto it = M.links.find(i); if (it == M.links.end() || done.count(it->second)) continue;
        if (MeNode* n = MeFind(it->second)) { order.push_back(n); done.insert(n->id); }
    }
    for (auto& n : M.nodes) if (!n.output && n.kind != MEK_COMMENT && !done.count(n.id)) order.push_back(&n);   // unplugged ones last
    for (MeNode* n : order) {
        float h = MeNodeH(*n);
        if (y > top && y + h > top + colH) { right -= colW + 40; colW = 0; y = top; }
        n->x = right - MeNodeW(*n); n->y = y;
        y += h + 16; colW = std::max(colW, MeNodeW(*n));
    }
    M.fitted = false;
}
static void MeAutoArrange() {
    // comments keep the nodes they had around them: remember which, then fit them around those again
    std::map<int, std::vector<int>> inside;
    for (auto& c : M.nodes) if (c.kind == MEK_COMMENT)
        for (auto& n : M.nodes) if (n.kind != MEK_COMMENT && !n.output && n.x >= c.x && n.y >= c.y && n.x + MeNodeW(n) <= c.x + c.w && n.y + MeNodeH(n) <= c.y + c.h) inside[c.id].push_back(n.id);
    // a graph with your own nodes wired together: arrange by depth; otherwise the compact columns
    bool deep = false; for (auto& kv : M.nin) { (void)kv; deep = true; break; }
    if (deep) MeArrangeLayered(); else MeArrange();
    float lowest = 0; for (auto& n : M.nodes) if (n.kind != MEK_COMMENT) lowest = std::max(lowest, n.y + MeNodeH(n));
    for (auto& c : M.nodes) if (c.kind == MEK_COMMENT) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int id : inside[c.id]) if (MeNode* n = MeFind(id)) { x0 = std::min(x0, n->x); y0 = std::min(y0, n->y); x1 = std::max(x1, n->x + MeNodeW(*n)); y1 = std::max(y1, n->y + MeNodeH(*n)); }
        if (x1 > x0) { c.x = x0 - 30; c.y = y0 - 60; c.w = x1 - x0 + 60; c.h = y1 - y0 + 90; }
        else { c.x = -400; c.y = lowest + 60; lowest += c.h + 30; }          // (an empty comment: out of the way, below)
    }
}

static void MeUseItsTextures();
// the add-node menu (right-click on the graph): searchable, like Unreal's
static void MeAddMenu(float gx, float gy) {
    static char f[64] = "";
    if (ImGui::IsWindowAppearing()) { f[0] = 0; ImGui::SetKeyboardFocusHere(); }
    ImGui::SetNextItemWidth(Px(260));
    ImGui::InputTextWithHint("##addf", ICON_SEARCH "  Search", f, sizeof f);
    std::string fl = W2U(Lower(U2W(f)));
    ImGui::BeginChild("##addl", ImVec2(Px(300), Px(360)));
    int first = -1;
    for (auto& cat : g_mePalCats) {
        if (cat.from == 40 && M.slots.empty()) continue;
        bool any = false; for (int i = cat.from; i < cat.to; i++) if (MePalMatches(i, fl)) { any = true; if (first < 0) first = i; }
        if (!any) continue;
        ImGui::PushFont(F_small); EdLabel(cat.name); ImGui::PopFont();
        for (int i = cat.from; i < cat.to; i++) if (MePalMatches(i, fl)) {
            if (ImGui::Selectable((std::string("   ") + g_mePal[i].label).c_str())) { MeAddFromPalette(i, gx, gy); ImGui::CloseCurrentPopup(); }
            if (ImGui::IsItemHovered() && *MePalHelp(g_mePal[i])) ImGui::SetTooltip("%s", MePalHelp(g_mePal[i]));
        }
    }
    if (!fl.empty() && first >= 0 && ImGui::IsKeyPressed(ImGuiKey_Enter)) { MeAddFromPalette(first, gx, gy); ImGui::CloseCurrentPopup(); }
    if (!M.slots.empty()) {
        ImGui::Separator();
        if (ImGui::Selectable(ICON_WAND "  Start from this material's textures")) { MeUseItsTextures(); ImGui::CloseCurrentPopup(); }
    }
    if (const char* clip = ImGui::GetClipboardText()) if (strncmp(clip, "UEGRAPHCLIP", 11) == 0) {
        ImGui::Separator();
        if (ImGui::Selectable(ICON_PASTE "  Paste here")) { MePasteText(clip, gx, gy); ImGui::CloseCurrentPopup(); }
    }
    ImGui::EndChild();
}

static void MeCanvas() {
    ImVec2 org = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    if (size.x < 50 || size.y < 50) return;
    if (!M.fitted) MeFit(size);
    MePreviewsUpdate();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end(org.x + size.x, org.y + size.y);
    dl->PushClipRect(org, end, true);
    dl->AddRectFilled(org, end, IM_COL32(24, 24, 27, 255));
    // grid
    float g = MeZ(16); int k = 0;
    if (g > 4) {
        float sx = fmodf(M.panX, g), sy = fmodf(M.panY, g);
        int baseX = (int)floorf(M.panX / g), baseY = (int)floorf(M.panY / g);
        k = 0; for (float x = org.x + sx; x < end.x; x += g, k++) dl->AddLine(ImVec2(x, org.y), ImVec2(x, end.y), ((k - baseX) % 8 == 0) ? IM_COL32(12, 12, 14, 255) : IM_COL32(34, 34, 38, 255));
        k = 0; for (float y = org.y + sy; y < end.y; y += g, k++) dl->AddLine(ImVec2(org.x, y), ImVec2(end.x, y), ((k - baseY) % 8 == 0) ? IM_COL32(12, 12, 14, 255) : IM_COL32(34, 34, 38, 255));
    }
    // breadcrumb
    {
        std::string bc = !M.asset.empty() ? "Material Library  >  " + W2U(M.asset.substr(M.asset.find_last_of(L"\\/") + 1)) + (M.dirty ? " *" : "") + "  >  Material Graph"
                                          : M.name + "  >  " + (M.cur < (int)M.slots.size() ? MeShort(M.slots[M.cur].mat) : "") + "  >  Material Graph";
        ImGui::PushFont(F_med);
        dl->AddText(ImVec2(org.x + Px(14), org.y + Px(10)), IM_COL32(200, 200, 206, 255), bc.c_str());
        ImGui::PopFont();
    }
    // interaction surface
    ImGui::SetCursorScreenPos(org);
    ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    bool hov = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 m = io.MousePos;
    // nodes dropped in from the palette (or the texture library)
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MENODE")) { ImVec2 gp = MeToGraph(org, m); MeAddFromPalette(*(const int*)pl->Data, gp.x, gp.y); }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("METEX")) {
            ImVec2 gp = MeToGraph(org, m); std::string v((const char*)pl->Data, pl->DataSize);
            int id = MeAddNode(MEK_TEX, gp.x, gp.y, v); MeSelectOnly(id); MeApplyTexSettings(id); MeStdChanged();
        }
        ImGui::EndDragDropTarget();
    }
    if (M.dragNew.rfind("pal:", 0) == 0) {            // double-clicked in the palette: in the middle of the view
        int i = atoi(M.dragNew.c_str() + 4); ImVec2 gp = MeToGraph(org, ImVec2(org.x + size.x / 2, org.y + size.y / 2));
        MeAddFromPalette(i, gp.x - 80, gp.y - 40); M.dragNew.clear();
    } else if (M.dragNew.rfind("tex:", 0) == 0) {
        ImVec2 gp = MeToGraph(org, ImVec2(org.x + size.x / 2, org.y + size.y / 2));
        int id = MeAddNode(MEK_TEX, gp.x - 100, gp.y - 60, M.dragNew.substr(4)); MeSelectOnly(id); MeApplyTexSettings(id); MeStdChanged(); M.dragNew.clear();
    }
    // zoom about the mouse
    if (hov && io.MouseWheel != 0) {
        float old = M.zoom;
        M.zoom = std::clamp(M.zoom * (io.MouseWheel > 0 ? 1.12f : 1 / 1.12f), 0.2f, 2.5f);
        float gx = (m.x - org.x - M.panX) / (old * S()), gy = (m.y - org.y - M.panY) / (old * S());
        M.panX = m.x - org.x - gx * M.zoom * S(); M.panY = m.y - org.y - gy * M.zoom * S();
    }
    // pan: right or middle drag
    if (hov && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) { M.panning = true; M.rclickAt = m; M.rmoved = false; }
    if (M.panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            M.panX += io.MouseDelta.x; M.panY += io.MouseDelta.y;
            if (Vlen(Vsub(m, M.rclickAt)) > Px(4)) M.rmoved = true;
        } else {
            M.panning = false;
            if (!M.rmoved && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                int hit = MeHitNode(org, m);
                if (hit >= 0) { if (!MeSelected(hit)) MeSelectOnly(hit); else M.sel = hit; ImGui::OpenPopup("##menode"); }
                else { M.rclickAt = m; ImGui::OpenPopup("##meadd"); }
            }
        }
    }
    // left: pins, nodes, a box to select with
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int oo = 0, ii = 0;
        int op = MeHitOutPin(org, m, &oo), ip = MeHitInPin(org, m), ni = MeHitNodeIn(org, m, &ii);
        if (io.KeyAlt && (ip >= 0 || ni >= 0)) {             // Alt+click a pin: break its wire (like Unreal)
            if (ip >= 0) MeUnlink(ip); else MeUnlinkIn(ni, ii);
        } else if (io.KeyAlt && op >= 0) {
            std::vector<int> pins; for (auto& l : M.links) if (l.second == op && (M.linkOut.count(l.first) ? M.linkOut[l.first] : 0) == oo) pins.push_back(l.first);
            for (int p : pins) MeUnlink(p);
            std::vector<long long> ins; for (auto& kv : M.nin) if (kv.second.first == op && kv.second.second == oo) ins.push_back(kv.first);
            for (auto key : ins) MeUnlinkIn((int)(key / 64), (int)(key % 64));
        } else if (op >= 0) { M.linkFrom = op; g_meLinkOut = oo; }
        else if (ip >= 0 && M.links.count(ip)) {        // pick up an existing wire by its end
            M.linkFrom = M.links[ip]; g_meLinkOut = M.linkOut.count(ip) ? M.linkOut[ip] : 0; MeUnlink(ip);
        } else if (ni >= 0 && M.nin.count(MeInKey(ni, ii))) {
            auto src = M.nin[MeInKey(ni, ii)]; M.linkFrom = src.first; g_meLinkOut = src.second; MeUnlinkIn(ni, ii);
        } else if (int corner = MeHitCommentCorner(org, m); corner >= 0) {
            M.resizing = corner; MeSelectOnly(corner);
        } else {
            int hit = MeHitNode(org, m);
            if (hit >= 0) {
                if (io.KeyCtrl) { if (M.selSet.count(hit)) { M.selSet.erase(hit); if (M.sel == hit) M.sel = M.selSet.empty() ? -1 : *M.selSet.begin(); } else { M.selSet.insert(hit); M.sel = hit; } }
                else if (io.KeyShift) { M.selSet.insert(hit); M.sel = hit; }
                else if (!MeSelected(hit)) MeSelectOnly(hit);
                else M.sel = hit;
                // drag everything selected (and what's inside a selected comment)
                M.dragGroup.clear(); M.dragMoved = false;
                std::set<int> moving; for (int id : MeSelection()) moving.insert(id);
                for (int id : std::vector<int>(moving.begin(), moving.end())) if (MeNode* c = MeFind(id)) if (c->kind == MEK_COMMENT)
                    for (auto& n : M.nodes) if (n.id != c->id && n.x >= c->x && n.y >= c->y && n.x + MeNodeW(n) <= c->x + c->w && n.y + MeNodeH(n) <= c->y + c->h) moving.insert(n.id);
                ImVec2 gp = MeToGraph(org, m);
                for (int id : moving) if (MeNode* n = MeFind(id)) M.dragGroup.push_back({ id, ImVec2(n->x - gp.x, n->y - gp.y) });
                M.drag.node = hit;
                // bring it to the front (comments stay at the back)
                auto it = std::find_if(M.nodes.begin(), M.nodes.end(), [&](const MeNode& x) { return x.id == hit; });
                if (it != M.nodes.end() && it->kind != MEK_COMMENT) { MeNode keep = *it; M.nodes.erase(it); M.nodes.push_back(keep); }
            } else {
                if (!io.KeyCtrl && !io.KeyShift) { M.selSet.clear(); M.sel = -1; }
                M.boxing = true; M.boxFrom = m;
            }
        }
    }
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        int hit = MeHitNode(org, m);
        if (MeNode* n = MeFind(hit)) {
            if (n->kind == MEK_TEX && !MeIsParam(*n) && M.slots.empty()) MePickFile(n->id);
            else if (n->kind == MEK_TEX) MeOpenPicker(1, n->id);
            else if (n->kind == MEK_MAT) MeOpenPicker(2, n->id);
        }
    }
    if (M.drag.node >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImVec2 gp = MeToGraph(org, m);
            if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) M.dragMoved = true;
            for (auto& d : M.dragGroup) if (MeNode* n = MeFind(d.first)) {
                n->x = gp.x + d.second.x; n->y = gp.y + d.second.y;
                if (io.KeyCtrl) { n->x = roundf(n->x / 16) * 16; n->y = roundf(n->y / 16) * 16; }   // Ctrl: snap to the grid
            }
        } else { M.drag.node = -1; M.dragGroup.clear(); }
    }
    if (M.resizing >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (MeNode* c = MeFind(M.resizing)) { ImVec2 gp = MeToGraph(org, m); c->w = std::max(160.f, gp.x - c->x); c->h = std::max(90.f, gp.y - c->y); }
        } else M.resizing = -1;
    }
    if (M.boxing) {
        ImVec2 a(std::min(M.boxFrom.x, m.x), std::min(M.boxFrom.y, m.y)), b(std::max(M.boxFrom.x, m.x), std::max(M.boxFrom.y, m.y));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            M.boxing = false;
            if (b.x - a.x > Px(4) || b.y - a.y > Px(4)) {
                for (auto& n : M.nodes) {
                    if (n.output) continue;
                    ImVec2 na = MeToScreen(org, n.x, n.y), nb = MeToScreen(org, n.x + MeNodeW(n), n.y + (n.kind == MEK_COMMENT ? 30 : MeNodeH(n)));
                    if (nb.x >= a.x && na.x <= b.x && nb.y >= a.y && na.y <= b.y) { M.selSet.insert(n.id); M.sel = n.id; }
                }
            }
        }
    }
    // comments behind, then wires, then nodes
    ImFont* font = ImGui::GetFont(); float fs = std::max(6.f, ImGui::GetFontSize() * M.zoom);
    for (auto& n : M.nodes) if (n.kind == MEK_COMMENT) MeDrawComment(dl, org, n, font, fs);
    MeNode* out = MeOutput();
    if (out) for (auto& l : M.links) {
        MeNode* n = MeFind(l.second); if (!n) continue;
        int o = M.linkOut.count(l.first) ? M.linkOut[l.first] : 0;
        MeBezier(dl, MeOutPin(org, *n, o), MeInPin(org, *out, l.first), MeIsStd(l.first) ? MeOutCol(*n, o, 0.9f) : MeKindCol(M.pins[l.first].kind, 0.9f), MeZ(2.2f));
    }
    for (auto& kv : M.nin) {
        MeNode* d = MeFind((int)(kv.first / 64)); MeNode* sN = MeFind(kv.second.first); if (!d || !sN) continue;
        MeBezier(dl, MeOutPin(org, *sN, kv.second.second), MeNodeInPin(org, *d, (int)(kv.first % 64)), MeOutCol(*sN, kv.second.second, 0.9f), MeZ(2.2f));
    }
    for (auto& n : M.nodes) MeDrawNode(dl, org, n, font, fs);
    if (M.boxing) {
        ImVec2 a(std::min(M.boxFrom.x, m.x), std::min(M.boxFrom.y, m.y)), b(std::max(M.boxFrom.x, m.x), std::max(M.boxFrom.y, m.y));
        dl->AddRectFilled(a, b, IM_COL32(240, 170, 40, 30)); dl->AddRect(a, b, IM_COL32(240, 170, 40, 200));
    }
    // a node with a problem says what it is when you point at it
    if (hov && !M.boxing && M.linkFrom < 0) { int h = MeHitNode(org, m); std::string e = MeNodeError(h); if (!e.empty()) ImGui::SetTooltip("%s", e.c_str()); }
    // wire being dragged
    if (M.linkFrom >= 0) {
        MeNode* n = MeFind(M.linkFrom);
        int ip = MeHitInPin(org, m), ii = 0, ni = MeHitNodeIn(org, m, &ii);
        bool okPin = n && ip >= 0 && MeCanLink(ip, *n, g_meLinkOut);
        bool okIn = n && ip < 0 && ni >= 0 && n->kind != MEK_MAT && !MeDependsOn(M.linkFrom, ni);
        ImVec2 to = m;
        if (okPin && out) to = MeInPin(org, *out, ip);
        else if (okIn) if (MeNode* d = MeFind(ni)) to = MeNodeInPin(org, *d, ii);
        if (n) MeBezier(dl, MeOutPin(org, *n, g_meLinkOut), to, MeOutCol(*n, g_meLinkOut, okPin || okIn ? 1.f : 0.6f), MeZ(2.2f));
        if (n && ip >= 0 && !okPin) {
            std::string t = MeIsStd(ip) ? std::string("A material can't go into ") + M.pins[ip].name
                          : g_meLinkOut != 0 || n->kind == MEK_OP ? std::string("Only a whole ") + MeKindName(M.pins[ip].kind) + " can go into the material's own " + M.pins[ip].name + " (use the Unreal inputs below for maths and channels)"
                          : std::string("A ") + MeKindName(n->kind) + " can't go into a " + MeKindName(M.pins[ip].kind) + " input";
            dl->AddText(ImVec2(m.x + Px(14), m.y + Px(10)), IM_COL32(240, 120, 110, 255), t.c_str());
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (okPin) MeLink(ip, n->id, g_meLinkOut);
            else if (okIn) MeLinkIn(ni, ii, n->id, g_meLinkOut);
            else if (n && hov && ip < 0 && ni < 0 && MeHitNode(org, m) < 0) {   // let go on empty graph: pick a node to wire it into (like Unreal)
                M.rclickAt = m; g_meWireToNew = n->id; g_meWireToNewOut = g_meLinkOut; ImGui::OpenPopup("##meadd");
            }
            M.linkFrom = -1;
        }
    }
    // hints
    {
        ImGui::PushFont(F_small);
        const char* hint = "Drag from a pin to wire it  \xC2\xB7  right-click: add a node  \xC2\xB7  right-drag: pan  \xC2\xB7  wheel: zoom  \xC2\xB7  drag on empty: select  \xC2\xB7  Ctrl+C / V / D / Z / Y  \xC2\xB7  C: comment  \xC2\xB7  Alt+click a pin: break";
        dl->AddText(ImVec2(org.x + Px(14), end.y - Px(22)), IM_COL32(130, 130, 138, 255), hint);
        ImGui::PopFont();
    }
    dl->PopClipRect();
    // keys (while the graph has the focus and nothing is being typed)
    bool keys = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput && hov;
    if (keys || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || (ImGui::IsKeyPressed(ImGuiKey_Backspace) && hov)) MeDeleteSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) MeCopy();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) { MeCopy(); MeDeleteSelected(); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) { ImVec2 gp = MeToGraph(org, hov ? m : ImVec2(org.x + size.x / 2, org.y + size.y / 2)); if (const char* c = ImGui::GetClipboardText()) MePasteText(c, gp.x, gp.y); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) MeDuplicate();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) { M.selSet.clear(); for (auto& n : M.nodes) if (!n.output) M.selSet.insert(n.id); }
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) MeUndo();
        if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)))) MeRedo();
        if (!io.KeyCtrl && hov && ImGui::IsKeyPressed(ImGuiKey_C, false)) MeCommentAroundSelection();
        if (!io.KeyCtrl && hov && ImGui::IsKeyPressed(ImGuiKey_Home, false)) M.fitted = false;
    }
    // menus
    if (ImGui::BeginPopup("##meadd")) {
        ImVec2 gp = MeToGraph(org, M.rclickAt);
        size_t before = M.nodes.size();
        MeAddMenu(gp.x, gp.y);
        if (M.nodes.size() > before && g_meWireToNew >= 0) {     // a node picked for a dragged wire: plug it into its first input
            int id = M.nodes.back().id; if (MeNode* nn = MeFind(id)) if (!MeNodeIns(*nn).empty()) MeLinkIn(id, 0, g_meWireToNew, g_meWireToNewOut);
            g_meWireToNew = -1;
        }
        ImGui::EndPopup();
    } else g_meWireToNew = -1;
    if (ImGui::BeginPopup("##menode")) {
        MeNode* n = MeFind(M.sel);
        auto sel = MeSelection();
        if (n && !n->output) {
            if (n->kind == MEK_TEX) {
                if (ImGui::MenuItem("Pick a texture from the game...")) { ImGui::CloseCurrentPopup(); MeOpenPicker(1, n->id); }
                if (ImGui::MenuItem("Pick a picture file...")) MePickFile(n->id);
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_DOWNLOAD "  Export texture")) MeExportTexture(MeParamValue(*n), MeFeeds(n->id));
                if (ImGui::MenuItem(ICON_FOLDER_OPEN "  Open exported textures folder")) MeOpenExportDir();
                ImGui::Separator();
            }
            if (n->kind == MEK_MAT && ImGui::MenuItem("Pick a material...")) { ImGui::CloseCurrentPopup(); MeOpenPicker(2, n->id); }
            if ((n->kind == MEK_SCA || n->kind == MEK_VEC || n->kind == MEK_TEX) && ImGui::MenuItem(MeIsParam(*n) ? "Convert to constant" : "Convert to parameter")) {
                if (MeIsParam(*n)) n->name.clear(); else n->name = n->kind == MEK_TEX ? "Texture" : n->kind == MEK_VEC ? "Color" : "Scalar";
            }
            if (ImGui::MenuItem(ICON_COPY "  Copy", "Ctrl+C")) MeCopy();
            if (ImGui::MenuItem(ICON_CLONE "  Duplicate", "Ctrl+D")) MeDuplicate();
            if (ImGui::MenuItem(ICON_COMMENT "  Comment around the selection", "C")) MeCommentAroundSelection();
            if (ImGui::MenuItem("Break all its wires")) {
                std::vector<int> pins; for (auto& l : M.links) if (l.second == n->id) pins.push_back(l.first); for (int p : pins) MeUnlink(p);
                std::vector<long long> ins; for (auto& kv : M.nin) if (kv.second.first == n->id || (int)(kv.first / 64) == n->id) ins.push_back(kv.first);
                for (auto key : ins) MeUnlinkIn((int)(key / 64), (int)(key % 64));
            }
            ImGui::Separator();
            if (ImGui::MenuItem(sel.size() > 1 ? (ICON_TRASH "  Delete " + std::to_string(sel.size()) + " nodes").c_str() : ICON_TRASH "  Delete", "Del")) MeDeleteSelected();
        } else EdLabel("The material's output can't be removed.");
        ImGui::EndPopup();
    }
}
// ---- the game's own picture of the material (the game renders a sphere with it): for the material's own settings,
// which the launcher's preview can't know (only the game has its shader)
static void MeGamePreview() {
    float w = ImGui::GetContentRegionAvail().x, h = std::max(Px(80), ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 2);
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(a, b, IM_COL32(58, 64, 74, 255), IM_COL32(58, 64, 74, 255), IM_COL32(22, 23, 26, 255), IM_COL32(22, 23, 26, 255));
    ID3D11ShaderResourceView* srv = nullptr;
    if (!M.prevPng.empty() && FileExists(U2W(M.prevPng))) srv = MatThumbFor(U2W(M.prevPng));
    static ID3D11ShaderResourceView* lastSrv = nullptr;
    if (srv) lastSrv = srv;
    float side = std::min(w, h) * 0.92f, cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
    if (lastSrv) dl->AddImage((ImTextureID)lastSrv, ImVec2(cx - side / 2, cy - side / 2), ImVec2(cx + side / 2, cy + side / 2));
    else { ImGui::PushFont(F_small); dl->AddText(ImVec2(a.x + Px(10), a.y + Px(10)), ImGui::GetColorU32(P.dim), "Asking the game for a picture..."); ImGui::PopFont(); }
    if (!M.prevPng.empty() && !srv && GetTickCount64() - M.prevAsked > 4000 && FileExists(U2W(M.prevPng.substr(0, M.prevPng.size() - 4) + ".fail"))) {
        ImGui::PushFont(F_small); dl->AddText(ImVec2(a.x + Px(10), b.y - Px(22)), ImGui::GetColorU32(P.dim), "The game couldn't make a preview."); ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PushFont(F_small); EdLabel("The material as the game draws it (its own shader), on a sphere. Changes to its settings show here."); ImGui::PopFont();
}

// a row in the Details: label on the left, the control on the right (like Unreal's property grid)
static float g_meLabelW = 0;
static void MeProp(const char* label, const char* tip = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextUnformatted(label); ImGui::PopStyleColor();
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine(g_meLabelW); ImGui::SetNextItemWidth(-1);
}
// the material's own properties (shown when the output node, or nothing, is selected)
static void MeMaterialProps() {
    bool ch = false;
    SectionLabel("Material");
    const char* blends[] = { "Opaque", "Masked", "Translucent", "Additive" };
    MeProp("Blend Mode", "Opaque: solid. Masked: Opacity Mask cuts holes. Translucent: Opacity blends it. Additive: adds light (glow, fire).");
    ch |= ImGui::Combo("##blend", &M.props.blend, blends, 4);
    const char* shading[] = { "Default Lit", "Unlit" };
    int sh = M.props.unlit ? 1 : 0;
    MeProp("Shading Model", "Unlit: only Emissive Color shows (no light)");
    if (ImGui::Combo("##shade", &sh, shading, 2)) { M.props.unlit = sh == 1; ch = true; }
    MeProp("Two Sided"); ch |= ImGui::Checkbox("##two", &M.props.twoSided);
    ImGui::BeginDisabled(M.props.blend != BLEND_MASKED);
    MeProp("Opacity Mask Clip Value", "Below this, a Masked material is cut away"); ch |= ImGui::SliderFloat("##clip", &M.props.clip, 0, 1, "%.3f");
    ImGui::EndDisabled();
    MeProp("Displacement Scale", "How far Displacement moves the surface (preview)"); ch |= ImGui::DragFloat("##disp", &M.props.dispScale, 0.002f, -1, 1, "%.3f");
    MeProp("World Position Offset Scale"); ch |= ImGui::DragFloat("##wpos", &M.props.wpoScale, 0.01f, 0, 10, "%.2f");
    if (ch) { MeStdChanged(); if (!M.asset.empty()) M.dirty = true; }
    ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("In the game, the material keeps its own blend mode and shading (a shipped game can't change them); the preview shows them as set here.");
    ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
    if (M.cur < (int)M.slots.size() && M.asset.empty()) {
        const MeSlot& s = M.slots[M.cur];
        SectionLabel("In the game");
        EdLabel("Material"); ImGui::TextWrapped("%s", s.mat.c_str());
        if (!s.base.empty() && s.base != s.mat) { EdLabel("Based on"); ImGui::TextWrapped("%s", s.base.c_str()); }
        EdLabel("Used by"); ImGui::TextWrapped("%s, slot %d%s", (s.comp.rfind("#", 0) == 0 ? "part " + s.comp.substr(1) : s.comp).c_str(), s.slot, s.slotName.empty() ? "" : (" (" + s.slotName + ")").c_str());
        ImGui::Dummy(ImVec2(0, Px(4)));
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextWrapped("The output's first inputs are this material's own settings (%d): they change in the game straight away. The Unreal inputs below them are worked out by the launcher and baked into textures, which go into the material's texture slots.", M.stdStart - 1);
        ImGui::PopStyleColor();
        if (ImGui::Button(ICON_WAND "  Start from this material's textures", ImVec2(-1, 0))) MeUseItsTextures();
    }
}
static void MeOpenTexLibraryPicker(int node);
static void MeDetails() {
    g_meLabelW = std::min(Px(150), ImGui::GetContentRegionAvail().x * 0.45f);
    auto sel = MeSelection();
    MeNode* n = MeFind(M.sel);
    if (sel.size() > 1) {
        ImGui::PushFont(F_h2); ImGui::Text("%d nodes selected", (int)sel.size()); ImGui::PopFont();
        if (ImGui::Button(ICON_COMMENT "  Comment around them", ImVec2(-1, 0))) MeCommentAroundSelection();
        if (ImGui::Button(ICON_CLONE "  Duplicate", ImVec2(-1, 0))) MeDuplicate();
        if (ImGui::Button(ICON_COPY "  Copy", ImVec2(-1, 0))) MeCopy();
        ImVec4 e = P.err; if (GhostButton(ICON_TRASH "  Delete them", ImVec2(-1, 0), &e)) MeDeleteSelected();
        return;
    }
    if (!n || n->output) { MeMaterialProps(); return; }
    std::string title = n->kind == MEK_COMMENT ? "Comment" : MeNodeTitle(*n);
    ImGui::PushFont(F_h2); ImGui::TextWrapped("%s", title.c_str()); ImGui::PopFont();
    if (n->kind == MEK_OP) { ImGui::PushFont(F_small); EdLabel(g_meOpHelp[n->op]); ImGui::PopFont(); }
    std::vector<std::string> feeds; for (auto& l : M.links) if (l.second == n->id) feeds.push_back(M.pins[l.first].kind == MEK_MAT ? "Material" : M.pins[l.first].name);
    for (auto& kv : M.nin) if (kv.second.first == n->id) if (MeNode* d = MeFind((int)(kv.first / 64))) { auto ins = MeNodeIns(*d); feeds.push_back(MeNodeTitle(*d) + " (" + (kv.first % 64 < (long long)ins.size() ? ins[kv.first % 64] : "?") + ")"); }
    std::string fl; for (size_t i = 0; i < feeds.size(); i++) fl += (i ? ", " : "") + feeds[i];
    if (n->kind != MEK_COMMENT) { ImGui::PushFont(F_small); EdLabel(feeds.empty() ? "Not plugged in" : ("Plugged into: " + fl).c_str()); ImGui::PopFont(); }
    std::string err = MeNodeError(n->id);
    if (!err.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, P.err); ImGui::TextWrapped(ICON_WARN "  %s", err.c_str()); ImGui::PopStyleColor(); }
    ImGui::Dummy(ImVec2(0, Px(4)));
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // parameters (Scalar / Vector / Texture Parameter): their name, which material instances use
    if (n->kind == MEK_SCA || n->kind == MEK_VEC || n->kind == MEK_TEX) {
        bool isParam = MeIsParam(*n);
        MeProp("Parameter", "A named value: material instances of this material can change it");
        if (ImGui::Checkbox("##isparam", &isParam)) n->name = isParam ? (n->kind == MEK_TEX ? "Texture" : n->kind == MEK_VEC ? "Color" : "Scalar") : "";
        if (MeIsParam(*n)) {
            char nb[96]; snprintf(nb, sizeof nb, "%s", n->name.c_str());
            MeProp("Parameter Name");
            if (ImGui::InputText("##pname", nb, sizeof nb) && nb[0]) n->name = nb;
            for (auto& o : M.nodes) if (o.id != n->id && o.name == n->name && MeIsParam(o) && o.kind != n->kind) { ImGui::PushStyleColor(ImGuiCol_Text, P.err); ImGui::TextWrapped("Another parameter of a different type has this name"); ImGui::PopStyleColor(); break; }
        }
    }
    if (n->kind == MEK_TEX) {
        float s = std::min(w, Px(160));
        ImVec2 p = ImGui::GetCursorScreenPos();
        MeTexBox(dl, n->value, p, ImVec2(p.x + s, p.y + s), Px(4));
        ImGui::Dummy(ImVec2(s, s));
        ImGui::PushFont(F_small); ImGui::TextWrapped("%s", n->value.rfind("file:", 0) == 0 ? n->value.substr(5).c_str() : n->value.empty() ? "(no texture)" : n->value.c_str()); ImGui::PopFont();
        if (AccentButton(ICON_SEARCH "  From the game...", ImVec2(w / 2 - Px(2), 0))) MeOpenPicker(1, n->id);
        ImGui::SameLine(0, Px(4));
        if (ImGui::Button(ICON_IMAGE "  Texture library...", ImVec2(-1, 0))) MeOpenTexLibraryPicker(n->id);
        if (ImGui::Button(ICON_FILE_IMPORT "  Picture file...", ImVec2(w / 2 - Px(2), 0))) MePickFile(n->id);
        ImGui::SameLine(0, Px(4));
        if (ImGui::Button(ICON_DOWNLOAD "  Export texture", ImVec2(-1, 0))) MeExportTexture(n->value, MeFeeds(n->id));
        SectionLabel("Sampler");
        bool ch = false;
        const char* st[] = { "Color", "Normal", "Masks (linear)" };
        MeProp("Sampler Type", "How the texture is read: Color is sRGB, Normal unpacks a normal map, Masks reads plain numbers (roughness, metal...)");
        ch |= ImGui::Combo("##smp", &n->sampler, st, 3);
        const char* fl2[] = { "Bilinear", "Nearest (pixelated)", "Anisotropic" };
        MeProp("Filter"); ch |= ImGui::Combo("##filter", &n->filter, fl2, 3);
        MeProp("Tiling", "How many times across (U, V)"); ch |= ImGui::DragFloat2("##tile", n->tile, 0.02f, -256, 256, "%.2f");
        MeProp("Offset"); ch |= ImGui::DragFloat2("##off", n->off, 0.005f, -16, 16, "%.3f");
        MeProp("Rotation", "Degrees, around the middle"); ch |= ImGui::DragFloat("##rot", &n->rot, 0.5f, -360, 360, "%.1f\xC2\xB0");
        if (ch) MeStdChanged();
        if (n->value.rfind("file:", 0) != 0 && !n->value.empty() && MeTexturePic(n->value) == nullptr) {
            ImGui::PushFont(F_small); EdLabel("(no picture of it: the launcher couldn't read it from the game's files)"); ImGui::PopFont();
        }
    } else if (n->kind == MEK_VEC) {
        float c[4]; MeColour(n->value, c);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::ColorPicker4("##col", c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_PickerHueWheel)) {
            char t[96]; snprintf(t, sizeof t, "%.4f,%.4f,%.4f,%.4f", c[0], c[1], c[2], c[3]); n->value = t; MeNodeChanged(*n, false);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) MeNodeChanged(*n, true);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat4("##colv", c, 0.005f, -100, 100, "%.3f")) { char t[96]; snprintf(t, sizeof t, "%.4f,%.4f,%.4f,%.4f", c[0], c[1], c[2], c[3]); n->value = t; MeNodeChanged(*n, false); }
        if (ImGui::IsItemDeactivatedAfterEdit()) MeNodeChanged(*n, true);
        ImGui::PushFont(F_small); EdLabel("Values over 1 make it glow, where the material uses the colour as light."); ImGui::PopFont();
    } else if (n->kind == MEK_SCA) {
        float v = (float)atof(n->value.c_str());
        ImGui::SetNextItemWidth(-1);
        bool ch = ImGui::SliderFloat("##sl", &v, 0, 1, "%.3f");
        ImGui::SetNextItemWidth(-1);
        ch |= ImGui::DragFloat("##dr", &v, 0.01f, 0, 0, "%.3f");
        if (ch) { char t[32]; snprintf(t, sizeof t, "%.4f", v); n->value = t; MeNodeChanged(*n, false); }
        if (ImGui::IsItemDeactivatedAfterEdit()) MeNodeChanged(*n, true);
    } else if (n->kind == MEK_OP) {
        auto ins = MeNodeIns(*n);
        bool ch = false;
        auto constFor = [&](int k, int inIdx, const char* label, float lo, float hi) {
            if (inIdx >= 0 && inIdx < (int)ins.size() && M.nin.count(MeInKey(n->id, inIdx))) { MeProp(label); ImGui::TextDisabled("(plugged in)"); return; }
            MeProp(label); ch |= ImGui::DragFloat((std::string("##c") + label).c_str(), &n->c[k], 0.01f, lo, hi, "%.3f");
        };
        switch (n->op) {
        case OP_TEXCOORD: MeProp("Tiling"); ch |= ImGui::DragFloat2("##tile", n->c, 0.05f, -64, 64, "%.2f"); break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MIN: case OP_MAX: constFor(0, 0, "Const A", 0, 0); constFor(1, 1, "Const B", 0, 0); break;
        case OP_LERP: constFor(0, 0, "Const A", 0, 0); constFor(1, 1, "Const B", 0, 0); constFor(2, 2, "Const Alpha", 0, 1); break;
        case OP_POW: constFor(0, 0, "Const Base", 0, 0); constFor(1, 1, "Const Exponent", 0, 16); break;
        case OP_CLAMP: constFor(0, 0, "Const Input", 0, 0); constFor(1, 1, "Min", 0, 0); constFor(2, 2, "Max", 0, 0); break;
        case OP_DESAT: constFor(1, 1, "Fraction", 0, 1); break;
        case OP_SIN: case OP_COS: constFor(0, -1, "Period", 0, 0); break;
        case OP_FRESNEL: constFor(0, 0, "Exponent", 0, 64); constFor(1, 1, "Base Reflect Fraction", 0, 1); break;
        case OP_NORMALSTR: constFor(1, 1, "Strength", -8, 8); break;
        case OP_PANNER: MeProp("Speed", "UV per second (X, Y)"); ch |= ImGui::DragFloat2("##spd", n->c, 0.005f, -16, 16, "%.3f"); break;
        case OP_ROTATOR: MeProp("Centre"); ch |= ImGui::DragFloat2("##ctr", n->c, 0.005f, -4, 4, "%.3f"); constFor(2, -1, "Speed", -16, 16); break;
        case OP_TIME: constFor(0, -1, "Speed", -16, 16); break;
        case OP_BUMPOFFSET: constFor(0, 2, "Height Ratio", -1, 1); constFor(1, -1, "Reference Plane", 0, 1); break;
        case OP_MASK: {
            MeProp("Channels");
            const char* cn[4] = { "R", "G", "B", "A" };
            for (int k = 0; k < 4; k++) { if (k) ImGui::SameLine(); bool b = n->c[k] > 0.5f; if (ImGui::Checkbox(cn[k], &b)) { n->c[k] = b ? 1.f : 0.f; ch = true; } }
            break;
        }
        case OP_LAYERBLEND: {
            EdLabel("Layers (each layer's preview weight, unless its Weight input is plugged in)");
            int del = -1;
            for (int l = 0; l < (int)n->layers.size(); l++) {
                ImGui::PushID(l);
                char nb[64]; snprintf(nb, sizeof nb, "%s", n->layers[l].first.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.42f);
                if (ImGui::InputText("##ln", nb, sizeof nb) && nb[0]) n->layers[l].first = nb;
                ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Px(30));
                ch |= ImGui::SliderFloat("##lw", &n->layers[l].second, 0, 1, "weight %.2f");
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_TRASH)) del = l;
                ImGui::PopID();
            }
            if (del >= 0 && n->layers.size() > 1) {
                std::map<long long, std::pair<int, int>> keep;
                for (auto& kv : M.nin) {
                    if ((int)(kv.first / 64) != n->id) { keep[kv.first] = kv.second; continue; }
                    int in = (int)(kv.first % 64);
                    if (in / 2 == del) continue;
                    keep[MeInKey(n->id, in / 2 > del ? in - 2 : in)] = kv.second;
                }
                M.nin.swap(keep); n->layers.erase(n->layers.begin() + del); ch = true;
            }
            if (n->layers.size() < 8 && ImGui::Button(ICON_PLUS "  Add layer", ImVec2(-1, 0))) { n->layers.push_back({ "Mat" + std::to_string(n->layers.size() + 1), 0.f }); ch = true; }
            break;
        }
        default: break;
        }
        if (ch) MeStdChanged();
        if (MeHasPreview(*n)) if (ID3D11ShaderResourceView* pv = MePreviewOf(n->id)) {
            ImGui::Dummy(ImVec2(0, Px(4))); EdLabel("What it makes (on a flat square, at time 0)");
            float s = std::min(w, Px(160)); ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddImage((ImTextureID)pv, p, ImVec2(p.x + s, p.y + s)); ImGui::Dummy(ImVec2(s, s));
        }
    } else if (n->kind == MEK_COMMENT) {
        static char buf[1024]; snprintf(buf, sizeof buf, "%s", n->name.c_str());
        MeProp("Text");
        if (ImGui::InputTextMultiline("##ct", buf, sizeof buf, ImVec2(-1, Px(70)))) n->name = buf;
        MeProp("Colour");
        for (int k = 0; k < (int)(sizeof g_meCommentCols / sizeof g_meCommentCols[0]); k++) {
            if (k) ImGui::SameLine(0, Px(4));
            ImGui::PushID(k);
            ImVec4 c = Hex(g_meCommentCols[k]);
            if (ImGui::ColorButton("##cc", c, ImGuiColorEditFlags_NoTooltip, ImVec2(Px(22), Px(22)))) n->col = g_meCommentCols[k];
            ImGui::PopID();
        }
        MeProp("Size"); float sz[2] = { n->w, n->h }; if (ImGui::DragFloat2("##csz", sz, 1, 160, 4000, "%.0f")) { n->w = sz[0]; n->h = sz[1]; }
    } else if (n->kind == MEK_MAT) {
        ImGui::TextWrapped("%s", n->value.empty() ? "(none)" : n->value.c_str());
        if (AccentButton(ICON_SEARCH "  Pick a material...", ImVec2(-1, 0))) MeOpenPicker(2, n->id);
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::TextWrapped("Plugged into Material, it replaces the whole material: the graph then shows the new material's settings."); ImGui::PopStyleColor();
    }
    // settings this node feeds that the material had differently: show the original
    for (auto& l : M.links) if (l.second == n->id && !MeIsStd(l.first) && M.pins[l.first].kind != MEK_MAT && M.pins[l.first].orig != n->value) {
        const MeParam& p = M.pins[l.first];
        ImGui::PushID(l.first);
        ImGui::PushFont(F_small);
        std::string o = p.kind == MEK_TEX ? MeShort(p.orig) : p.orig;
        EdLabel((p.name + " was " + o).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::SmallButton("Put back")) { n->value = p.orig; MeNodeChanged(*n, true); }
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, Px(6)));
    ImVec4 e = P.err;
    if (GhostButton(ICON_TRASH "  Delete node", ImVec2(-1, 0), &e)) MeDeleteSelected();
}

// ---- Stats: what the compiler said, and what the shader costs
static void MeStatsPanel() {
    ImGui::PushFont(F_small);
    if (g_msBusy) EdLabel("Compiling...");
    int errs = 0;
    for (auto& e : g_ms.src.errs) {
        errs++;
        ImGui::PushStyleColor(ImGuiCol_Text, P.err);
        std::string t = std::string(ICON_CIRCLE_XMARK "  ") + (e.first >= 0 ? "[" + (MeFind(e.first) ? MeNodeTitle(*MeFind(e.first)) : std::string("node")) + "] " : "") + e.second;
        if (ImGui::Selectable(t.c_str()) && e.first >= 0) { MeSelectOnly(e.first); if (MeNode* n = MeFind(e.first)) { (void)n; } }
        ImGui::PopStyleColor();
    }
    if (!g_ms.log.empty()) {
        std::istringstream ss(g_ms.log); std::string line;
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            bool isErr = line.find("error") != std::string::npos || line.find(": E") != std::string::npos;
            if (isErr) errs++;
            ImGui::PushStyleColor(ImGuiCol_Text, isErr ? P.err : P.warn);
            ImGui::TextWrapped("%s  %s", isErr ? ICON_CIRCLE_XMARK : ICON_WARN, line.c_str());
            ImGui::PopStyleColor();
        }
    }
    for (auto& w : g_ms.src.warns) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.warn);
        std::string t = std::string(ICON_WARN "  ") + (w.first >= 0 && MeFind(w.first) ? "[" + MeNodeTitle(*MeFind(w.first)) + "] " : "") + w.second;
        if (ImGui::Selectable(t.c_str()) && w.first >= 0) MeSelectOnly(w.first);
        ImGui::PopStyleColor();
    }
    if (g_ms.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.ok);
        ImGui::Text(ICON_CIRCLE_CHECK "  Compiled in %.0f ms", g_ms.ms);
        ImGui::PopStyleColor();
    }
    int nodes = 0; for (auto& n : M.nodes) if (!n.output && n.kind != MEK_COMMENT) nodes++;
    EdLabel(("Base pass shader: " + std::to_string(g_ms.psInstr) + " instructions  \xC2\xB7  vertex shader: " + std::to_string(g_ms.vsInstr) + " instructions").c_str());
    EdLabel(("Texture samplers: " + std::to_string(g_ms.src.tex.size()) + " / " + std::to_string(MS_MAX_TEX) + "  \xC2\xB7  texture lookups: " + std::to_string(g_ms.psTex) +
             "  \xC2\xB7  values: " + std::to_string(g_ms.src.slots) + " / " + std::to_string(MS_MAX_SLOTS) + "  \xC2\xB7  nodes: " + std::to_string(nodes)).c_str());
    if (!M.bakeNote.empty()) EdLabel(("Game: " + M.bakeNote).c_str());
    ImGui::PopFont();
}

#include "mgtex.inc"
#include "mgasset.inc"

// ---- the whole editor (replaces the tabs while it's open), laid out like Unreal 5's Material Editor:
// toolbar on top; Palette (node library) on the left; the graph in the middle with Stats under it; Viewport and
// Details on the right. Tabs switch to the Texture Editor and the Material Instance editor.
static float g_meSplitR = 0.46f;            // viewport height / right column
static bool g_meTabJump = false; static int g_meTabWas = 0;
static bool g_meVpJump = false;
static int g_meVpTab = 0;                    // the viewport shows the launcher's preview (0) or the game's own picture (1)      // (a tab chosen by the program, not a click)
static void MeToolButton(const char* icon, const char* label, bool enabled, const char* tip, const std::function<void()>& f, bool accent = false) {
    ImGui::BeginDisabled(!enabled);
    ImGui::BeginGroup();
    float w = std::max(Px(52), ImGui::CalcTextSize(label).x + Px(12));
    ImGui::PushID(label);
    bool hit = accent ? AccentButton((std::string(icon) + "##b").c_str(), ImVec2(w, Px(28))) : ImGui::Button((std::string(icon) + "##b").c_str(), ImVec2(w, Px(28)));
    ImGui::PopID();
    ImGui::PushFont(F_small);
    float tw = ImGui::CalcTextSize(label).x; ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - tw) / 2); ImGui::TextUnformatted(label);
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && tip) ImGui::SetTooltip("%s", tip);
    if (hit) f();
    ImGui::SameLine(0, Px(4));
}
static void MeFileMenu() {
    bool asset = !M.asset.empty();
    if (ImGui::IsWindowAppearing()) MeLibReload();          // (files may have changed outside the launcher)
    if (ImGui::MenuItem(ICON_PLUS "  New Material")) MeNewAsset();
    if (ImGui::BeginMenu(ICON_FOLDER_OPEN "  Open from the library")) { MeLibraryMenu(false); ImGui::EndMenu(); }
    ImGui::Separator();
    if (asset && ImGui::MenuItem(ICON_FLOPPY "  Save", "Ctrl+S")) { MeSaveAsset(); if (!g_meInst.empty()) MeSaveInstance(); }
    if (ImGui::MenuItem(ICON_FLOPPY "  Save as material asset...")) {
        std::string base = asset ? W2U(M.asset.substr(M.asset.find_last_of(L"\\/") + 1)) : "M_" + (M.cur < (int)M.slots.size() ? MeShort(M.slots[M.cur].mat) : std::string("Material"));
        if (base.find('.') != std::string::npos) base = base.substr(0, base.find_last_of('.'));
        MakeDirs(MeLibDir());
        OvBrowse(L"Material (*.uemat)\0*.uemat\0", L"Save the graph as a material asset", MeLibDir() + L"\\", true, MeSafeName(base), L"uemat", [](std::wstring f) {
            if (f.empty()) return;
            if (Lower(f).size() < 6 || Lower(f).substr(f.size() - 6) != L".uemat") f += L".uemat";
            if (MeSaveAssetAs(f)) { g_toasts.push_back({ "Saved " + W2U(f), false, GetTickCount64() }); if (!M.asset.empty()) { M.asset = f; M.dirty = false; } }
        });
    }
    ImGui::BeginDisabled(!asset);
    if (ImGui::MenuItem(ICON_SLIDERS "  New Material Instance of this")) MeNewInstance();
    ImGui::EndDisabled();
    if (!asset && ImGui::BeginMenu(ICON_FILE_IMPORT "  Load a material from the library into this slot")) { MeLibraryMenu(true); ImGui::EndMenu(); }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FILE_CODE "  Export shader code (.hlsl)...")) MeExportShader();
    if (ImGui::MenuItem(ICON_FILE_EXPORT "  Export baked textures...")) MeExportBaked();
    if (asset && ImGui::MenuItem(ICON_CUBE "  Apply to the selected object")) MeApplyAssetToSelected();
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_CIRCLE_XMARK "  Close the material editor")) MatEdClose();
}
static void MatEdDraw() {
    MePollGraph();
    MeFlush();
    {
        std::lock_guard<std::mutex> lk(g_meExMx);
        for (auto& m : g_meExMsgs) { g_toasts.push_back({ m.first, m.second, GetTickCount64() }); Log(U2W("Material editor: " + m.first), m.second ? LOG_WARN : LOG_INFO); }
        g_meExMsgs.clear();
    }
    bool asset = !M.asset.empty();
    if (!asset && M.loaded && !M.slots.empty() && M.builtFor != M.cur) MeBuild();
    MePendingStep();
    MeHistoryTrack();
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.WantTextInput) { if (asset) { MeSaveAsset(); if (!g_meInst.empty()) MeSaveInstance(); } else if (!M.builtKey.empty()) { MeSaveGraphFile(M.builtKey, MeTakeCustom()); g_toasts.push_back({ "Saved the graph", false, GetTickCount64() }); } }

    // ---- toolbar
    if (ImGui::Button(ICON_BARS " File")) ImGui::OpenPopup("##mefile");
    if (ImGui::BeginPopup("##mefile")) { MeFileMenu(); ImGui::EndPopup(); }
    if (!M.open) return;
    ImGui::SameLine(0, Px(10));
    ImVec2 tb = ImGui::GetCursorPos();
    MeToolButton(ICON_FLOPPY, "Save", true, asset ? "Save the material asset (Ctrl+S)" : "Save the graph (it's also kept with the level's layout). File > Save as material asset to keep it in the library.", [&]() {
        if (asset) { MeSaveAsset(); if (!g_meInst.empty()) MeSaveInstance(); } else if (!M.builtKey.empty()) { MeSaveGraphFile(M.builtKey, MeTakeCustom()); g_toasts.push_back({ "Saved the graph", false, GetTickCount64() }); }
    });
    MeToolButton(ICON_HAMMER, "Compile", !g_msBusy, "Compile the graph into a shader now (it also compiles by itself as you work)", []() { g_msForce = true; });
    if (!asset) MeToolButton(ICON_CIRCLE_CHECK, M.baking ? "Baking..." : "Apply", !M.baking && MeStdUsed(), "Bake what's wired into the Unreal inputs and put it on the object in the game", []() { MeStartBake(); }, true);
    else MeToolButton(ICON_CIRCLE_CHECK, "Apply", !g_ed.sel.empty(), g_ed.sel.empty() ? "Select an object in the game first, then apply this material to it" : "Apply this material to the selected object (baked into its textures)", []() { MeApplyAssetToSelected(); }, true);
    MeToolButton(ICON_UNDO, "Undo", !g_meUndo.empty(), "Undo (Ctrl+Z)", []() { MeUndo(); });
    MeToolButton(ICON_REDO, "Redo", !g_meRedo.empty(), "Redo (Ctrl+Y)", []() { MeRedo(); });
    MeToolButton(ICON_EXPAND, "Home", true, "Show the whole graph (Home)", []() { M.fitted = false; });
    MeToolButton(ICON_SITEMAP, "Arrange", true, "Arrange the nodes automatically", []() { MeAutoArrange(); });
    MeToolButton(ICON_GEAR, "Settings", true, "Editor settings", []() { ImGui::OpenPopup("##meset"); });
    if (ImGui::BeginPopup("##meset")) {
        ImGui::Checkbox("Live node previews", &g_meNodePreviews);
        if (!asset) {
            ImGui::Checkbox("Live Update (bake into the game a moment after every change)", &M.live);
            const int sizes[] = { 256, 512, 1024, 2048 }; int cur = 2; for (int k = 0; k < 4; k++) if (sizes[k] == M.bakeRes) cur = k;
            const char* sn[] = { "256", "512", "1024", "2048" };
            ImGui::SetNextItemWidth(Px(100));
            if (ImGui::Combo("Baked texture size", &cur, sn, 4)) { M.bakeRes = sizes[cur]; MeStdChanged(); }
        }
        ImGui::EndPopup();
    }
    // right side: which material (an object's), refresh / reset, close
    ImGui::SameLine();
    float right = ImGui::GetContentRegionMax().x;
    if (!asset) {
        ImGui::SetCursorPos(ImVec2(std::max(ImGui::GetCursorPosX(), right - Px(520)), tb.y + Px(4)));
        ImGui::BeginDisabled(M.slots.empty());
        ImGui::SetNextItemWidth(Px(300));
        auto partName = [](const MeSlot& s) { return s.comp.rfind("#", 0) == 0 ? "part " + s.comp.substr(1) : s.comp; };
        std::string curName = M.cur < (int)M.slots.size() ? MeShort(M.slots[M.cur].mat) + "  (" + partName(M.slots[M.cur]) + " #" + std::to_string(M.slots[M.cur].slot) + ")" : "";
        if (ImGui::BeginCombo("##slot", curName.c_str())) {
            for (int i = 0; i < (int)M.slots.size(); i++) {
                const MeSlot& s = M.slots[i];
                std::string l = MeShort(s.mat) + "   " + partName(s) + " #" + std::to_string(s.slot) + (s.slotName.empty() ? "" : "  " + s.slotName);
                if (ImGui::Selectable((l + "##" + std::to_string(i)).c_str(), i == M.cur)) { M.cur = i; M.builtFor = -1; M.prevWanted = true; M.lastChange = 0; }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each material the object uses (%d)", (int)M.slots.size());
        ImGui::SameLine();
        if (ImGui::Button(ICON_ROTATE "##refresh")) MeAskGraph();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read the material from the game again");
        ImGui::SameLine();
        if (ImGui::Button(ICON_ERASER " Reset")) ImGui::OpenPopup("##mereset");
        if (ImGui::BeginPopup("##mereset")) {
            if (ImGui::MenuItem("Reset this material (as the game had it)") && M.cur < (int)M.slots.size()) {
                const MeSlot& s = M.slots[M.cur];
                Ed("ed_matset", M.actor + "\t" + s.comp + "\t" + std::to_string(s.slot) + "\treset\t\t");
                M.pend.clear(); M.prevWanted = true; M.lastChange = GetTickCount64(); MeAskGraph();
            }
            if (ImGui::MenuItem("Reset every material of this object")) { Ed("ed_matresetall", M.actor); M.pend.clear(); M.prevWanted = true; M.lastChange = GetTickCount64(); MeAskGraph(); }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    } else ImGui::SetCursorPos(ImVec2(std::max(ImGui::GetCursorPosX(), right - Px(40)), tb.y + Px(4)));
    ImGui::SetCursorPosX(right - Px(34));
    if (ImGui::Button(ICON_CIRCLE_XMARK "##meclose", ImVec2(Px(32), 0))) { MatEdClose(); return; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close the material editor");
    ImGui::SetCursorPosY(tb.y + Px(28) + ImGui::GetTextLineHeight() + Px(6));

    // ---- tabs
    if (ImGui::BeginTabBar("##metabs")) {
        if (ImGui::BeginTabItem(ICON_SITEMAP "  Material Graph", nullptr, M.tab == 0 && g_meTabJump ? ImGuiTabItemFlags_SetSelected : 0)) { if (!g_meTabJump) M.tab = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(ICON_IMAGE "  Textures", nullptr, M.tab == 1 && g_meTabJump ? ImGuiTabItemFlags_SetSelected : 0)) { if (!g_meTabJump) M.tab = 1; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(ICON_SLIDERS "  Material Instance", nullptr, M.tab == 2 && g_meTabJump ? ImGuiTabItemFlags_SetSelected : 0)) { if (!g_meTabJump) M.tab = 2; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    g_meTabJump = false;
    if (M.tab != g_meTabWas) { g_meTabJump = true; g_meTabWas = M.tab; }

    if (!asset && !M.loaded) {
        EdLabel(GetTickCount64() - M.askedAt > 6000 ? "The game hasn't answered. Is it still running, with the editor connected?" : "Reading the materials from the game...");
        return;
    }
    if (!asset && M.slots.empty()) { EdLabel(M.err.empty() ? "This object has no materials." : M.err.c_str()); return; }
    if (!asset && M.builtFor != M.cur) MeBuild();

    float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() - Px(4);
    if (M.tab == 1) { TxEditor(); }
    else if (M.tab == 2) {
        float vw = std::clamp(W * 0.55f, Px(300), W - Px(300));
        ImGui::BeginChild("##instvp", ImVec2(vw, H), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        MsViewportPanel();
        ImGui::EndChild();
        ImGui::SameLine(0, Px(4));
        ImGui::BeginChild("##instp", ImVec2(0, H), ImGuiChildFlags_Borders);
        SectionLabel("Material Instance");
        MeInstancePanel();
        ImGui::EndChild();
    } else {
        float palW = std::clamp(W * 0.15f, Px(180), Px(260)), rightW = std::clamp(W * 0.27f, Px(280), Px(440));
        float statsH = std::clamp(H * 0.2f, Px(90), Px(170));
        // Palette
        ImGui::BeginChild("##mepal", ImVec2(palW, H), ImGuiChildFlags_Borders);
        SectionLabel("Palette");
        MePalettePanel();
        ImGui::EndChild();
        ImGui::SameLine(0, Px(4));
        // graph + stats
        ImGui::BeginGroup();
        float gw = W - palW - rightW - Px(8);
        ImGui::BeginChild("##mecanvas", ImVec2(gw, H - statsH - Px(4)), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        MeCanvas();
        ImGui::EndChild();
        ImGui::BeginChild("##mestats", ImVec2(gw, statsH), ImGuiChildFlags_Borders);
        MeStatsPanel();
        ImGui::EndChild();
        ImGui::EndGroup();
        ImGui::SameLine(0, Px(4));
        // viewport over details (a splitter between them)
        ImGui::BeginGroup();
        float vpH = std::clamp(H * g_meSplitR, Px(160), H - Px(160));
        ImGui::BeginChild("##mevp", ImVec2(rightW, vpH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        int& vpTab = g_meVpTab;
        {   // a new material: the game's own picture when nothing is wired into the Unreal inputs yet
            static std::string vpFor;
            if (!asset && vpFor != M.builtKey) { vpFor = M.builtKey; vpTab = MeStdUsed() ? 0 : 1; g_meVpJump = true; }
        }
        if (!asset) {
            if (ImGui::BeginTabBar("##vptabs")) {
                if (ImGui::BeginTabItem("Preview", nullptr, g_meVpJump && vpTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { if (!g_meVpJump) vpTab = 0; ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("In the game", nullptr, g_meVpJump && vpTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { if (!g_meVpJump) vpTab = 1; ImGui::EndTabItem(); }
                g_meVpJump = false;
                ImGui::EndTabBar();
            }
        } else vpTab = 0;
        if (vpTab == 0) MsViewportPanel(); else MeGamePreview();
        ImGui::EndChild();
        ImGui::InvisibleButton("##vpsplit", ImVec2(rightW, Px(6)));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) g_meSplitR = std::clamp(g_meSplitR + io.MouseDelta.y / std::max(1.f, H), 0.2f, 0.8f);
        ImGui::BeginChild("##medet", ImVec2(rightW, H - vpH - Px(6) - ImGui::GetStyle().ItemSpacing.y * 2));
        SectionLabel("Details");
        MeDetails();
        ImGui::EndChild();
        ImGui::EndGroup();
    }
    ImGui::PushFont(F_small);
    if (asset) EdLabel(("Material asset: " + W2U(M.asset) + (M.dirty ? "  (not saved)" : "") + (g_meInst.empty() ? "" : "   \xC2\xB7   instance: " + W2U(g_meInst.substr(g_meInst.find_last_of(L"\\/") + 1)))).c_str());
    else if (!M.model.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.accent);
        ImGui::TextUnformatted((ICON_BOX_OPEN "  Imported model " + M.model + ": changes are also saved into the model, so every copy gets them, in this game or another.").c_str());
        ImGui::PopStyleColor();
    } else
        EdLabel("The material's own settings change in the game straight away. The Unreal inputs (Base Color, Normal...) are compiled for the preview and baked into textures for the game. All of it is saved with the level's layout.");
    ImGui::PopFont();
    if (M.picker) { if (!ImGui::IsPopupOpen("##mepick")) ImGui::OpenPopup("##mepick"); MePickerPopup(); }
    MeTexLibraryPopup();
}
