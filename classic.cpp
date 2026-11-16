// Unreal Engine 1-3 support: detection from package headers, .ini editing with backups, launching.
#include "core.h"
#include "classic.h"
#include <shlobj.h>
#include <map>
#include <mutex>
#include <algorithm>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------- small helpers
static std::string LowerA(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string TrimA(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static bool EndsWithI(const std::wstring& s, const std::wstring& suf) {
    return s.size() >= suf.size() && Lower(s.substr(s.size() - suf.size())) == Lower(suf);
}
static bool StartsWithIW(const std::wstring& s, const std::wstring& pre) {
    return s.size() >= pre.size() && Lower(s.substr(0, pre.size())) == Lower(pre);
}
static std::vector<std::wstring> ListDir(const std::wstring& dir, const std::wstring& pattern, bool dirs) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (fd.cFileName[0] == L'.' || isDir != dirs) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
static ULONGLONG FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a; if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}
static bool ReadAll(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear(); char b[65536]; DWORD n;
    while (ReadFile(h, b, sizeof b, &n, nullptr) && n) out.append(b, n);
    CloseHandle(h);
    return true;
}
static bool WriteAll(const std::wstring& p, const std::string& data) {
    DWORD attr = GetFileAttributesW(p.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(p.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(h);
    return ok;
}
static std::wstring DocumentsDir() {
    wchar_t* p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) d = p;
    if (p) CoTaskMemFree(p);
    return d;
}

// ---------------------------------------------------------------- package headers
// Every Unreal package starts with the tag 0x9E2A83C1, then the file version (low 16 bits) and the licensee
// version (high 16 bits). UE1 ~ 61-69, UE2 ~ 100-129 (UE2.5 up to ~150), UE3 ~ 180-900. UE4/5 store a negative
// "legacy version" there instead.
static bool PkgVer(const std::wstring& f, int& ver, int& lic, bool& ue4) {
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint32_t hd[2] = {}; DWORD n = 0;
    BOOL ok = ReadFile(h, hd, 8, &n, nullptr);
    CloseHandle(h);
    if (!ok || n != 8 || hd[0] != 0x9E2A83C1u) return false;
    if ((int32_t)hd[1] < 0) { ue4 = true; return true; }
    ver = (int)(hd[1] & 0xFFFF); lic = (int)(hd[1] >> 16); ue4 = false;
    return ver > 0;
}
static int GenFromVer(int v) { return v < 100 ? 1 : (v < 180 ? 2 : 3); }
// First readable package in a folder: Core / Engine first, then anything with the package tag (a few tries).
static bool PkgInDir(const std::wstring& dir, int& ver, int& lic, bool& ue4) {
    for (auto n : { L"Core.u", L"Engine.u" }) if (PkgVer(dir + L"\\" + n, ver, lic, ue4)) return true;
    int tries = 0;
    for (auto ext : { L"*.u", L"*.upk", L"*.xxx", L"*.pcc", L"*.utx", L"*.uax", L"*.usx", L"*.*" }) {
        for (auto& f : ListDir(dir, ext, false)) {
            if (++tries > 12) return false;
            if (PkgVer(dir + L"\\" + f, ver, lic, ue4)) return true;
        }
    }
    return false;
}
// Is this (or a folder near it) a UE4/5 game? (Content\Paks)
static bool LooksUE4Plus(const std::wstring& exe) {
    std::wstring d = DirOf(exe);
    for (int up = 0; up < 4 && !d.empty(); up++) {
        if (DirExists(d + L"\\Content\\Paks")) return true;
        for (auto& sub : ListDir(d, L"*", true)) if (DirExists(d + L"\\" + sub + L"\\Content\\Paks")) return true;
        std::wstring p = DirOf(d); if (p == d) break; d = p;
    }
    return false;
}
static std::wstring UE3Root(const std::wstring& exe) {
    std::wstring l = Lower(exe);
    size_t b = l.rfind(L"\\binaries\\");
    return b == std::wstring::npos ? DirOf(exe) : exe.substr(0, b);
}
struct Probe { int gen = 0, ver = 0, lic = 0; std::wstring root, sysDir, gameFolder; };
static Probe ProbeExe(const std::wstring& exe) {
    Probe p;
    if (exe.empty() || !FileExists(exe)) return p;
    int ver = 0, lic = 0; bool ue4 = false;
    std::wstring d = DirOf(exe);
    // UE1/2: the exe sits in System\ next to Core.u (or the game's root holds System\)
    for (auto sys : { d, d + L"\\System" }) {
        if ((FileExists(sys + L"\\Core.u") || FileExists(sys + L"\\Engine.u")) && PkgVer(FileExists(sys + L"\\Core.u") ? sys + L"\\Core.u" : sys + L"\\Engine.u", ver, lic, ue4) && !ue4) {
            p.gen = GenFromVer(ver); p.ver = ver; p.lic = lic; p.sysDir = sys; p.root = DirOf(sys);
            if (p.gen == 3) { p.gen = 0; continue; }   // (a UE3 Core.u in a System folder would be unusual)
            return p;
        }
    }
    if (LooksUE4Plus(exe)) return p;
    // UE3: <root>\Binaries\[Win32|Win64]\Game.exe and <root>\<Game>Game\CookedPC*\*.upk (or \Script for UDK)
    std::wstring root = UE3Root(exe);
    for (auto& f : ListDir(root, L"*", true)) {
        std::wstring fd = root + L"\\" + f;
        std::vector<std::wstring> pkgDirs;
        for (auto& c : ListDir(fd, L"Cooked*", true)) pkgDirs.push_back(fd + L"\\" + c);
        if (DirExists(fd + L"\\Script")) pkgDirs.push_back(fd + L"\\Script");
        for (auto& pd : pkgDirs) {
            if (PkgInDir(pd, ver, lic, ue4)) {
                if (ue4) return p;
                int g = GenFromVer(ver);
                if (g == 3 || (g == 2 && Lower(f).find(L"game") != std::wstring::npos)) {   // UE2.5 games can be laid out like UE3
                    p.gen = g; p.ver = ver; p.lic = lic; p.root = root; p.sysDir = fd; p.gameFolder = f;
                    return p;
                }
            }
        }
    }
    return p;
}

static std::mutex g_cmx;
static std::map<std::wstring, Probe> g_probe;
static std::map<std::wstring, std::pair<ULONGLONG, ClassicInfo>> g_info;
static Probe CachedProbe(const std::wstring& exe) {
    std::wstring k = Lower(exe);
    { std::lock_guard<std::mutex> lk(g_cmx); auto it = g_probe.find(k); if (it != g_probe.end()) return it->second; }
    Probe p = ProbeExe(exe);
    std::lock_guard<std::mutex> lk(g_cmx); g_probe[k] = p;
    return p;
}
int ClassicGen(const std::wstring& exe) { return exe.empty() ? 0 : CachedProbe(exe).gen; }
void ClassicForget() { std::lock_guard<std::mutex> lk(g_cmx); g_info.clear(); }
const char* ClassicGenName(int gen) {
    switch (gen) { case 1: return "Unreal Engine 1"; case 2: return "Unreal Engine 2"; case 3: return "Unreal Engine 3"; default: return "Unreal Engine 4 / 5"; }
}
const char* ClassicExamples(int gen) {
    switch (gen) {
    case 1: return "Unreal, Unreal Tournament (1999), Deus Ex, Rune, Harry Potter 1-2";
    case 2: return "Unreal Tournament 2004, Splinter Cell, SWAT 4, Killing Floor, BioShock";
    case 3: return "Unreal Tournament 3, Mass Effect, Batman Arkham, Borderlands, Dishonored";
    default: return "";
    }
}
std::wstring ClassicGameName(const std::wstring& exe) {
    Probe p = CachedProbe(exe);
    if (!p.gen || p.root.empty()) return StemOf(exe);
    return NameOf(p.root);
}

// ---------------------------------------------------------------- ini files
// Kept as UTF-8 text in memory; UTF-16 files (common in UE3) are converted back when saved.
struct IniFile { std::wstring path; std::vector<std::string> lines; bool utf16 = false, ok = false; };
static IniFile IniLoad(const std::wstring& path) {
    IniFile f; f.path = path;
    std::string raw; if (!ReadAll(path, raw)) return f;
    std::string text;
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        f.utf16 = true;
        std::wstring w((raw.size() - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); i++) w[i] = (wchar_t)((unsigned char)raw[2 + i * 2] | ((unsigned char)raw[3 + i * 2] << 8));
        text = W2U(w);
    } else if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) text = raw.substr(3);
    else text = raw;
    size_t s = 0;
    while (s <= text.size()) {
        size_t e = text.find('\n', s);
        std::string l = text.substr(s, e == std::string::npos ? std::string::npos : e - s);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        f.lines.push_back(l);
        if (e == std::string::npos) break;
        s = e + 1;
    }
    while (!f.lines.empty() && f.lines.back().empty()) f.lines.pop_back();
    f.ok = true;
    return f;
}
static std::wstring BackupOf(const std::wstring& p) { return p + L".launcher-backup"; }
// Saves (backing up the original the first time). Default*.ini keep their old time stamp, so a UE3 game
// doesn't think its defaults changed and rebuild your own settings files from scratch.
static bool IniSave(IniFile& f) {
    if (!FileExists(BackupOf(f.path))) CopyFileW(f.path.c_str(), BackupOf(f.path).c_str(), TRUE);
    FILETIME ct{}, at{}, wt{}; bool keepTime = StartsWithIW(NameOf(f.path), L"Default");
    if (keepTime) { HANDLE h = CreateFileW(f.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { GetFileTime(h, &ct, &at, &wt); CloseHandle(h); } else keepTime = false; }
    std::string text;
    for (auto& l : f.lines) { text += l; text += "\r\n"; }
    std::string out;
    if (f.utf16) { std::wstring w = U2W(text); out = "\xFF\xFE"; for (wchar_t ch : w) { out += (char)(ch & 0xFF); out += (char)((ch >> 8) & 0xFF); } }
    else out = text;
    if (!WriteAll(f.path, out)) return false;
    if (keepTime) { HANDLE h = CreateFileW(f.path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { SetFileTime(h, &ct, &at, &wt); CloseHandle(h); } }
    return true;
}
static bool IsHeader(const std::string& l, std::string* name = nullptr) {
    std::string t = TrimA(l);
    if (t.size() < 2 || t[0] != '[' || t.back() != ']') return false;
    if (name) *name = t.substr(1, t.size() - 2);
    return true;
}
static int FindSection(const IniFile& f, const std::string& sec) {
    std::string want = LowerA(sec), n;
    for (size_t i = 0; i < f.lines.size(); i++) if (IsHeader(f.lines[i], &n) && LowerA(n) == want) return (int)i;
    return -1;
}
static int SectionEnd(const IniFile& f, int hdr) {
    size_t i = hdr + 1;
    while (i < f.lines.size() && !IsHeader(f.lines[i])) i++;
    while (i > (size_t)hdr + 1 && TrimA(f.lines[i - 1]).empty()) i--;   // before the blank lines at the end
    return (int)i;
}
static std::vector<std::string> Sections(const IniFile& f) {
    std::vector<std::string> out; std::string n;
    for (auto& l : f.lines) if (IsHeader(l, &n)) out.push_back(n);
    return out;
}
static bool KeyOf(const std::string& l, std::string& key, std::string& val) {
    size_t e = l.find('='); if (e == std::string::npos) return false;
    key = TrimA(l.substr(0, e)); val = l.substr(e + 1);
    return !key.empty();
}
static std::string IniGet(const IniFile& f, const std::string& sec, const std::string& key, bool* found = nullptr) {
    if (found) *found = false;
    int h = FindSection(f, sec); if (h < 0) return "";
    std::string k, v, want = LowerA(key);
    for (int i = h + 1; i < (int)f.lines.size() && !IsHeader(f.lines[i]); i++)
        if (KeyOf(f.lines[i], k, v) && LowerA(k) == want) { if (found) *found = true; return TrimA(v); }
    return "";
}
static void IniSet(IniFile& f, const std::string& sec, const std::string& key, const std::string& val) {
    int h = FindSection(f, sec);
    if (h < 0) {
        if (!f.lines.empty() && !TrimA(f.lines.back()).empty()) f.lines.push_back("");
        f.lines.push_back("[" + sec + "]"); f.lines.push_back(key + "=" + val);
        return;
    }
    std::string k, v, want = LowerA(key);
    for (int i = h + 1; i < (int)f.lines.size() && !IsHeader(f.lines[i]); i++)
        if (KeyOf(f.lines[i], k, v) && LowerA(k) == want) { f.lines[i] = k + "=" + val; return; }
    f.lines.insert(f.lines.begin() + SectionEnd(f, h), key + "=" + val);
}
static bool HasLine(const IniFile& f, const std::string& sec, const std::string& line) {
    int h = FindSection(f, sec); if (h < 0) return false;
    std::string want = LowerA(TrimA(line));
    for (int i = h + 1; i < (int)f.lines.size() && !IsHeader(f.lines[i]); i++) if (LowerA(TrimA(f.lines[i])) == want) return true;
    return false;
}
static void AddLine(IniFile& f, const std::string& sec, const std::string& line) {
    if (HasLine(f, sec, line)) return;
    int h = FindSection(f, sec);
    if (h < 0) { if (!f.lines.empty() && !TrimA(f.lines.back()).empty()) f.lines.push_back(""); f.lines.push_back("[" + sec + "]"); f.lines.push_back(line); return; }
    f.lines.insert(f.lines.begin() + SectionEnd(f, h), line);
}
static int RemoveLines(IniFile& f, const std::string& line) {
    std::string want = LowerA(TrimA(line)); int n = 0;
    for (size_t i = 0; i < f.lines.size();) { if (LowerA(TrimA(f.lines[i])) == want) { f.lines.erase(f.lines.begin() + i); n++; } else i++; }
    return n;
}

// ---------------------------------------------------------------- details
static void AddInis(std::vector<std::wstring>& out, const std::wstring& dir) {
    for (auto& f : ListDir(dir, L"*.ini", false)) out.push_back(dir + L"\\" + f);
}
static bool IsDefaultIni(const std::wstring& name) {
    return StartsWithIW(name, L"Default") || StartsWithIW(name, L"Base") || StartsWithIW(name, L"Def");
}
static ClassicInfo Build(const std::wstring& exe) {
    ClassicInfo c; c.exe = exe;
    Probe p = CachedProbe(exe);
    if (!p.gen) return c;
    c.gen = p.gen; c.pkgVersion = p.ver; c.licensee = p.lic; c.root = p.root; c.sysDir = p.sysDir; c.gameFolder = p.gameFolder;
    if (c.gen <= 2 && c.gameFolder.empty()) {
        // UE1/2: everything is in System\  (<Game>.ini + User.ini, or <Game>User.ini)
        c.configDir = c.sysDir;
        AddInis(c.inis, c.sysDir);
        std::wstring stem = StemOf(exe);
        if (FileExists(c.sysDir + L"\\" + stem + L".ini")) c.mainIni = c.sysDir + L"\\" + stem + L".ini";
        if (FileExists(c.sysDir + L"\\User.ini")) c.userIni = c.sysDir + L"\\User.ini";
        for (auto& i : c.inis) {
            std::wstring n = NameOf(i);
            if (c.userIni.empty() && EndsWithI(n, L"User.ini") && !IsDefaultIni(n)) c.userIni = i;
        }
        if (c.mainIni.empty()) {   // the main ini is the one with the engine's settings
            for (auto& i : c.inis) {
                std::wstring n = NameOf(i);
                if (IsDefaultIni(n) || EndsWithI(n, L"User.ini") || Lower(n) == L"manifest.ini") continue;
                IniFile f = IniLoad(i);
                if (f.ok && FindSection(f, "Engine.Engine") >= 0) { c.mainIni = i; break; }
            }
        }
    } else {
        // UE3: <Game>Game\Config\Default*.ini, and the files the game makes from them (in the same folder or
        // in Documents\My Games\<Name>\<Game>Game\Config)
        std::wstring gameCfg = c.sysDir + L"\\Config";
        std::vector<std::wstring> userDirs;
        std::wstring docs = DocumentsDir();
        if (!docs.empty()) {
            std::wstring mg = docs + L"\\My Games";
            for (auto& d : ListDir(mg, L"*", true)) {
                std::wstring cand = mg + L"\\" + d + L"\\" + c.gameFolder + L"\\Config";
                if (DirExists(cand)) userDirs.push_back(cand);
            }
        }
        if (DirExists(gameCfg)) AddInis(c.inis, gameCfg);
        for (auto& u : userDirs) AddInis(c.inis, u);
        auto pick = [&](const std::wstring& suffix) -> std::wstring {
            for (auto& u : userDirs) for (auto& f : ListDir(u, L"*" + suffix, false)) if (!IsDefaultIni(f)) return u + L"\\" + f;
            for (auto& f : ListDir(gameCfg, L"*" + suffix, false)) if (!IsDefaultIni(f)) return gameCfg + L"\\" + f;
            return L"";
        };
        c.mainIni = pick(L"Engine.ini");
        c.userIni = pick(L"Input.ini");
        if (FileExists(gameCfg + L"\\DefaultInput.ini")) c.defaultInput = gameCfg + L"\\DefaultInput.ini";
        c.configDir = !userDirs.empty() ? userDirs[0] : gameCfg;
        if (!DirExists(gameCfg)) {
            for (auto& d : ListDir(c.sysDir, L"Cooked*", true))
                if (!ListDir(c.sysDir + L"\\" + d, L"Coalesced*", false).empty()) c.coalesced = true;
        }
    }
    for (auto& i : c.inis) if (FileExists(BackupOf(i))) { c.hasBackups = true; break; }

    // current state
    if (c.gen <= 2 && c.gameFolder.empty()) {
        IniFile u = IniLoad(c.userIni);
        std::string t = LowerA(IniGet(u, "Engine.Input", "Tilde"));
        c.consoleOn = t.find("console") != std::string::npos || t.find("type") != std::string::npos;
        c.cheatKeysOn = LowerA(IniGet(u, "Engine.Input", "NumPad1")) == "god";
        IniFile m = IniLoad(c.mainIni);
        c.windowed = LowerA(IniGet(m, "WinDrv.WindowsClient", "StartupFullscreen")) == "false";
        c.resX = atoi(IniGet(m, "WinDrv.WindowsClient", c.windowed ? "WindowedViewportX" : "FullscreenViewportX").c_str());
        c.resY = atoi(IniGet(m, "WinDrv.WindowsClient", c.windowed ? "WindowedViewportY" : "FullscreenViewportY").c_str());
    } else {
        IniFile in = IniLoad(!c.userIni.empty() ? c.userIni : c.defaultInput);
        std::string k = LowerA(IniGet(in, "Engine.Console", "ConsoleKey"));
        c.consoleOn = !k.empty() && k != "none";
        c.cheatKeysOn = false;
        for (auto& l : in.lines) { std::string ll = LowerA(l); if (ll.find("name=\"numpadone\",command=\"enablecheats | god\"") != std::string::npos) { c.cheatKeysOn = true; break; } }
    }
    return c;
}
ClassicInfo ClassicDetect(const std::wstring& exe) {
    std::wstring k = Lower(exe);
    ULONGLONG now = GetTickCount64();
    { std::lock_guard<std::mutex> lk(g_cmx); auto it = g_info.find(k); if (it != g_info.end() && now - it->second.first < 2000) return it->second.second; }
    ClassicInfo c = Build(exe);
    std::lock_guard<std::mutex> lk(g_cmx); g_info[k] = { now, c };
    return c;
}

// ---------------------------------------------------------------- changes
static const ClassicKey kKeys12[] = {
    { "NumPad1", "god" }, { "NumPad2", "fly" }, { "NumPad3", "ghost" }, { "NumPad4", "walk" },
    { "NumPad5", "behindview 1" }, { "NumPad6", "behindview 0" }, { "NumPad7", "slomo 0.5" }, { "NumPad8", "slomo 1" },
    { "NumPad9", "playersonly" }, { "NumPad0", "allammo" } };
static const ClassicKey kKeys3[] = {
    { "NumPadOne", "God" }, { "NumPadTwo", "Fly" }, { "NumPadThree", "Ghost" }, { "NumPadFour", "Walk" },
    { "NumPadFive", "ToggleDebugCamera" }, { "NumPadSix", "slomo 0.5" }, { "NumPadSeven", "slomo 1" }, { "NumPadEight", "PlayersOnly" },
    { "NumPadNine", "AllAmmo" } };
std::vector<ClassicKey> ClassicCheatKeyList(int gen) {
    if (gen == 3) return std::vector<ClassicKey>(std::begin(kKeys3), std::end(kKeys3));
    return std::vector<ClassicKey>(std::begin(kKeys12), std::end(kKeys12));
}
static bool UE3Style(const ClassicInfo& c) { return c.gen == 3 || !c.gameFolder.empty(); }

bool ClassicSetConsole(const std::wstring& exe, bool on, std::string& msg) {
    ClassicInfo c = ClassicDetect(exe);
    if (!c.gen) { msg = "That isn't an Unreal Engine 1-3 game."; return false; }
    int changed = 0;
    if (!UE3Style(c)) {
        IniFile u = IniLoad(c.userIni);
        if (!u.ok) { msg = "Couldn't find the game's User.ini. Start the game once so it makes one, then try again."; return false; }
        if (on) IniSet(u, "Engine.Input", "Tilde", c.gen == 1 ? "Type" : "ConsoleToggle");
        else { IniFile b = IniLoad(BackupOf(u.path)); IniSet(u, "Engine.Input", "Tilde", b.ok ? IniGet(b, "Engine.Input", "Tilde") : ""); }
        if (IniSave(u)) changed++;
        if (on) { IniFile m = IniLoad(c.mainIni); if (m.ok) { IniSet(m, "Engine.Console", "ConsoleKey", "192"); if (IniSave(m)) changed++; } }
        msg = on ? "Console on: press ~ (the key under Esc) in the game." : "Console key put back.";
    } else {
        if (c.coalesced) { msg = "This game packs its settings into one file the launcher can't change, so the console can't be switched on this way."; return false; }
        for (auto path : { c.userIni, c.defaultInput }) {
            IniFile f = IniLoad(path); if (!f.ok) continue;
            std::vector<std::string> secs = { "Engine.Console" };
            for (auto& s : Sections(f)) if (LowerA(s).size() > 7 && LowerA(s).rfind("console") == LowerA(s).size() - 7 && LowerA(s) != "engine.console") secs.push_back(s);
            for (auto& s : secs) {
                if (on) { IniSet(f, s, "ConsoleKey", "Tilde"); IniSet(f, s, "TypeKey", "Tab"); }
                else { IniFile b = IniLoad(BackupOf(path)); IniSet(f, s, "ConsoleKey", b.ok ? IniGet(b, s, "ConsoleKey") : "None"); IniSet(f, s, "TypeKey", b.ok ? IniGet(b, s, "TypeKey") : "None"); }
            }
            if (IniSave(f)) changed++;
        }
        if (!changed) { msg = "Couldn't find the game's input settings (start the game once, then try again)."; return false; }
        msg = on ? "Console on: press ~ for the full console or Tab for a one-line one. Some UE3 games removed the console when they shipped, so it may still not open."
                 : "Console keys put back.";
    }
    ClassicForget();
    if (!changed) { msg = "Couldn't save the game's settings (is the game running, or the folder read-only?)."; return false; }
    return true;
}

// One key binding line: [+.-]<List>Bindings=(Name="Key",Command="...") -> list name, key, command
static bool ParseBinding(const std::string& line, std::string& arr, std::string& name, std::string& cmd) {
    std::string t = TrimA(line);
    if (!t.empty() && (t[0] == '+' || t[0] == '.' || t[0] == '-')) t = t.substr(1);
    size_t eq = t.find('='); if (eq == std::string::npos || eq + 1 >= t.size() || t[eq + 1] != '(') return false;
    arr = TrimA(t.substr(0, eq));
    std::string la = LowerA(arr);
    if (la.size() < 8 || la.compare(la.size() - 8, 8, "bindings") != 0) return false;
    std::string body = t.substr(eq + 1), lb = LowerA(body);
    size_t n = lb.find("name=");
    if (n == std::string::npos) return false;
    size_t v = n + 5;
    if (v < body.size() && body[v] == '"') { size_t e = body.find('"', v + 1); if (e == std::string::npos) return false; name = body.substr(v + 1, e - v - 1); }
    else { size_t e = body.find_first_of(",)", v); name = TrimA(body.substr(v, e == std::string::npos ? std::string::npos : e - v)); }
    size_t c = lb.find("command=\"");
    cmd.clear();
    if (c != std::string::npos) { size_t e = body.find('"', c + 9); if (e != std::string::npos) cmd = body.substr(c + 9, e - c - 9); }
    return true;
}
static bool IsCheatKey(const std::string& name) { for (auto& k : kKeys3) if (LowerA(name) == LowerA(k.key)) return true; return false; }
static bool IsCheatCmd(const std::string& cmd) {
    std::string l = LowerA(TrimA(cmd));
    for (auto& k : kKeys3) { std::string w = LowerA(k.what); if (l == w || l == "enablecheats | " + w) return true; }
    return false;
}

bool ClassicSetCheatKeys(const std::wstring& exe, bool on, std::string& msg) {
    ClassicInfo c = ClassicDetect(exe);
    if (!c.gen) { msg = "That isn't an Unreal Engine 1-3 game."; return false; }
    int changed = 0;
    if (!UE3Style(c)) {
        IniFile u = IniLoad(c.userIni);
        if (!u.ok) { msg = "Couldn't find the game's User.ini. Start the game once so it makes one, then try again."; return false; }
        IniFile b = IniLoad(BackupOf(u.path));
        for (auto& k : kKeys12) IniSet(u, "Engine.Input", k.key, on ? k.what : (b.ok ? IniGet(b, "Engine.Input", k.key) : ""));
        if (IniSave(u)) changed++;
    } else {
        if (c.coalesced) { msg = "This game packs its settings into one file the launcher can't change, so cheat keys can't be added."; return false; }
        for (auto path : { c.userIni, c.defaultInput }) {
            IniFile f = IniLoad(path); if (!f.ok) continue;
            IniFile bak = IniLoad(BackupOf(path));
            bool isDefault = IsDefaultIni(NameOf(path));
            // 1. take out our keys from any earlier version (any section), and anything else already on those keys
            //    in the player-input sections (the first binding for a key wins, so they'd hide ours)
            std::vector<std::string> secs = { "Engine.PlayerInput" };
            for (auto& sname : Sections(f)) { std::string l = LowerA(sname); if (l != "engine.playerinput" && l.size() >= 11 && l.compare(l.size() - 11, 11, "playerinput") == 0) secs.push_back(sname); }
            std::vector<std::pair<std::string, std::vector<std::string>>> keysPerSec;   // section -> binding lists in it
            for (auto& sname : secs) {
                std::vector<std::string> arrs;
                int h = FindSection(f, sname);
                if (LowerA(sname) == "engine.playerinput") arrs.push_back("Bindings");
                if (h >= 0) for (int i = h + 1; i < (int)f.lines.size() && !IsHeader(f.lines[i]); i++) {
                    std::string arr, nm, cmd;
                    if (!ParseBinding(f.lines[i], arr, nm, cmd)) continue;
                    std::string la = LowerA(arr);
                    if (la.find("controller") != std::string::npos || la.find("gamepad") != std::string::npos || la.find("xbox") != std::string::npos || la.find("mobile") != std::string::npos) continue;
                    if (std::find(arrs.begin(), arrs.end(), arr) == arrs.end()) arrs.push_back(arr);
                }
                keysPerSec.push_back({ sname, arrs });
            }
            for (size_t i = 0; i < f.lines.size();) {
                std::string arr, nm, cmd;
                bool ours = ParseBinding(f.lines[i], arr, nm, cmd) && IsCheatKey(nm);
                if (ours && IsCheatCmd(cmd)) { f.lines.erase(f.lines.begin() + i); continue; }
                if (ours && on) {   // something else on one of our keys, inside a player-input section
                    std::string sec; for (int j = (int)i; j >= 0; j--) if (IsHeader(f.lines[j], &sec)) break;
                    bool inTarget = false; for (auto& t : secs) if (LowerA(t) == LowerA(sec)) inTarget = true;
                    if (inTarget) { f.lines.erase(f.lines.begin() + i); continue; }
                }
                i++;
            }
            // 2. on: add ours to every binding list of those sections. off: put back what was on those keys before.
            for (auto& sp : keysPerSec) {
                if (on) {
                    for (auto& arr : sp.second) for (auto& k : kKeys3)
                        AddLine(f, sp.first, std::string(isDefault ? "+" : "") + arr + "=(Name=\"" + k.key + "\",Command=\"EnableCheats | " + k.what + "\")");
                } else if (bak.ok) {
                    int h = FindSection(bak, sp.first);
                    if (h >= 0) for (int i = h + 1; i < (int)bak.lines.size() && !IsHeader(bak.lines[i]); i++) {
                        std::string arr, nm, cmd;
                        if (ParseBinding(bak.lines[i], arr, nm, cmd) && IsCheatKey(nm) && !IsCheatCmd(cmd)) AddLine(f, sp.first, bak.lines[i]);
                    }
                }
            }
            if (IniSave(f)) changed++;
        }
        if (!changed) { msg = "Couldn't find the game's input settings (start the game once, then try again)."; return false; }
    }
    ClassicForget();
    if (!changed) { msg = "Couldn't save the game's settings (is the game running, or the folder read-only?)."; return false; }
    msg = on ? "Cheat keys added to the number pad (Num Lock on). Single player only; some games turn cheats off." : "Cheat keys removed.";
    return true;
}

bool ClassicSetDisplay(const std::wstring& exe, bool windowed, int w, int h, std::string& msg) {
    ClassicInfo c = ClassicDetect(exe);
    if (!c.gen || UE3Style(c)) { msg = "Unreal Engine 3 games take the window size from the launch options instead."; return false; }
    IniFile m = IniLoad(c.mainIni);
    if (!m.ok) { msg = "Couldn't find the game's main .ini. Start the game once so it makes one, then try again."; return false; }
    IniSet(m, "WinDrv.WindowsClient", "StartupFullscreen", windowed ? "False" : "True");
    if (w > 0 && h > 0) {
        IniSet(m, "WinDrv.WindowsClient", "WindowedViewportX", std::to_string(w)); IniSet(m, "WinDrv.WindowsClient", "WindowedViewportY", std::to_string(h));
        IniSet(m, "WinDrv.WindowsClient", "FullscreenViewportX", std::to_string(w)); IniSet(m, "WinDrv.WindowsClient", "FullscreenViewportY", std::to_string(h));
    }
    ClassicForget();
    if (!IniSave(m)) { msg = "Couldn't save the game's settings (is the game running?)."; return false; }
    msg = std::string(windowed ? "Starts in a window" : "Starts full screen") + (w > 0 && h > 0 ? " at " + std::to_string(w) + " x " + std::to_string(h) : "") + ".";
    return true;
}

bool ClassicRestore(const std::wstring& exe, std::string& msg) {
    ClassicInfo c = ClassicDetect(exe);
    int n = 0;
    std::vector<std::wstring> all = c.inis;
    if (!c.defaultInput.empty()) all.push_back(c.defaultInput);
    std::sort(all.begin(), all.end()); all.erase(std::unique(all.begin(), all.end()), all.end());
    for (auto& i : all) {
        std::wstring b = BackupOf(i);
        if (!FileExists(b)) continue;
        DWORD attr = GetFileAttributesW(i.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(i.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
        if (CopyFileW(b.c_str(), i.c_str(), FALSE)) { DeleteFileW(b.c_str()); n++; }
    }
    ClassicForget();
    msg = n ? "Put back the game's original settings (" + std::to_string(n) + " file" + (n == 1 ? "" : "s") + ")." : "Nothing to put back: the launcher hasn't changed this game's settings.";
    return true;
}

// ---------------------------------------------------------------- maps and launching
static std::string MapExt(const ClassicInfo& c) {
    std::vector<std::wstring> src;
    if (!c.mainIni.empty()) src.push_back(c.mainIni);
    if (!c.gameFolder.empty()) src.push_back(c.sysDir + L"\\Config\\DefaultEngine.ini");
    for (auto& s : src) { IniFile f = IniLoad(s); std::string e = TrimA(IniGet(f, "URL", "MapExt")); if (!e.empty()) return LowerA(e); }
    return c.gen == 1 ? "unr" : (c.gen == 2 ? "ut2" : "");
}
static void FindMaps(const std::wstring& dir, const std::wstring& ext, int depth, std::vector<std::wstring>& out) {
    if (out.size() > 600) return;
    for (auto& f : ListDir(dir, L"*." + ext, false)) out.push_back(StemOf(f));
    if (depth <= 0) return;
    for (auto& d : ListDir(dir, L"*", true)) FindMaps(dir + L"\\" + d, ext, depth - 1, out);
}
std::vector<std::wstring> ClassicMaps(const std::wstring& exe) {
    ClassicInfo c = ClassicDetect(exe);
    std::vector<std::wstring> out;
    if (!c.gen) return out;
    std::wstring ext = U2W(MapExt(c));
    if (ext.empty()) return out;
    if (c.gameFolder.empty()) FindMaps(c.root + L"\\Maps", ext, 1, out);
    else for (auto& d : ListDir(c.sysDir, L"*", true)) if (StartsWithIW(d, L"Cooked") || Lower(d) == L"content" || Lower(d) == L"maps") FindMaps(c.sysDir + L"\\" + d, ext, 3, out);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return Lower(a) < Lower(b); });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
std::wstring ClassicLaunchArgs(int gen, const std::wstring& map, bool windowed, bool noMovies, bool log, int w, int h, const std::wstring& extra) {
    std::wstring a;
    auto add = [&](const std::wstring& s) { if (s.empty()) return; if (!a.empty()) a += L" "; a += s; };
    add(map);
    if (gen == 3) {
        if (windowed) add(L"-windowed");
        if (w > 0 && h > 0) add(L"-ResX=" + std::to_wstring(w) + L" -ResY=" + std::to_wstring(h));
        if (noMovies) add(L"-nostartupmovies");
    }
    if (log) add(L"-log");
    add(Trim(extra));
    return a;
}
bool ClassicLaunch(const std::wstring& exe, const std::wstring& args) {
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? L"" : L" " + args);
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, DirOf(exe).c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

// ---------------------------------------------------------------- finding games on this PC
static bool SkipExe(const std::wstring& n) {
    std::wstring l = Lower(n);
    for (auto s : { L"ucc", L"setup", L"unins", L"unrealed", L"editor", L"server", L"redist", L"crash", L"dxsetup", L"vcredist", L"benchmark", L"config", L"report", L"update", L"autorun", L"easyanticheat", L"be_service" })
        if (l.find(s) != std::wstring::npos) return true;
    return false;
}
static std::wstring PickExe(const std::wstring& dir, const std::wstring& prefer) {
    std::wstring best; ULONGLONG bestSz = 0; int bestScore = -1;
    for (auto& f : ListDir(dir, L"*.exe", false)) {
        if (SkipExe(f)) continue;
        std::wstring l = Lower(f);
        int score = 0;
        if (!prefer.empty() && FileExists(dir + L"\\" + StemOf(f) + L".ini")) score += 4;
        if (l.find(L"shipping") != std::wstring::npos || (!prefer.empty() && l.find(Lower(prefer)) != std::wstring::npos)) score += 3;
        if (l.find(L"launcher") != std::wstring::npos) score -= 2;
        ULONGLONG sz = FileSize(dir + L"\\" + f);
        if (score > bestScore || (score == bestScore && sz > bestSz)) { best = dir + L"\\" + f; bestScore = score; bestSz = sz; }
    }
    return best;
}
static std::wstring ExeForGameDir(const std::wstring& gd) {
    if (FileExists(gd + L"\\System\\Core.u") || FileExists(gd + L"\\System\\Engine.u")) return PickExe(gd + L"\\System", L"x");
    if (DirExists(gd + L"\\Binaries")) {
        std::wstring folder;
        for (auto& d : ListDir(gd, L"*", true)) if (!ListDir(gd + L"\\" + d, L"Cooked*", true).empty() || DirExists(gd + L"\\" + d + L"\\Script")) { folder = d; break; }
        if (folder.empty()) return L"";
        std::wstring stem = folder.size() > 4 && EndsWithI(folder, L"Game") ? folder.substr(0, folder.size() - 4) : folder;
        for (auto sub : { L"\\Binaries\\Win64", L"\\Binaries\\Win32", L"\\Binaries" }) {
            std::wstring e = PickExe(gd + sub, stem);
            if (!e.empty()) return e;
        }
    }
    return L"";
}
std::vector<ClassicFound> ClassicScanPC() {
    std::vector<std::wstring> libs;
    for (auto steam : { L"C:\\Program Files (x86)\\Steam", L"C:\\Program Files\\Steam" }) {
        std::string vdf; if (!ReadAll(std::wstring(steam) + L"\\steamapps\\libraryfolders.vdf", vdf)) continue;
        libs.push_back(std::wstring(steam) + L"\\steamapps\\common");
        for (size_t p = vdf.find("\"path\""); p != std::string::npos; p = vdf.find("\"path\"", p + 1)) {
            size_t a = vdf.find('"', p + 6), e = a == std::string::npos ? a : vdf.find('"', a + 1);
            if (a == std::string::npos || e == std::string::npos) break;
            std::string path = vdf.substr(a + 1, e - a - 1), clean;
            for (size_t i = 0; i < path.size(); i++) { if (path[i] == '\\' && i + 1 < path.size() && path[i + 1] == '\\') i++; clean += path[i]; }
            libs.push_back(U2W(clean) + L"\\steamapps\\common");
        }
    }
    for (wchar_t d = L'C'; d <= L'Z'; d++) {
        std::wstring r = std::wstring(1, d) + L":\\";
        if (GetDriveTypeW(r.c_str()) != DRIVE_FIXED) continue;
        for (auto sub : { L"SteamLibrary\\steamapps\\common", L"Games", L"GOG Games", L"GOG Galaxy\\Games", L"Program Files (x86)\\GOG Galaxy\\Games",
                          L"Program Files (x86)", L"Program Files\\Epic Games", L"Epic Games", L"UnrealTournament", L"UT2004" }) libs.push_back(r + sub);
    }
    std::vector<ClassicFound> found; std::vector<std::wstring> seen;
    for (auto& lib : libs) {
        std::wstring ll = Lower(lib);
        if (std::find(seen.begin(), seen.end(), ll) != seen.end() || !DirExists(lib)) continue;
        seen.push_back(ll);
        // the library itself can be a game folder (e.g. C:\UT2004)
        std::vector<std::wstring> games = { lib };
        for (auto& d : ListDir(lib, L"*", true)) games.push_back(lib + L"\\" + d);
        for (auto& gd : games) {
            std::wstring exe = ExeForGameDir(gd);
            if (exe.empty()) continue;
            int g = ClassicGen(exe);
            if (!g) continue;
            bool dup = false; for (auto& f : found) if (Lower(f.exe) == Lower(exe)) dup = true;
            if (!dup) found.push_back({ NameOf(gd), exe, g });
        }
    }
    std::sort(found.begin(), found.end(), [](const ClassicFound& a, const ClassicFound& b) { return Lower(a.name) < Lower(b.name); });
    return found;
}
