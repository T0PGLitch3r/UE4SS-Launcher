// Talks to the in-game LauncherBridge Lua mod through files in %TEMP%\UE4SS-LauncherBridge.
#include "bridge.h"
#include "models.h"
#include <algorithm>
#include <fstream>
#include <sstream>

static int g_nextId = (int)(GetTickCount() % 100000) + 1;
static void AddEntry(std::vector<LevelEntry>& out, const std::string& path);
static std::string ToPackagePath(std::string full);

std::wstring BridgeDir() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring d = tmp; if (!d.empty() && d.back() == L'\\') d.pop_back();
    return d + L"\\UE4SS-LauncherBridge";
}
static std::wstring CacheDir() { return ModsDir() + L"\\LauncherBridge\\cache"; }

static bool ReadAll(const std::wstring& p, std::string& out) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(f, nullptr), rd = 0;
    out.assign(sz, '\0'); if (sz) ReadFile(f, &out[0], sz, &rd, nullptr);
    out.resize(rd);
    CloseHandle(f);
    return true;
}
static bool WriteAtomic(const std::wstring& p, const std::string& data) {
    std::wstring tmp = p + L".launcher.tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0; WriteFile(f, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(f);
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
static ULONGLONG FileAgeMs(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return ~0ull;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = fa.ftLastWriteTime.dwLowDateTime; a.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (b.QuadPart - a.QuadPart) / 10000 : 0;
}

void BridgeInit() {
    MakeDirs(BridgeDir());
    // Make sure the bridge mod is switched on (older mods.txt files won't list it).
    if (DirExists(ModsDir() + L"\\LauncherBridge")) {
        bool listed = false;
        for (auto& m : ReadMods()) if (Lower(m.name) == L"launcherbridge") listed = m.enabled;
        if (!listed) SetModEnabled(L"LauncherBridge", true);
    }
}

void BridgeReset() {
    for (auto f : { L"\\command.txt", L"\\result.txt", L"\\status.txt" }) DeleteFileW((BridgeDir() + f).c_str());
}

BridgeStatus GetBridgeStatus() {
    BridgeStatus s;
    std::wstring p = BridgeDir() + L"\\status.txt";
    if (FileAgeMs(p) > 4000) return s;
    std::string d;
    if (!ReadAll(p, d)) return s;
    s.connected = true;
    auto field = [&](const char* k) -> std::string {
        std::string key = std::string("\n") + k + "=";
        std::string dd = "\n" + d;
        size_t i = dd.find(key);
        if (i == std::string::npos) return "";
        i += key.size();
        size_t e = dd.find_first_of("\r\n", i);
        return dd.substr(i, e == std::string::npos ? std::string::npos : e - i);
    };
    s.currentMap = field("map");
    if (s.currentMap == "unknown") s.currentMap.clear();
    s.freecam = field("freecam") == "on";
    s.fcMode = field("fcmode"); s.cam = field("cam"); s.rot = field("rot"); s.fcKey = field("fckey");
    s.fcSpeed = (float)atof(field("fcspeed").c_str());
    return s;
}

// Commands are queued and written as one file (one command per line). The game removes the file once it has
// read it, and we only write again after that, so nothing gets overwritten. Rapid edits of the same thing
// (slider drags) are merged so only the latest value is sent.
static std::vector<std::pair<std::string, std::string>> g_queue;   // (merge key, line)

void BridgeFlush() {
    if (g_queue.empty()) return;
    std::wstring cf = BridgeDir() + L"\\command.txt";
    if (GetFileAttributesW(cf.c_str()) != INVALID_FILE_ATTRIBUTES) return;   // game hasn't picked up the last batch yet
    std::string all;
    for (auto& q : g_queue) all += q.second + "\n";
    MakeDirs(BridgeDir());
    if (WriteAtomic(cf, all)) g_queue.clear();
}

int BridgeSend(const std::string& verb, const std::string& arg) {
    int id = g_nextId++;
    if (verb.rfind("ed_", 0) != 0) DeleteFileW((BridgeDir() + L"\\result.txt").c_str());
    std::string key;
    // how many leading tab-separated fields identify "the same edit"
    static const struct { const char* verb; int fields; } mergeable[] = {
        { "ed_set", 2 }, { "ed_xform", 1 }, { "ed_rot", 1 }, { "ed_pset", 1 }, { "ed_timescale", 0 }, { "ed_fov", 0 }, { "fcspeed", 0 } };
    for (auto& m : mergeable) if (verb == m.verb) {
        size_t end = 0;
        for (int i = 0; i < m.fields && end != std::string::npos; i++) end = arg.find('\t', end ? end + 1 : 0);
        key = verb + "|" + (m.fields == 0 ? "" : arg.substr(0, end));
        break;
    }
    std::string line = std::to_string(id) + "|" + verb + "|" + arg;
    bool merged = false;
    if (!key.empty()) for (auto& q : g_queue) if (q.first == key) { q.second = line; merged = true; break; }
    if (!merged) g_queue.push_back({ key, line });
    BridgeFlush();
    return id;
}

// ---------------------------------------------------------------- editor files
static bool Changed(const std::wstring& p, ULONGLONG& stamp) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return false;
    ULONGLONG t = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    t ^= fa.nFileSizeLow;
    if (t == stamp) return false;
    stamp = t;
    return true;
}
static void Floats(const std::string& s, float* out, int n) {
    for (int i = 0; i < n; i++) out[i] = 0;
    if (n == 3) sscanf(s.c_str(), "%f,%f,%f", &out[0], &out[1], &out[2]);
}
static std::vector<std::string> Tabs(const std::string& s) {
    std::vector<std::string> v; size_t a = 0;
    while (true) { size_t b = s.find('\t', a); v.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
    return v;
}

bool ReadEdCam(EdCam& c) {
    std::string d;
    if (!ReadAll(BridgeDir() + L"\\cam.txt", d)) return false;
    EdCam n;
    if (sscanf(d.c_str(), "%d %lf %f %f %f %f %f %f %f %d %d", &n.seq, &n.t, &n.loc[0], &n.loc[1], &n.loc[2],
               &n.rot[0], &n.rot[1], &n.rot[2], &n.fov, &n.w, &n.h) != 11) return false;
    if (n.seq == c.seq || n.w <= 0 || n.h <= 0) return false;
    c = n;
    return true;
}

bool ReadEdState(EdState& st) {
    static ULONGLONG stamp = 0;
    std::wstring p = BridgeDir() + L"\\editor_state.txt";
    if (!Changed(p, stamp)) return false;
    std::string d; if (!ReadAll(p, d)) return false;
    EdState n; n.valid = true;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('='); if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "pawn") n.pawn = v;
        else if (k == "loc") Floats(v, n.loc, 3);
        else if (k == "rot") Floats(v, n.rot, 3);
        else if (k == "walk") { n.walk = (float)atof(v.c_str()); n.hasMove = true; }
        else if (k == "jump") n.jump = (float)atof(v.c_str());
        else if (k == "gravity") n.gravity = (float)atof(v.c_str());
        else if (k == "mode") n.mode = atoi(v.c_str());
        else if (k == "god") n.god = v == "1";
        else if (k == "ghost") n.ghost = v == "1";
        else if (k == "timescale") n.timescale = (float)atof(v.c_str());
        else if (k == "fov") n.fov = (float)atof(v.c_str());
        else if (k == "sel") n.sel = v;
        else if (k == "selname") n.selName = v;
        else if (k == "selclass") n.selClass = v;
        else if (k == "selloc") Floats(v, n.sloc, 3);
        else if (k == "selrot") Floats(v, n.srot, 3);
        else if (k == "selscale") Floats(v, n.sscale, 3);
        else if (k == "selhidden") n.shidden = v == "1";
        else if (k == "selscreen") n.hasScreen = sscanf(v.c_str(), "%f,%f", &n.sscreen[0], &n.sscreen[1]) == 2;
        else if (k == "selcenter") { Floats(v, n.scenter, 3); n.hasCenter = true; }
        else if (k == "swapped") n.swapped = v;
        else if (k == "added") { auto t = Tabs(v); if (t.size() >= 2) n.added.push_back({ t[0], t[1] }); }
        else if (k == "playingas") n.playingAs = v;
        else if (k == "lookingas") n.lookingAs = v;
        else if (k == "undo") { auto t = Tabs(v); n.undoCount = t.empty() ? 0 : atoi(t[0].c_str()); n.undoWhat = t.size() > 1 ? t[1] : ""; }
        else if (k == "layout") { auto t = Tabs(v); n.hasLayout = true; n.layoutCount = t.empty() ? 0 : atoi(t[0].c_str()); n.layoutOn = t.size() < 2 || t[1] == "1"; n.layoutGame = t.size() > 2 ? t[2] : ""; n.layoutMap = t.size() > 3 ? t[3] : ""; }
        else if (k == "selcam") { float* c = n.selcam; n.hasSelCam = sscanf(v.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7], &c[8]) == 9; }
        else if (k == "gizmo") n.hasGizmo = sscanf(v.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f", &n.gz[0], &n.gz[1], &n.gz[2], &n.gz[3], &n.gz[4], &n.gz[5], &n.gz[6], &n.gz[7], &n.gzLen) == 9;
        else if (k == "msg") { auto t = Tabs(v); if (t.size() >= 3) n.msgs.push_back({ atoi(t[0].c_str()), t[1] == "err", t[2] }); }
    }
    st = n;
    return true;
}

std::string g_edSelMesh, g_edSelMeshKind, g_edSelActorXf; std::vector<std::string> g_edSelParts;
bool ReadEdProps(std::vector<EdProp>& out, std::string& forPath) {
    static ULONGLONG stamp = 0;
    std::wstring p = BridgeDir() + L"\\props.txt";
    if (!Changed(p, stamp)) return false;
    std::string d; if (!ReadAll(p, d)) return false;
    out.clear(); forPath.clear(); g_edSelMesh.clear(); g_edSelMeshKind.clear(); g_edSelParts.clear(); g_edSelActorXf.clear();
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("for=", 0) == 0) forPath = line.substr(4);
        else if (line.rfind("mesh=", 0) == 0) { auto t = Tabs(line.substr(5)); if (t.size() >= 2) { g_edSelMeshKind = t[0]; g_edSelMesh = t[1]; } }
        else if (line.rfind("meshp=", 0) == 0) g_edSelParts.push_back(line.substr(6));
        else if (line.rfind("actorxf=", 0) == 0) g_edSelActorXf = line.substr(8);
        else if (line.rfind("p=", 0) == 0) { auto t = Tabs(line.substr(2)); if (t.size() >= 4) out.push_back({ t[0], t[1], t[2], t[3] }); }
    }
    return true;
}

bool ReadEdActors(std::vector<EdActor>& out, int& total) {
    static ULONGLONG stamp = 0;
    std::wstring p = BridgeDir() + L"\\actors.txt";
    if (!Changed(p, stamp)) return false;
    std::string d; if (!ReadAll(p, d)) return false;
    out.clear(); total = 0;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("total=", 0) == 0) total = atoi(line.c_str() + 6);
        else if (line.rfind("a=", 0) == 0) { auto t = Tabs(line.substr(2)); if (t.size() >= 4) out.push_back({ t[0], t[1], t[2], (float)atof(t[3].c_str()) }); }
    }
    return true;
}

bool ReadEdNpcs(std::vector<EdActor>& out, int& total) {
    static ULONGLONG stamp = 0;
    std::wstring p = BridgeDir() + L"\\npcs.txt";
    if (!Changed(p, stamp)) return false;
    std::string d; if (!ReadAll(p, d)) return false;
    out.clear(); total = 0;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("total=", 0) == 0) total = atoi(line.c_str() + 6);
        else if (line.rfind("a=", 0) == 0) { auto t = Tabs(line.substr(2)); if (t.size() >= 4) out.push_back({ t[0], t[1], t[2], (float)atof(t[3].c_str()) }); }
    }
    return true;
}

void EdClearFiles() {
    for (auto f : { L"\\editor_state.txt", L"\\props.txt", L"\\actors.txt", L"\\cam.txt", L"\\models.txt", L"\\npcs.txt" }) DeleteFileW((BridgeDir() + f).c_str());
}

bool BridgeResult(int id, bool& ok, std::string& msg) {
    std::string d;
    if (!ReadAll(BridgeDir() + L"\\result.txt", d) || d.empty()) return false;
    size_t a = d.find('|'); if (a == std::string::npos) return false;
    if (atoi(d.substr(0, a).c_str()) != id) return false;
    size_t b = d.find('|', a + 1);
    ok = d.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1) == "ok";
    msg = b == std::string::npos ? "" : d.substr(b + 1);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return true;
}

static std::wstring CacheFile() {
    std::wstring g = GameDisplayName();
    if (g.empty()) return L"";
    for (auto& c : g) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    return CacheDir() + L"\\" + g + L".txt";
}

// After a successful scan, keep a copy for this game so the list shows even when it isn't running.
void BridgeCacheLevels() {
    std::wstring cf = CacheFile();
    if (cf.empty()) return;
    std::string d;
    if (!ReadAll(BridgeDir() + L"\\levels.txt", d)) return;
    MakeDirs(CacheDir());
    // games that can only report the levels loaded right now: keep the ones seen before too (and any added by hand)
    std::string old;
    if (d.find("#method loaded worlds") != std::string::npos && ReadAll(cf, old)) {
        std::string add;
        std::istringstream ss(old); std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            bool have = false;
            std::istringstream ns(d); std::string l2;
            while (!have && std::getline(ns, l2)) { if (!l2.empty() && l2.back() == '\r') l2.pop_back(); have = _stricmp(l2.c_str(), line.c_str()) == 0; }
            if (!have) add += line + "\n";
        }
        if (!d.empty() && d.back() != '\n') d += "\n";
        d += add;
    }
    WriteAtomic(cf, d);
}

static void ParseLevels(const std::string& data, std::vector<LevelEntry>& out, std::string& method) {
    std::istringstream ss(data);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') { if (line.rfind("#method ", 0) == 0) method = line.substr(8); continue; }
        AddEntry(out, line);
    }
}

// Loose .umap files (only for games that ship unpacked content)
static void ScanUmaps(const std::wstring& dir, const std::wstring& prefix, std::vector<LevelEntry>& out, int depth = 0) {
    if (depth > 12 || out.size() > 5000) return;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring n = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (Lower(n) != L"paks") ScanUmaps(dir + L"\\" + n, prefix + L"/" + n, out, depth + 1); }
        else if (ExtOf(n) == L".umap") {
            AddEntry(out, W2U(prefix) + "/" + W2U(StemOf(n)));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ================================================================ .pak index reader
// Lists *.umap files straight from the game's .pak files (no injection, nothing runs in the game).
// Supports pak versions 3-11 with an unencrypted index; UE5 IoStore-only games fall back to the in-game scan.
struct PakFile {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~PakFile() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool Read(long long off, void* buf, DWORD n) {
        LARGE_INTEGER li; li.QuadPart = off;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
        DWORD rd = 0; return ReadFile(h, buf, n, &rd, nullptr) && rd == n;
    }
};
struct Buf {
    const std::string& b; size_t o = 0; bool bad = false;
    explicit Buf(const std::string& s) : b(s) {}
    template <class T> T get() { T v{}; if (o + sizeof(T) > b.size()) { bad = true; return v; } memcpy(&v, b.data() + o, sizeof(T)); o += sizeof(T); return v; }
    void skip(size_t n) { if (o + n > b.size()) bad = true; else o += n; }
    std::string str() {
        int n = get<int>(); if (bad) return "";
        if (n == 0) return "";
        if (n > 0) { if (n > 4096 || o + n > b.size()) { bad = true; return ""; } std::string r(b.data() + o, n - 1); o += n; return r; }
        n = -n; if (n > 4096 || o + 2ull * n > b.size()) { bad = true; return ""; }
        std::wstring w((const wchar_t*)(b.data() + o), n - 1); o += 2ull * n; return W2U(w);
    }
};

// "../../../Hk_project/Content/Map/X.umap" -> "/Game/Map/X"
static std::string ToPackagePath(std::string full) {
    while (full.rfind("../", 0) == 0) full = full.substr(3);
    size_t dot = full.find_last_of('.'), sl = full.find_last_of('/');
    if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) return "";
    full = full.substr(0, dot);
    if (full.rfind("Engine/Content/", 0) == 0) return "/Engine/" + full.substr(15);
    size_t pl = full.find("/Plugins/");
    if (pl != std::string::npos) {           // .../Plugins/[...]/<Name>/Content/...
        size_t c = full.find("/Content/", pl);
        if (c == std::string::npos) return "";
        size_t ns = full.rfind('/', c - 1);
        return "/" + full.substr(ns + 1, c - ns - 1) + "/" + full.substr(c + 9);
    }
    size_t c = full.find("/Content/");
    if (c == std::string::npos) return "";
    return "/Game/" + full.substr(c + 9);
}

typedef bool (*PakFilter)(const std::string& fileName);
static bool IsUmap(const std::string& fn) { return fn.size() > 5 && _stricmp(fn.c_str() + fn.size() - 5, ".umap") == 0; }
static bool ReadPakFiles(const std::wstring& path, PakFilter keep, std::vector<std::string>& maps, std::string& note) {
    PakFile f;
    f.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(f.h, &sz);
    if (sz.QuadPart < 64) return false;
    // footer: find magic in the last 512 bytes
    long long tailOff = std::max(0LL, sz.QuadPart - 512);
    std::string tail((size_t)(sz.QuadPart - tailOff), '\0');
    if (!f.Read(tailOff, &tail[0], (DWORD)tail.size())) return false;
    const uint32_t magic = 0x5A6F12E1;
    long long m = -1;
    for (long long i = (long long)tail.size() - 4; i >= 0; i--) if (memcmp(tail.data() + i, &magic, 4) == 0) {
        // magic must be followed by version, index offset/size that fit in the file
        if (i + 24 > (long long)tail.size()) continue;
        uint32_t v; long long io, is; memcpy(&v, tail.data() + i + 4, 4); memcpy(&io, tail.data() + i + 8, 8); memcpy(&is, tail.data() + i + 16, 8);
        if (v >= 1 && v <= 20 && io > 0 && is > 0 && io + is <= sz.QuadPart) { m = i; break; }
    }
    if (m < 0) { note = "unrecognised pak format"; return false; }
    uint32_t ver; long long idxOff, idxSize;
    memcpy(&ver, tail.data() + m + 4, 4); memcpy(&idxOff, tail.data() + m + 8, 8); memcpy(&idxSize, tail.data() + m + 16, 8);
    bool encIndex = ver >= 4 && m >= 1 && tail[(size_t)m - 1] != 0;
    if (encIndex) { note = "index is encrypted"; return false; }
    if (idxSize > 256ll * 1024 * 1024) return false;
    std::string idx((size_t)idxSize, '\0');
    if (!f.Read(idxOff, &idx[0], (DWORD)idxSize)) return false;
    Buf b(idx);
    std::string mount = b.str();
    int num = b.get<int>();
    if (b.bad || num < 0) return false;

    auto add = [&](const std::string& full) { std::string pp = ToPackagePath(full); if (!pp.empty()) maps.push_back(pp); };
    if (ver >= 10) {
        b.get<unsigned long long>();                       // path hash seed
        if (b.get<uint32_t>()) b.skip(8 + 8 + 20);         // path hash index
        if (!b.get<uint32_t>()) { note = "pak has no directory index"; return false; }
        long long fdOff = b.get<long long>(), fdSize = b.get<long long>();
        if (b.bad || fdOff <= 0 || fdSize <= 0 || fdOff + fdSize > sz.QuadPart || fdSize > 512ll * 1024 * 1024) return false;
        std::string fd((size_t)fdSize, '\0');
        if (!f.Read(fdOff, &fd[0], (DWORD)fdSize)) return false;
        Buf d(fd);
        int dirs = d.get<int>();
        for (int i = 0; i < dirs && !d.bad; i++) {
            std::string dir = d.str();
            int files = d.get<int>();
            for (int j = 0; j < files && !d.bad; j++) {
                std::string fn = d.str(); d.get<int>();
                if (keep(fn)) add(mount + dir + fn);
            }
        }
        return !d.bad;
    }
    // v3-v9: filename followed by a serialized FPakEntry
    for (int i = 0; i < num && !b.bad; i++) {
        std::string fn = b.str();
        b.skip(8 + 8 + 8);                                 // offset, size, uncompressed size
        uint32_t comp = b.get<uint32_t>();                 // compression method (index in v8+)
        if (ver == 1) b.skip(8);                           // timestamp
        b.skip(20);                                        // SHA1
        if (ver >= 3) {
            if (comp != 0) { int blocks = b.get<int>(); if (blocks < 0 || blocks > 1000000) { b.bad = true; break; } b.skip(16ull * blocks); }
            b.skip(1 + 4);                                 // flags, compression block size
        }
        if (keep(fn.substr(fn.find_last_of('/') + 1))) add(mount + fn);
    }
    return !b.bad;
}

// All levels found in the game's .pak files (cached per pak set until the files change)
static std::vector<std::string> PakLevels(std::string& note) {
    static std::wstring cachedKey; static std::vector<std::string> cached; static std::string cachedNote;
    std::wstring paks = PaksDir();
    std::vector<std::string> maps;
    if (paks.empty()) return maps;
    std::vector<std::wstring> files;
    std::wstring key = paks;
    auto collect = [&](const std::wstring& dir) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*.pak").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            files.push_back(dir + L"\\" + fd.cFileName);
            key += L"|" + std::wstring(fd.cFileName) + L":" + std::to_wstring(fd.nFileSizeLow) + L":" + std::to_wstring(fd.ftLastWriteTime.dwLowDateTime);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    collect(paks); collect(paks + L"\\~mods"); collect(paks + L"\\LogicMods");
    if (key == cachedKey) { note = cachedNote; return cached; }
    int okCount = 0, enc = 0;
    for (auto& f : files) {
        std::string n;
        if (ReadPakFiles(f, IsUmap, maps, n)) okCount++;
        else if (n == "index is encrypted") enc++;
    }
    std::sort(maps.begin(), maps.end());
    maps.erase(std::unique(maps.begin(), maps.end()), maps.end());
    note = enc && maps.empty() ? "the game's files are encrypted" : "";
    cachedKey = key; cached = maps; cachedNote = note;
    return maps;
}

// Model-looking assets in the game's .pak files. A .pak only lists file names (not what each asset is),
// so this goes by the usual naming: SM_ prefix, or a mesh/prop/model folder, minus obvious non-models.
static bool IsModelName(const std::string& fn) {
    if (fn.size() < 8 || _stricmp(fn.c_str() + fn.size() - 7, ".uasset") != 0) return false;
    std::string u; for (char c : fn.substr(0, fn.size() - 7)) u += (char)toupper((unsigned char)c);
    if (u.rfind("SM_", 0) == 0 || u.rfind("S_", 0) == 0 || u.rfind("MESH_", 0) == 0) return true;
    static const char* notPre[] = { "M_", "MI_", "MF_", "MM_", "T_", "TX_", "BP_", "ABP_", "SK_", "SKM_", "SKEL_", "PHYS_", "PA_", "AM_", "AS_", "BS_",
        "PS_", "P_", "NS_", "FX_", "VFX_", "SFX_", "SC_", "SW_", "A_", "W_", "WBP_", "UI_", "DA_", "DT_", "CT_", "CV_", "E_", "F_", "ST_", "MPC_", "RT_",
        "LUT_", "TEX_", "MAT_", "ANIM_", "SEQ_", "LS_", "SND_", "COL_", "UCX_", "UBX_", "USP_", "ATLAS" };
    for (auto x : notPre) if (u.rfind(x, 0) == 0) return false;
    static const char* notSuf[] = { "_PHYSICSASSET", "_SKELETON", "_BUILTDATA", "_MAT", "_MATERIAL", "_TEX", "_ANIM", "_BP", "_CUE",
        "_D", "_N", "_NRM", "_NORMAL", "_BC", "_BASECOLOR", "_ALBEDO", "_ORM", "_MRA", "_RMA", "_R", "_M", "_AO", "_E", "_MASK", "_MASKS", "_H", "_HEIGHT",
        "_DIFFUSE", "_EMISSIVE", "_ROUGHNESS", "_METALLIC", "_SPEC", "_A", "_S", "_O", "_ALPHA", "_OPACITY", "_MI", "_INST", "_INSTANCE",
        "_COLOR", "_PHYSICS", "_CUSTOMCOLLISION", "_COLLISION", "_ATLAS", "_LUT", "_CURVE" };
    for (auto x : notSuf) { size_t l = strlen(x); if (u.size() > l && u.compare(u.size() - l, l, x) == 0) return false; }
    // texture-size endings like _2K or _512X512
    size_t us = u.find_last_of('_');
    if (us != std::string::npos && us + 1 < u.size()) {
        std::string t = u.substr(us + 1);
        bool digitsK = t.size() >= 2 && t.back() == 'K' && t.find_first_not_of("0123456789") == t.size() - 1;
        size_t x = t.find('X');
        bool dims = x != std::string::npos && x > 0 && x + 1 < t.size() && t.find_first_not_of("0123456789X") == std::string::npos;
        if (digitsK || dims) return false;
    }
    return true;   // the folder decides (below)
}
static bool LooksLikeModelPath(const std::string& pp) {
    std::string name = pp.substr(pp.find_last_of('/') + 1), u;
    for (char c : name) u += (char)toupper((unsigned char)c);
    if (u.rfind("SM_", 0) == 0 || u.rfind("S_", 0) == 0 || u.rfind("MESH_", 0) == 0) return true;
    std::string l; for (char c : pp) l += (char)tolower((unsigned char)c);
    for (auto x : { "/sound/", "/audio/", "/animation/", "/anims", "/textures/", "/texture/", "/materials/", "/material/", "/gui/", "/ui/", "/dialogue/",
                    "/fx/", "/vfx/", "/particles/", "/blueprints/", "/sequences/", "/cinematics/", "/maps/", "/map/", "/decal", "/font", "/localization",
                    "/movies", "/collisions/", "/collision/", "/poster" })
        if (l.find(x) != std::string::npos) return false;
    for (auto x : { "/mesh", "/meshes/", "/staticmesh", "/props/", "/prop/", "/props_", "/models/", "/model/", "/geometry/", "/geo/", "/archi/",
                    "/architecture/", "/vegetation/", "/foliage/", "/vehicle", "/background/", "/building", "/furniture/", "/environment/", "/env/", "/pipe" })
        if (l.find(x) != std::string::npos) return true;
    return false;
}
std::vector<std::string> PakModels(std::string& note) {
    static std::wstring cachedKey; static std::vector<std::string> cached; static std::string cachedNote;
    std::wstring paks = PaksDir();
    std::vector<std::string> all;
    if (paks.empty()) { note = "no game selected"; return all; }
    std::vector<std::wstring> files; std::wstring key = paks;
    auto collect = [&](const std::wstring& dir) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*.pak").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do { files.push_back(dir + L"\\" + fd.cFileName); key += L"|" + std::wstring(fd.cFileName) + L":" + std::to_wstring(fd.nFileSizeLow); } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    collect(paks); collect(paks + L"\\~mods"); collect(paks + L"\\LogicMods");
    if (key == cachedKey) { note = cachedNote; return cached; }
    int enc = 0;
    for (auto& f : files) { std::string n; if (!ReadPakFiles(f, IsModelName, all, n) && n == "index is encrypted") enc++; }
    std::vector<std::string> out;
    for (auto& p : all) if (LooksLikeModelPath(p)) out.push_back(p);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    note = out.empty() ? (enc ? "the game's files are encrypted" : files.empty() ? "no .pak files found" : "no model files recognised by name") : "";
    cachedKey = key; cached = out; cachedNote = note;
    return out;
}

// Developer / debug / cheat menus left in the game's files (widgets, going by their names)
static bool IsDevMenuName(const std::string& fn) {
    if (fn.size() < 8 || _stricmp(fn.c_str() + fn.size() - 7, ".uasset") != 0) return false;
    std::string u; for (char c : fn.substr(0, fn.size() - 7)) u += (char)toupper((unsigned char)c);
    bool widget = false;
    for (auto pre : { "W_", "WBP_", "WB_", "UI_", "UMG_", "WD_", "BPW_", "HUD_" }) if (u.rfind(pre, 0) == 0) widget = true;
    for (auto x : { "WIDGET", "MENU", "PANEL", "HUD" }) if (u.find(x) != std::string::npos) widget = true;
    if (!widget) return false;
    for (auto x : { "DEBUG", "CHEAT", "DEVMENU", "DEV_MENU", "DEVELOPER", "DEVTOOL", "DEV_TOOL", "DEVPANEL", "DEV_PANEL", "ADMIN", "GODMODE", "TESTMENU", "TEST_MENU", "QAMENU", "QA_MENU" })
        if (u.find(x) != std::string::npos) return true;
    return u.rfind("W_DEV", 0) == 0 || u.rfind("WBP_DEV", 0) == 0 || u.rfind("UI_DEV", 0) == 0;
}
// Cheat manager blueprints left in the game's files (BP_CheatManager, B_MyGameCheats...)
static bool IsCheatManagerName(const std::string& fn) {
    if (fn.size() < 8 || _stricmp(fn.c_str() + fn.size() - 7, ".uasset") != 0) return false;
    std::string u; for (char c : fn.substr(0, fn.size() - 7)) u += (char)toupper((unsigned char)c);
    if (u.find("CHEATMANAGER") != std::string::npos || u.find("CHEAT_MANAGER") != std::string::npos) return true;
    bool bp = u.rfind("BP_", 0) == 0 || u.rfind("B_", 0) == 0;
    return bp && (u.find("CHEATS") != std::string::npos || u.find("DEBUGMANAGER") != std::string::npos);
}
std::vector<std::string> PakDevMenus(std::string& note, std::vector<std::string>* cheatMgrs) {
    std::wstring paks = PaksDir();
    std::vector<std::string> out;
    if (paks.empty()) { note = "no game selected"; return out; }
    // all the game's files, packed or IoStore (with the game's key when it has one)
    std::vector<std::string> all; std::string n2;
    if (ModelListGameFiles(paks, [](const std::string& fn) { return IsDevMenuName(fn) || IsCheatManagerName(fn); }, all, n2)) {
        // pieces of a menu (its buttons, toggles, rows...) aren't menus of their own: leave them out
        auto piece = [](const std::string& k) {
            std::string u; for (char c : k) u += (char)toupper((unsigned char)c);
            if (u.find("/WIDGETS/") != std::string::npos || u.find("/COMPONENTS/") != std::string::npos) return true;
            std::string n = u.substr(u.find_last_of('/') + 1);
            for (auto x : { "BUTTON", "TOGGLE", "ENTRY", "LABEL", "_ITEM", "ITEMCATEGORY", "STATUS", "BACK.", "GREY", "_ROW", "ROW.", "SLOT", "ICON", "CHECKBOX", "SLIDER", "TOOLTIP", "_TAB", "LISTITEM", "HEADER" })
                if (n.find(x) != std::string::npos) return true;
            return false;
        };
        for (auto& k : all) {
            std::string fn = k.substr(k.find_last_of('/') + 1);
            if (IsDevMenuName(fn) && !piece(k)) out.push_back(k);
            if (cheatMgrs && IsCheatManagerName(fn)) cheatMgrs->push_back(k);
        }
        auto rank = [](const std::string& k) {
            std::string n = k.substr(k.find_last_of('/') + 1);
            if (n.find("debugmainmenu") != std::string::npos || n.rfind("debugmenu.", 0) == 0 || n.find("devmenu.") != std::string::npos || n.find("cheatmenu.") != std::string::npos) return 0;
            return 1;
        };
        std::stable_sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
        note = out.empty() ? (n2.empty() ? "no developer menus found in the game's files" : n2) : "";
        return out;
    }
    std::vector<std::wstring> files;
    auto collect = [&](const std::wstring& dir) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*.pak").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do files.push_back(dir + L"\\" + fd.cFileName); while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    collect(paks); collect(paks + L"\\~mods"); collect(paks + L"\\LogicMods");
    int enc = 0;
    for (auto& f : files) { std::string n; if (!ReadPakFiles(f, IsDevMenuName, out, n) && n == "index is encrypted") enc++; }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    note = out.empty() ? (enc ? "the game's files are encrypted, so its menus can't be listed" : "no developer menus found in the game's files") : "";
    return out;
}

// Character blueprints in the game's files, going by the usual naming (BP_ + a character-ish folder or name)
static bool IsBlueprintName(const std::string& fn) {
    if (fn.size() < 8 || _stricmp(fn.c_str() + fn.size() - 7, ".uasset") != 0) return false;
    std::string u; for (char c : fn) u += (char)toupper((unsigned char)c);
    return u.rfind("BP_", 0) == 0 || u.rfind("B_", 0) == 0 || u.rfind("CH_", 0) == 0 || u.rfind("CHR_", 0) == 0 || u.rfind("NPC_", 0) == 0;
}
std::vector<std::string> PakCharacters(std::string& note) {
    std::wstring paks = PaksDir();
    std::vector<std::string> all, out;
    if (paks.empty()) { note = "no game selected"; return out; }
    std::vector<std::wstring> files;
    auto collect = [&](const std::wstring& dir) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*.pak").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do files.push_back(dir + L"\\" + fd.cFileName); while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    collect(paks); collect(paks + L"\\~mods"); collect(paks + L"\\LogicMods");
    int enc = 0;
    for (auto& f : files) { std::string n; if (!ReadPakFiles(f, IsBlueprintName, all, n) && n == "index is encrypted") enc++; }
    for (auto& p : all) {
        std::string l; for (char c : p) l += (char)tolower((unsigned char)c);
        std::string name = l.substr(l.find_last_of('/') + 1);
        bool folder = false, named = false, bad = false;
        for (auto x : { "/character", "/characters/", "/npc", "/npcs/", "/enemies/", "/enemy/", "/creatures/", "/creature/", "/animals/", "/animal/", "/pawns/", "/pawn/", "/ai/", "/mobs/", "/monsters/", "/humans/", "/robots/" })
            if (l.find(x) != std::string::npos) folder = true;
        for (auto x : { "character", "npc", "enemy", "creature", "animal", "pawn", "player", "human", "robot", "monster", "zombie", "soldier", "guard", "boss", "mob" })
            if (name.find(x) != std::string::npos) named = true;
        for (auto x : { "anim", "abp_", "controller", "_ai", "aic_", "bt_", "bb_", "task", "service", "decorator", "eqs", "spawner", "spawnpoint", "manager", "component",
                        "widget", "gamemode", "gamestate", "playerstate", "camera", "cheat", "save", "interface", "_enum", "struct", "_dt", "datatable", "notify", "projectile",
                        "weapon", "pickup", "trigger", "volume", "sound", "_fx", "effect", "damage", "ability", "_ge_", "_ga_", "hud", "input", "settings", "_lib", "library", "macro" })
            if (name.find(x) != std::string::npos) bad = true;
        if ((folder || named) && !bad) out.push_back(p.substr(0, p.size() - 7));   // package path without .uasset
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    note = out.empty() ? (enc ? "the game's files are encrypted" : "no character blueprints recognised in the game's files") : "";
    return out;
}

bool ReadEdSwap(std::vector<EdSwapModel>& out, int& total) {
    static ULONGLONG stamp = 0;
    std::wstring p = BridgeDir() + L"\\swaplist.txt";
    if (!Changed(p, stamp)) return false;
    std::string d; if (!ReadAll(p, d)) return false;
    out.clear(); total = 0;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("total=", 0) == 0) total = atoi(line.c_str() + 6);
        else if (line.rfind("s=", 0) == 0) { auto t = Tabs(line.substr(2)); if (t.size() >= 3) out.push_back({ t[0], t[1], t[2], t.size() > 3 ? atoi(t[3].c_str()) : 0 }); }
    }
    return true;
}

bool ReadEdCheats(EdCheats& out) {
    static ULONGLONG stamp = 0;
    std::wstring p = BridgeDir() + L"\\cheats.txt";
    if (!Changed(p, stamp)) return false;
    std::string d; if (!ReadAll(p, d)) return false;
    EdCheats c; c.scanned = true;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "cheatmanager=1") c.cheatMgr = true;
        else if (line == "console=1") c.console = true;
        else if (line.rfind("consolekeys=", 0) == 0) c.consoleKeys = line.substr(12);
        else if (line.rfind("view=", 0) == 0) c.view = line.substr(5);
        else if (line == "fog=0") c.fog = false;
        else if (line.rfind("cheat=", 0) == 0) { auto t = Tabs(line.substr(6)); if (!t.empty()) c.cheats.push_back({ t[0], t.size() > 1 ? t[1] : "", t.size() > 2 ? t[2] : "" }); }
        else if (line.rfind("menu=", 0) == 0) { auto t = Tabs(line.substr(5)); if (t.size() >= 2) c.menus.push_back({ t[0], t[1], t.size() > 2 && t[2] == "1" }); }
        else if (line.rfind("cheatclass=", 0) == 0) { auto t = Tabs(line.substr(11)); if (t.size() >= 2) c.cheatClasses.push_back({ t[0], t[1], t.size() > 2 && t[2] == "1" }); }
        else if (line.rfind("cheatcurrent=", 0) == 0) c.cheatCurrent = line.substr(13);
    }
    out = std::move(c);
    return true;
}

bool ReadEdModels(std::vector<EdModel>& out, int& total) {
    static ULONGLONG stamp = 0;
    std::wstring p = BridgeDir() + L"\\models.txt";
    if (!Changed(p, stamp)) return false;
    std::string d; if (!ReadAll(p, d)) return false;
    out.clear(); total = 0;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("total=", 0) == 0) total = atoi(line.c_str() + 6);
        else if (line.rfind("m=", 0) == 0) { auto t = Tabs(line.substr(2)); if (t.size() >= 2) out.push_back({ t[0], t[1] }); }
    }
    return true;
}

// Big games split one playable level into many streamed pieces (lighting, sound, layout...).
// Those pieces can't be opened on their own, so we recognise them by their usual name endings.
bool LooksLikeSubLevel(const std::string& path) {
    std::string n = path.substr(path.find_last_of('/') + 1), u;
    for (char c : n) u += (char)toupper((unsigned char)c);
    static const char* suf[] = { "_LD", "_GD", "_GRAPH", "_SOUND", "_SND", "_AUDIO", "_ZONE", "_LIGHTS", "_LIGHT", "_LIGHTING",
        "_COL", "_COLLISION", "_BKGD", "_BG", "_BACKGROUND", "_FEEDBACK", "_POST", "_POSTPROCESS", "_FORBAKINGONLY", "_TRANS",
        "_FX", "_VFX", "_ART", "_GEO", "_GEOMETRY", "_ENV", "_GAMEPLAY", "_GP", "_SCRIPT", "_SCRIPTING", "_BLOCKOUT", "_DECO",
        "_PROPS", "_NAV", "_NAVMESH", "_FOLIAGE", "_LANDSCAPE", "_DESIGN", "_LOGIC", "_STREAMING", "_SUB", "_SUBLEVEL", "_BUILTDATA",
        "_ARTTEMP", "_ART_STATELESS", "_ART_STATEFUL", "_ARTSTATELESS", "_ARTSTATEFUL", "_STATELESS", "_STATEFUL", "_SHARED",
        "_TECHART", "_VISTA", "_VISTAS", "_SE", "_VFX_SE", "_SCRIPTEDEVENTS", "_SCRIPTEDEVENT", "_QAPERF", "_ANIMALS", "_RAIN-ASSETS",
        "_ALSHARED", "_PREHOOKS", "_LIGHTS_DEFAULT", "_PERFVOLUMES", "_PERFVOLUMES_TEMP", "_AUDIO", "_SFX", "_MUSIC", "_CINEMATICS", "_CINE" };
    for (auto x : suf) { size_t l = strlen(x); if (u.size() > l && u.compare(u.size() - l, l, x) == 0) return true; }
    // lighting / art / design pieces with a number or tag after them (ZA_GLOBAL_LIGHTING_02, ZoneC_01_LightingTEMP_ART1, ZA_01_New_Design)
    for (auto x : { "LIGHTING", "_ART_", "_ARTSTATE", "_ARTTEMP", "_DESIGN", "_VFX_", "_SCRIPTEDEVENT", "_QAPERF" }) if (u.find(x) != std::string::npos) return true;
    for (auto pre : { "BACKGROUND_", "BG_", "SUB_", "SL_" }) if (u.rfind(pre, 0) == 0) return true;
    return false;
}

static void AddEntry(std::vector<LevelEntry>& out, const std::string& path) {
    LevelEntry e; e.path = path;
    size_t s = path.find_last_of('/');
    e.name = s == std::string::npos ? path : path.substr(s + 1);
    e.folder = s == std::string::npos ? "" : path.substr(0, s);
    e.sub = LooksLikeSubLevel(path);
    out.push_back(e);
}

std::string NormalizeLevelPath(std::string in) {
    // trim spaces and quotes, unify slashes
    while (!in.empty() && strchr(" \t\r\n\"'", in.back())) in.pop_back();
    size_t a = 0; while (a < in.size() && strchr(" \t\"'", in[a])) a++;
    in = in.substr(a);
    for (auto& c : in) if (c == '\\') c = '/';
    if (in.empty()) return in;
    auto endsI = [](const std::string& s, const char* x) { size_t l = strlen(x); return s.size() >= l && _stricmp(s.c_str() + s.size() - l, x) == 0; };
    // file path form: ../../../Proj/Content/Map/X.umap, Proj/Content/..., C:/Games/.../Content/...
    if (in.find("/Content/") != std::string::npos || in.rfind("Content/", 0) == 0) {
        std::string f = in.rfind("Content/", 0) == 0 ? "Game/" + in : in;
        if (!endsI(f, ".umap")) f += ".umap";
        std::string p = ToPackagePath(f);
        if (!p.empty()) return p;
    }
    if (endsI(in, ".umap")) in = in.substr(0, in.size() - 5);
    if (in[0] == '/') {
        size_t dot = in.find('.', in.find_last_of('/'));   // /Game/Maps/X.X -> /Game/Maps/X
        if (dot != std::string::npos) in = in.substr(0, dot);
    }
    return in;
}

LevelSource LoadLevels(std::vector<LevelEntry>& out, std::string& method) {
    out.clear(); method.clear();
    LevelSource src = LS_NONE;
    // 1) straight from the game's .pak files (safe; works without the game running)
    std::string note;
    for (auto& p : PakLevels(note)) AddEntry(out, p);
    if (!out.empty()) { src = LS_FILES; method = "pak files"; }
    // 2) loose .umap files (games that ship unpacked)
    if (out.empty()) {
        std::wstring paks = PaksDir();
        if (!paks.empty()) ScanUmaps(DirOf(paks), L"/Game", out);
        if (!out.empty()) { src = LS_FILES; method = "game folder"; }
    }
    // 3) merge in whatever the game itself reported (cached per game)
    std::string d; std::wstring cf = CacheFile();
    if (!cf.empty() && ReadAll(cf, d)) {
        std::vector<LevelEntry> game; std::string gm;
        ParseLevels(d, game, gm);
        size_t before = out.size();
        for (auto& g : game) {
            bool dup = false;
            for (size_t i = 0; i < before && !dup; i++) if (_stricmp(out[i].path.c_str(), g.path.c_str()) == 0) dup = true;
            if (!dup) out.push_back(g);
        }
        if (src == LS_NONE && !out.empty()) { src = LS_CACHE; method = gm; }
    }
    // 4) no game selected yet: show the connected game's latest scan directly
    if (out.empty() && cf.empty() && GetBridgeStatus().connected) {
        std::string ld;
        if (ReadAll(BridgeDir() + L"\\levels.txt", ld)) { std::string gm; ParseLevels(ld, out, gm); if (!out.empty()) { src = LS_CACHE; method = gm; } }
    }
    std::sort(out.begin(), out.end(), [](const LevelEntry& a, const LevelEntry& b) { return _stricmp(a.path.c_str(), b.path.c_str()) < 0; });
    if (out.empty() && !note.empty()) method = note;
    return src;
}

void WriteFreecamConfig(const FreecamConfig& c) {
    MakeDirs(BridgeDir());
    char buf[256];
    snprintf(buf, sizeof buf, "enabled=%d\nkey=%s\nmode=%s\nhidetext=%d\nspeed=%.2f\n", c.enabled ? 1 : 0, c.key.c_str(), c.mode.c_str(), c.hideText ? 1 : 0, c.speed);
    WriteAtomic(BridgeDir() + L"\\config.txt", buf);
}
