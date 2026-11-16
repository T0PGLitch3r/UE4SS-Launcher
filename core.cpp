#include <map>
// Core launcher logic: bundled UE4SS files, game detection, injection, mod management.
#include "core.h"
#include "unity.h"
#include "classic.h"
#include "updater.h"
#include <tlhelp32.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <sstream>

#define IDR_BUNDLE_INDEX 900

std::mutex g_logMx;
std::vector<LogLine> g_logs;
std::atomic<int> g_status{ ST_IDLE };
std::wstring g_dataDir, g_iniPath, g_gameExe;
bool g_autoInject = false;
ULONGLONG g_watchUntil = 0;
std::vector<DWORD> g_handled;
HWND g_hwnd = nullptr;
std::atomic<bool> g_modsDirty{ false };
std::atomic<bool> g_busy{ false };
static HINSTANCE g_inst;

// ================================================================ strings & files
std::string W2U(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring U2W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}
static bool StartsWithI(const std::wstring& s, const std::wstring& p) { return s.size() >= p.size() && Lower(s.substr(0, p.size())) == Lower(p); }
bool FileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
bool DirExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
std::wstring DirOf(const std::wstring& p) { size_t i = p.find_last_of(L"\\/"); return i == std::wstring::npos ? L"" : p.substr(0, i); }
std::wstring NameOf(const std::wstring& p) { size_t i = p.find_last_of(L"\\/"); return i == std::wstring::npos ? p : p.substr(i + 1); }
std::wstring StemOf(const std::wstring& p) { std::wstring n = NameOf(p); size_t i = n.find_last_of(L'.'); return i == std::wstring::npos ? n : n.substr(0, i); }
std::wstring ExtOf(const std::wstring& p) { std::wstring n = NameOf(p); size_t i = n.find_last_of(L'.'); return i == std::wstring::npos ? L"" : Lower(n.substr(i)); }
std::wstring ExeDir() { wchar_t p[MAX_PATH * 2]; GetModuleFileNameW(nullptr, p, MAX_PATH * 2); return DirOf(p); }
void MakeDirs(const std::wstring& p) { SHCreateDirectoryExW(nullptr, p.c_str(), nullptr); }
void OpenFolder(const std::wstring& p) { MakeDirs(p); ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void OpenUrl(const wchar_t* url) { ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL); }

void Log(const std::wstring& msg, int level) {
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[16]; snprintf(ts, sizeof ts, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    {
        std::lock_guard<std::mutex> lk(g_logMx);
        g_logs.push_back({ ts, W2U(msg), level });
        if (g_logs.size() > 500) g_logs.erase(g_logs.begin());
    }
    if (g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0);  // wake the UI
}
static std::wstring ErrStr(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? Trim(buf) : L"Unknown error";
    if (buf) LocalFree(buf);
    return s + L" (code " + std::to_wstring(e) + L")";
}

static bool ReadUtf8(const std::wstring& path, std::wstring& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(f, nullptr), rd = 0;
    std::string b(sz, '\0'); if (sz) ReadFile(f, &b[0], sz, &rd, nullptr);
    CloseHandle(f);
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) b.erase(0, 3);
    out = U2W(b);
    return true;
}
static bool WriteBytes(const std::wstring& path, const void* data, DWORD size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0; BOOL ok = WriteFile(f, data, size, &wr, nullptr);
    CloseHandle(f);
    return ok && wr == size;
}
static bool WriteUtf8(const std::wstring& path, const std::wstring& text) { std::string b = W2U(text); return WriteBytes(path, b.data(), (DWORD)b.size()); }
static std::vector<std::wstring> SplitLines(const std::wstring& t) {
    std::vector<std::wstring> v; std::wstring cur;
    for (wchar_t c : t) { if (c == L'\n') { if (!cur.empty() && cur.back() == L'\r') cur.pop_back(); v.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) v.push_back(cur);
    return v;
}
static std::wstring JoinLines(const std::vector<std::wstring>& v) { std::wstring s; for (auto& l : v) s += l + L"\r\n"; return s; }
static bool CopyTree(const std::wstring& from, const std::wstring& toDir) {
    std::wstring f = from + L'\0', t = toDir + L'\0';
    SHFILEOPSTRUCTW op{}; op.wFunc = FO_COPY; op.pFrom = f.c_str(); op.pTo = t.c_str();
    op.fFlags = FOF_NOCONFIRMMKDIR | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}
static void DeleteTree(const std::wstring& path) {
    std::wstring f = path + L'\0';
    SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = f.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationW(&op);
}
static bool DirWritable(const std::wstring& dir) {
    MakeDirs(dir);
    std::wstring probe = dir + L"\\.write_test";
    if (!WriteBytes(probe, "x", 1)) return false;
    DeleteFileW(probe.c_str());
    return true;
}

// ================================================================ bundled UE4SS
int ExtractBundle(bool force) {
    if (force) UpdForgetInstalled();   // "Reset files" goes back to the UE4SS built into the launcher
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(IDR_BUNDLE_INDEX), RT_RCDATA);
    if (!r) { Log(L"This build has no bundled UE4SS files.", LOG_ERR); return -1; }
    std::string index((const char*)LockResource(LoadResource(g_inst, r)), SizeofResource(g_inst, r));
    std::istringstream ss(index);
    std::string line; int written = 0, failed = 0;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t'); if (tab == std::string::npos) continue;
        int id = atoi(line.substr(0, tab).c_str());
        std::wstring rel = U2W(line.substr(tab + 1));
        std::wstring dest = g_dataDir + L"\\" + rel;
        HRSRC fr = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!fr) continue;
        DWORD size = SizeofResource(g_inst, fr);
        const void* data = LockResource(LoadResource(g_inst, fr));
        bool need = force || !FileExists(dest);
        // The launcher's own bridge mod is always kept in sync with this build.
        if (!need && StartsWithI(rel, L"Mods\\LauncherBridge\\Scripts\\")) {
            HANDLE ef = CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (ef == INVALID_HANDLE_VALUE) need = true;
            else {
                DWORD esz = GetFileSize(ef, nullptr), rd = 0;
                std::string cur(esz, '\0');
                if (esz) ReadFile(ef, &cur[0], esz, &rd, nullptr);
                CloseHandle(ef);
                need = esz != size || memcmp(cur.data(), data, size) != 0;
            }
        }
        // (unless a newer UE4SS was installed by the updater: then keep that one)
        if (!need && Lower(NameOf(rel)) == L"ue4ss.dll" && !UpdKeepInstalledDll()) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(dest.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh != 0 || fa.nFileSizeLow != size)) need = true;
        }
        if (!need) continue;
        MakeDirs(DirOf(dest));
        if (WriteBytes(dest, data, size)) written++; else failed++;
    }
    if (failed) Log(L"Couldn't write " + std::to_wstring(failed) + L" UE4SS file(s). Close the game if it's running and try again.", LOG_WARN);
    return written;
}

void SetupDataDir(HINSTANCE inst) {
    g_inst = inst;
    std::wstring exe = ExeDir(), local = exe + L"\\UE4SS";
    if (FileExists(exe + L"\\UE4SS.dll") && FileExists(exe + L"\\UE4SS-settings.ini") && DirWritable(exe)) g_dataDir = exe;
    else if (DirWritable(local)) g_dataDir = local;
    else {
        wchar_t* appdata = nullptr;
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata);
        g_dataDir = std::wstring(appdata ? appdata : L"C:") + L"\\UE4SS-Launcher\\UE4SS";
        if (appdata) CoTaskMemFree(appdata);
        MakeDirs(g_dataDir);
    }
    g_iniPath = (DirWritable(exe) ? exe : DirOf(g_dataDir)) + L"\\UE4SS-Launcher.ini";
    int n = ExtractBundle(false);
    if (n > 0) Log(L"Unpacked " + std::to_wstring(n) + L" UE4SS files to " + g_dataDir, LOG_OK);
}
std::wstring DllPath() { return g_dataDir + L"\\UE4SS.dll"; }
std::wstring ModsDir() { return g_dataDir + L"\\Mods"; }

bool RuntimeMissing() {
    for (auto n : { L"MSVCP140.dll", L"VCRUNTIME140.dll", L"VCRUNTIME140_1.dll", L"MSVCP140_ATOMIC_WAIT.dll" }) {
        HMODULE h = LoadLibraryExW(n, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!h) return true;
        FreeLibrary(h);
    }
    return false;
}

static std::wstring SettingsPath() { return g_dataDir + L"\\UE4SS-settings.ini"; }
bool IniGetBool(const std::wstring& key) {
    std::wstring t; if (!ReadUtf8(SettingsPath(), t)) return false;
    for (auto& l : SplitLines(t)) {
        std::wstring s = Trim(l); size_t eq = s.find(L'=');
        if (s.empty() || eq == std::wstring::npos || s[0] == L';') continue;
        if (Lower(Trim(s.substr(0, eq))) == Lower(key)) return Trim(s.substr(eq + 1)) == L"1";
    }
    return false;
}
void IniSet(const std::vector<std::pair<std::wstring, std::wstring>>& kv) {
    std::wstring t; if (!ReadUtf8(SettingsPath(), t)) { Log(L"UE4SS-settings.ini not found.", LOG_ERR); return; }
    auto lines = SplitLines(t);
    for (auto& l : lines) {
        std::wstring s = Trim(l); size_t eq = s.find(L'=');
        if (s.empty() || s[0] == L';' || eq == std::wstring::npos) continue;
        std::wstring k = Trim(s.substr(0, eq));
        for (auto& p : kv) if (Lower(k) == Lower(p.first)) l = k + L" = " + p.second;
    }
    if (!WriteUtf8(SettingsPath(), JoinLines(lines))) Log(L"Couldn't save UE4SS-settings.ini.", LOG_ERR);
}

// ================================================================ game paths
bool LooksUnreal(const std::wstring& name, const std::wstring& path) {
    std::wstring n = Lower(name), p = Lower(path);
    for (auto s : { L"-win64-shipping", L"-wingdk-shipping", L"-win64-test", L"-win64-development" })
        if (n.find(s) != std::wstring::npos) return true;
    return p.find(L"\\binaries\\win64\\") != std::wstring::npos || p.find(L"\\binaries\\wingdk\\") != std::wstring::npos;
}
static std::wstring GameRoot(const std::wstring& exe) {
    size_t b = Lower(exe).find(L"\\binaries\\");
    return b != std::wstring::npos ? DirOf(exe.substr(0, b)) : DirOf(exe);
}
static std::wstring FindShippingExe(const std::wstring& root) {
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        for (auto plat : { L"Win64", L"WinGDK" }) {
            std::wstring bin = root + L"\\" + fd.cFileName + L"\\Binaries\\" + plat;
            WIN32_FIND_DATAW fe; HANDLE h2 = FindFirstFileW((bin + L"\\*.exe").c_str(), &fe);
            if (h2 == INVALID_HANDLE_VALUE) continue;
            do { if (LooksUnreal(fe.cFileName, bin + L"\\" + fe.cFileName)) { found = bin + L"\\" + fe.cFileName; break; } } while (FindNextFileW(h2, &fe));
            FindClose(h2);
            if (!found.empty()) break;
        }
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}
static std::wstring g_targetFor, g_target;
void InvalidateTarget() { g_targetFor = L"\x1"; }
std::wstring TargetExe() {
    if (g_targetFor == g_gameExe) return g_target;
    g_targetFor = g_gameExe;
    std::wstring g = Trim(g_gameExe);
    if (g.empty()) g_target = L"";
    else if (ClassicGen(g)) g_target = g;            // Unreal 1-3: the exe you chose is the game
    else if (LooksUnreal(NameOf(g), g)) g_target = g;
    else { std::wstring s = FindShippingExe(GameRoot(g)); g_target = s.empty() ? g : s; }
    return g_target;
}
std::wstring PaksDir() {
    std::wstring t = TargetExe();
    size_t b = Lower(t).find(L"\\binaries\\");
    return b == std::wstring::npos ? L"" : t.substr(0, b) + L"\\Content\\Paks";
}
std::wstring GameDisplayName() {
    std::wstring t = TargetExe();
    if (t.empty()) return L"";
    if (ClassicGen(t)) return ClassicGameName(t);
    std::wstring n = StemOf(t);
    for (auto suf : { L"-Win64-Shipping", L"-WinGDK-Shipping", L"-Win64-Test", L"-Win64-Development" }) {
        size_t i = Lower(n).find(Lower(suf));
        if (i != std::wstring::npos) { n = n.substr(0, i); break; }
    }
    return n;
}

// ================================================================ processes & injection
static bool ModuleLoaded(DWORD pid, const std::wstring& modName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{ sizeof(me) }; bool found = false;
    if (Module32FirstW(snap, &me)) do { if (Lower(me.szModule) == Lower(modName)) { found = true; break; } } while (Module32NextW(snap, &me));
    CloseHandle(snap);
    return found;
}
static std::wstring ProcPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t p[MAX_PATH * 2]; DWORD sz = MAX_PATH * 2;
    std::wstring r = QueryFullProcessImageNameW(h, 0, p, &sz) ? p : L"";
    CloseHandle(h);
    return r;
}

// ---- per-game fixes for games whose engine was changed so UE4SS can't find things on its own.
// UE4SS looks for a folder in its own directory named after the folder 3 levels above the game's exe
// (e.g. ...\<this>\Dungeons\Binaries\WinGDK\Game.exe) and then reads its settings and layout files from there.
struct GameSig { const wchar_t* file; const char* lua; };
struct GameFix { const wchar_t* exe; const wchar_t* pathHas; const wchar_t* name; const char* layout; const GameSig* sigs; const char* settings = nullptr; };
// High on Life: UE4SS's "level loaded" hook lands on the wrong function in this game and crashes it a few seconds
// after every level load (copying a level address that isn't there). The launcher's mod doesn't need it.
static const char* kHoLSettings = "HookLoadMap = 0\0";
// Hogwarts Legacy (about two million objects, thousands of actors ticking): UE4SS hooks that run for every actor
// every frame, or on every level / actor start, cost frame time there and the launcher's mod doesn't use them.
static const char* kHLSettings = "HookAActorTick = 0\0HookLoadMap = 0\0HookBeginPlay = 0\0HookEndPlay = 0\0HookGameViewportClientTick = 0\0";
// South of Midnight (UE 4.27, Compulsion's own engine branch): UE4SS can't find the object list by itself.
// This finds it from the lock taken around the old object's destructor in StaticAllocateObject
// (GUObjectArray.ObjObjectsCritical, which sits 0x30 into GUObjectArray).
static const GameSig kSoMSigs[] = {
    { L"GUObjectArray.lua",
      "function Register()\r\n"
      "    return \"48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8B 06 33 D2 48 8B CE FF 10 48 8D 0D ?? ?? ?? ?? FF 15\"\r\n"
      "end\r\n"
      "function OnMatchFound(MatchAddress)\r\n"
      "    local Offset = DerefToInt32(MatchAddress + 3)\r\n"
      "    if not Offset then return nil end\r\n"
      "    return MatchAddress + 7 + Offset - 0x30\r\n"
      "end\r\n" },
    { nullptr, nullptr }
};
// Minecraft Dungeons II (UE 5.6, Xbox app and Steam builds are the same): Mojang added fields to UClass after
// ClassWithin (everything from ClassConfigName on moves by 0x60) and 4 bytes after FProperty::RepIndex (same change as
// Titan Quest 2's UE4SS config). Worked out from the game's own objects in crash dumps.
static const char* kMD2Layout =
      "[UClass]\r\n"
      "ClassConfigName = 0x148\r\n"
      "NetFields = 0x160\r\n"
      "ClassDefaultObject = 0x170\r\n"
      "SparseClassData = 0x178\r\n"
      "SparseClassDataStruct = 0x180\r\n"
      "FuncMap = 0x188\r\n"
      "AllFunctionsCache = 0x1E0\r\n"
      "Interfaces = 0x238\r\n"
      "ReferenceSchema = 0x248\r\n"
      "[FField]\r\n"
      "ClassPrivate = 0x8\r\n"
      "Owner = 0x10\r\n"
      "Next = 0x18\r\n"
      "NamePrivate = 0x20\r\n"
      "FlagsPrivate = 0x28\r\n"
      "[FProperty]\r\n"
      "ArrayDim = 0x30\r\n"
      "ElementSize = 0x34\r\n"
      "PropertyFlags = 0x38\r\n"
      "RepIndex = 0x40\r\n"
      "Offset_Internal = 0x48\r\n"
      "PropertyLinkNext = 0x50\r\n"
      "NextRef = 0x58\r\n"
      "DestructorLinkNext = 0x60\r\n"
      "PostConstructLinkNext = 0x68\r\n"
      "RepNotifyFunc = 0x70\r\n"
      "UEP_TotalSize = 0x74\r\n"
      "[FMulticastDelegateProperty]\r\n"
      "SignatureFunction = 0x78\r\n"
      "UEP_TotalSize = 0x80\r\n"
      "[FObjectPropertyBase]\r\n"
      "PropertyClass = 0x78\r\n"
      "UEP_TotalSize = 0x80\r\n"
      "[FStructProperty]\r\n"
      "Struct = 0x78\r\n"
      "UEP_TotalSize = 0x80\r\n"
      "[FArrayProperty]\r\n"
      "ArrayFlags = 0x78\r\n"
      "Inner = 0x80\r\n"
      "UEP_TotalSize = 0x88\r\n"
      "[FMapProperty]\r\n"
      "KeyProp = 0x78\r\n"
      "ValueProp = 0x80\r\n"
      "MapLayout = 0x88\r\n"
      "MapFlags = 0xA0\r\n"
      "UEP_TotalSize = 0xA8\r\n"
      "[FBoolProperty]\r\n"
      "FieldSize = 0x74\r\n"
      "ByteOffset = 0x75\r\n"
      "ByteMask = 0x76\r\n"
      "FieldMask = 0x77\r\n"
      "UEP_TotalSize = 0x7C\r\n"
      "[FByteProperty]\r\n"
      "Enum = 0x78\r\n"
      "UEP_TotalSize = 0x80\r\n"
      "[FEnumProperty]\r\n"
      "UnderlyingProp = 0x78\r\n"
      "Enum = 0x80\r\n"
      "UEP_TotalSize = 0x88\r\n"
      "[FClassProperty]\r\n"
      "MetaClass = 0x80\r\n"
      "UEP_TotalSize = 0x88\r\n"
      "[FSoftClassProperty]\r\n"
      "MetaClass = 0x80\r\n"
      "UEP_TotalSize = 0x88\r\n"
      "[FDelegateProperty]\r\n"
      "SignatureFunction = 0x78\r\n"
      "UEP_TotalSize = 0x80\r\n"
      "[FInterfaceProperty]\r\n"
      "InterfaceClass = 0x78\r\n"
      "UEP_TotalSize = 0x80\r\n"
      "[FFieldPathProperty]\r\n"
      "PropertyClass = 0x78\r\n"
      "UEP_TotalSize = 0x80\r\n"
      "[FSetProperty]\r\n"
      "ElementProp = 0x78\r\n"
      "SetLayout = 0x80\r\n"
      "UEP_TotalSize = 0x98\r\n";
static const GameFix kGameFixes[] = {
    { L"dungeons-wingdk-shipping.exe", L"minecraftdungeons2", L"Minecraft Dungeons II", kMD2Layout, nullptr },   // Xbox app
    { L"dungeons-win64-shipping.exe",  L"minecraftdungeons2", L"Minecraft Dungeons II", kMD2Layout, nullptr },   // Steam (not the first game: path must say II / 2)
    { L"hogwartslegacy.exe",                 L"", L"Hogwarts Legacy", nullptr, nullptr, kHLSettings },
    { L"oregon-wingdk-shipping.exe",         L"", L"High on Life", nullptr, nullptr, kHoLSettings },   // Xbox app
    { L"oregon-win64-shipping.exe",          L"", L"High on Life", nullptr, nullptr, kHoLSettings },   // Steam
    { L"southofmidnight.exe",                L"", L"South of Midnight", nullptr, kSoMSigs },
    { L"southofmidnight-win64-shipping.exe", L"", L"South of Midnight", nullptr, kSoMSigs },
    { L"southofmidnight-wingdk-shipping.exe",L"", L"South of Midnight", nullptr, kSoMSigs },
    { L"midnight-win64-shipping.exe",        L"", L"South of Midnight", nullptr, kSoMSigs },
    { L"midnight-wingdk-shipping.exe",       L"", L"South of Midnight", nullptr, kSoMSigs },
};
static void PrepareGameConfig(DWORD pid, const std::wstring& dllPath) {
    std::wstring exe = ProcPath(pid);
    if (exe.empty()) return;
    std::wstring lexe = Lower(exe), file = Lower(NameOf(exe));
    std::wstring flat; for (wchar_t c : lexe) if (c != L' ' && c != L'_' && c != L'-' && c != L'.') flat += c;
    const GameFix* fix = nullptr;
    for (auto& f : kGameFixes) if (file == f.exe && (flat.find(f.pathHas) != std::wstring::npos || flat.find(L"minecraftdungeonsii") != std::wstring::npos)) fix = &f;
    if (fix) Log(L"Game path: " + exe, LOG_INFO);
    if (!fix) return;
    // the folder UE4SS will look for. Store / Xbox app games run from C:\Program Files\WindowsApps\<package>\...
    // even when installed elsewhere (that's the path UE4SS sees), so for those it's the package name.
    std::wstring folder;
    {
        typedef LONG (WINAPI *GetPkg)(HANDLE, UINT32*, PWSTR);
        auto getPkg = (GetPkg)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetPackageFullName");
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (getPkg && ph) {
            wchar_t pkg[256]; UINT32 len = 256;
            if (getPkg(ph, &len, pkg) == 0 && pkg[0]) folder = pkg;
        }
        if (ph) CloseHandle(ph);
    }
    std::wstring folder2;   // the folder 3 above the exe, which UE4SS uses when the game isn't a Store package
    {
        std::wstring d = exe.substr(0, exe.find_last_of(L"\\/"));
        for (int i = 0; i < 3 && d.find_last_of(L"\\/") != std::wstring::npos; i++) d = d.substr(0, d.find_last_of(L"\\/"));
        folder2 = d.substr(d.find_last_of(L"\\/") + 1);
    }
    if (folder.empty()) {
        std::wstring d = exe.substr(0, exe.find_last_of(L"\\/"));
        for (int i = 0; i < 3 && d.find_last_of(L"\\/") != std::wstring::npos; i++) d = d.substr(0, d.find_last_of(L"\\/"));
        folder = d.substr(d.find_last_of(L"\\/") + 1);
    }
    std::wstring root = dllPath.substr(0, dllPath.find_last_of(L"\\/"));
    std::wstring cfg = root + L"\\" + folder;
    CreateDirectoryW(cfg.c_str(), nullptr);
    // settings: the normal ones, but keep using the shared Mods folder
    std::string ini;
    {
        HANDLE h = CreateFileW((root + L"\\UE4SS-settings.ini").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[65536]; DWORD n = 0;
            while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) ini.append(buf, n);
            CloseHandle(h);
        }
    }
    std::string mods; for (wchar_t c : root + L"\\Mods") mods += (char)(c < 128 ? c : '?');
    if (ini.empty()) ini = "[Overrides]\r\nModsFolderPath =\r\n";
    size_t k = ini.find("ModsFolderPath");
    if (k != std::string::npos) {
        size_t e = ini.find_first_of("\r\n", k);
        ini.replace(k, (e == std::string::npos ? ini.size() : e) - k, "ModsFolderPath = " + mods);
    } else ini = "[Overrides]\r\nModsFolderPath = " + mods + "\r\n" + ini;
    // per-game setting changes ("Key = value", several separated by \0)
    for (const char* kv = fix->settings; kv && *kv; kv += strlen(kv) + 1) {
        std::string line = kv, key = line.substr(0, line.find('='));
        while (!key.empty() && key.back() == ' ') key.pop_back();
        size_t at = 0; bool done = false;
        while ((at = ini.find(key, at)) != std::string::npos) {
            bool lineStart = at == 0 || ini[at - 1] == '\n';
            size_t after = at + key.size();
            while (after < ini.size() && ini[after] == ' ') after++;
            if (lineStart && after < ini.size() && ini[after] == '=') {
                size_t e = ini.find_first_of("\r\n", at);
                ini.replace(at, (e == std::string::npos ? ini.size() : e) - at, line);
                done = true; break;
            }
            at = after;
        }
        if (!done) ini += "\r\n" + line + "\r\n";
    }
    auto write = [](const std::wstring& p, const std::string& data) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD w = 0; bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
        CloseHandle(h); return ok;
    };
    auto writeAll = [&](const std::wstring& dir) {
        CreateDirectoryW(dir.c_str(), nullptr);
        bool good = write(dir + L"\\UE4SS-settings.ini", ini);
        if (fix->layout) good = write(dir + L"\\MemberVariableLayout.ini", fix->layout) && good;
        if (fix->sigs) {
            CreateDirectoryW((dir + L"\\UE4SS_Signatures").c_str(), nullptr);
            for (const GameSig* g = fix->sigs; g->file; g++) good = write(dir + L"\\UE4SS_Signatures\\" + g->file, g->lua) && good;
        }
        return good;
    };
    bool ok = writeAll(cfg);
    if (Lower(folder2) != Lower(folder) && Lower(folder2) != L"content") writeAll(root + L"\\" + folder2);
    Log(std::wstring(L"Using UE4SS fixes for ") + fix->name + (ok ? L"" : L" (couldn't write all of them)"), ok ? LOG_OK : LOG_WARN);
}

static bool InjectDll(DWORD pid, const std::wstring& dllPath) {
    if (!FileExists(dllPath)) { Log(L"UE4SS.dll not found at " + dllPath, LOG_ERR); return false; }
    { std::wstring pp = ProcPath(pid);
      if (int g = ClassicGen(pp)) { Log(NameOf(pp) + L" is an " + U2W(ClassicGenName(g)) + L" game. UE4SS only works with Unreal Engine 4 and 5, so it wasn't loaded (it can't be modded with this launcher).", LOG_WARN); return false; } }
    if (!ModuleLoaded(pid, NameOf(dllPath))) PrepareGameConfig(pid, dllPath);
    if (ModuleLoaded(pid, NameOf(dllPath))) { Log(L"UE4SS is already loaded in this game (PID " + std::to_wstring(pid) + L").", LOG_OK); return true; }
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        DWORD e = GetLastError();
        Log(L"Couldn't open the game process: " + ErrStr(e), LOG_ERR);
        if (e == ERROR_ACCESS_DENIED) Log(L"Try right-clicking the launcher and choosing 'Run as administrator'.", LOG_WARN);
        return false;
    }
    BOOL wow = FALSE; IsWow64Process(h, &wow);
    if (wow) { Log(L"That's a 32-bit process; UE4SS only works with 64-bit games.", LOG_ERR); CloseHandle(h); return false; }

    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { Log(L"VirtualAllocEx failed: " + ErrStr(GetLastError()), LOG_ERR); CloseHandle(h); return false; }
    bool ok = false, stillRunning = false;
    if (!WriteProcessMemory(h, remote, dllPath.c_str(), bytes, nullptr)) Log(L"WriteProcessMemory failed: " + ErrStr(GetLastError()), LOG_ERR);
    else {
        auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE t = CreateRemoteThread(h, nullptr, 0, loadLib, remote, 0, nullptr);
        if (!t) Log(L"CreateRemoteThread failed: " + ErrStr(GetLastError()), LOG_ERR);
        else {
            DWORD w = WaitForSingleObject(t, 20000);
            DWORD code = 0; GetExitCodeThread(t, &code);
            CloseHandle(t);
            if (w == WAIT_TIMEOUT) { ok = stillRunning = true; Log(L"UE4SS is still initialising...", LOG_INFO); }
            else if (code == 0) {
                Log(L"The game refused to load UE4SS.dll.", LOG_ERR);
                Log(L"Check the Visual C++ 2015-2022 x64 runtime is installed and the game has no anti-cheat.", LOG_WARN);
            } else ok = true;
        }
    }
    if (!stillRunning) VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    if (ok) Log(L"UE4SS loaded into PID " + std::to_wstring(pid) + L". Your mods will load in-game.", LOG_OK);
    return ok;
}

static std::mutex g_lastMx;
static std::wstring g_lastInjectedExe;

struct InjectJob { DWORD pid; std::wstring dll; DWORD delayMs; };
static DWORD WINAPI InjectThread(LPVOID p) {
    InjectJob* j = (InjectJob*)p;
    g_status = ST_INJECTING;
    if (j->delayMs) Sleep(j->delayMs);
    bool ok = InjectDll(j->pid, j->dll);
    g_status = ok ? ST_INJECTED : ST_FAILED;
    if (ok) { std::wstring p = ProcPath(j->pid); std::lock_guard<std::mutex> lk(g_lastMx); if (!p.empty()) g_lastInjectedExe = p; }
    delete j;
    if (g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0);
    return 0;
}
void InjectAsync(DWORD pid, DWORD delayMs) {
    HANDLE t = CreateThread(nullptr, 0, InjectThread, new InjectJob{ pid, DllPath(), delayMs }, 0, nullptr);
    if (t) CloseHandle(t);
}

std::vector<ProcInfo> EnumProcs() {
    std::vector<ProcInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snap, &pe)) do {
        if (pe.th32ProcessID <= 4 || pe.th32ProcessID == self) continue;
        ProcInfo pi{ pe.th32ProcessID, pe.szExeFile, L"", true, false };
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (h) {
            wchar_t path[MAX_PATH * 2]; DWORD sz = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(h, 0, path, &sz)) pi.path = path;
            BOOL wow = FALSE; if (IsWow64Process(h, &wow)) pi.is64 = !wow;
            CloseHandle(h);
        }
        pi.isUnreal = LooksUnreal(pi.name, pi.path);
        out.push_back(pi);
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    std::sort(out.begin(), out.end(), [](const ProcInfo& a, const ProcInfo& b) {
        if (a.isUnreal != b.isUnreal) return a.isUnreal;
        return Lower(a.name) < Lower(b.name);
    });
    return out;
}

static bool MatchesGame(const std::wstring& name, const std::wstring& path, const std::wstring& target) {
    if (target.empty()) return false;
    if (!path.empty() && Lower(path) == Lower(target)) return true;
    if (!LooksUnreal(name, path)) return false;
    if (Lower(name) == Lower(NameOf(target))) return true;
    return !path.empty() && StartsWithI(path, GameRoot(target) + L"\\");
}
bool IsWatching() { return g_autoInject || GetTickCount64() < g_watchUntil; }

void WatchTick() {
    if (!IsWatching()) { if (g_status == ST_WAITING) g_status = ST_IDLE; return; }
    std::wstring target = TargetExe();
    if (target.empty()) return;
    if (IsUnityGame(target)) { if (g_status == ST_WAITING) g_status = ST_IDLE; return; }   // MelonLoader loads itself
    if (ClassicGen(target)) { if (g_status == ST_WAITING) g_status = ST_IDLE; return; }    // Unreal 1-3: nothing to load
    if (g_status == ST_IDLE || g_status == ST_FAILED) g_status = ST_WAITING;
    std::wstring targetName = Lower(NameOf(target));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof(pe) };
    std::vector<std::pair<DWORD, std::wstring>> hits;
    if (Process32FirstW(snap, &pe)) do {
        if (std::find(g_handled.begin(), g_handled.end(), pe.th32ProcessID) != g_handled.end()) continue;
        std::wstring name = pe.szExeFile;
        if (Lower(name) != targetName && !LooksUnreal(name, L"")) continue;
        if (MatchesGame(name, ProcPath(pe.th32ProcessID), target)) hits.push_back({ pe.th32ProcessID, name });
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    // Wait until the game has opened its window before loading UE4SS: injecting the moment the process appears
    // is too early for games that unpack themselves first (Stray: UE4SS's scan finds nothing and stalls), and
    // games that restart themselves through Steam (Hogwarts Legacy) close that first process straight away.
    static std::map<DWORD, ULONGLONG> pending;          // pid -> when it was first seen
    static std::map<DWORD, std::wstring> pendingName;
    for (auto& h : hits) if (!pending.count(h.first)) {
        pending[h.first] = GetTickCount64(); pendingName[h.first] = h.second;
        Log(L"Game detected: " + h.second + L" (PID " + std::to_wstring(h.first) + L"). Waiting for its window...");
    }
    for (auto it = pending.begin(); it != pending.end();) {
        DWORD pid = it->first; ULONGLONG age = GetTickCount64() - it->second;
        HANDLE ph = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        bool alive = ph && WaitForSingleObject(ph, 0) == WAIT_TIMEOUT;
        if (ph) CloseHandle(ph);
        if (!alive) {                                    // it closed (a Steam relaunch): keep watching for the real one
            g_handled.push_back(pid); pendingName.erase(pid); it = pending.erase(it);
            g_watchUntil = std::max<ULONGLONG>(g_watchUntil, GetTickCount64() + 120000);
            continue;
        }
        struct W { DWORD pid; bool found; } w{ pid, false };
        EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
            W* w = (W*)lp; DWORD wp = 0; GetWindowThreadProcessId(hw, &wp);
            if (wp != w->pid || !IsWindowVisible(hw) || GetWindow(hw, GW_OWNER)) return TRUE;
            RECT r; GetWindowRect(hw, &r);
            if (r.right - r.left >= 320 && r.bottom - r.top >= 200) { w->found = true; return FALSE; }
            return TRUE;
        }, (LPARAM)&w);
        if (w.found || age > 90000) {
            g_handled.push_back(pid);
            Log(L"Loading UE4SS into " + pendingName[pid] + L" (PID " + std::to_wstring(pid) + L")...");
            InjectAsync(pid, 2000);
            pendingName.erase(pid); it = pending.erase(it);
            g_watchUntil = 0;
            continue;
        }
        ++it;
    }
}

bool LaunchGame() {
    std::wstring exe = Trim(g_gameExe);
    if (exe.empty() || !FileExists(exe)) { Log(L"Choose your game's .exe first.", LOG_WARN); return false; }
    g_handled.clear();
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, DirOf(exe).c_str(), &si, &pi)) {
        Log(L"Couldn't start the game: " + ErrStr(GetLastError()), LOG_ERR);
        return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (int g = ClassicGen(exe)) {
        Log(L"Starting " + ClassicGameName(exe) + L" (" + U2W(ClassicGenName(g)) + L"). UE4SS isn't used for Unreal 1-3 games.");
        return true;
    }
    if (IsUnityGame(exe)) {
        UnityInfo u = UnityDetect(exe);
        Log(L"Starting " + NameOf(exe) + (u.mlInstalled && !u.mlDisabled ? L"... MelonLoader loads with it." : L" (MelonLoader isn't on, so no mods will load)."));
        return true;
    }
    Log(L"Starting " + NameOf(exe) + L"... UE4SS will load as soon as the game appears.");
    g_watchUntil = GetTickCount64() + 180000;
    g_status = ST_WAITING;
    return true;
}

// ================================================================ mods
std::vector<ModInfo> ReadMods() {
    std::vector<ModInfo> mods;
    std::wstring txt; ReadUtf8(ModsDir() + L"\\mods.txt", txt);
    std::vector<std::pair<std::wstring, bool>> listed;
    for (auto& l : SplitLines(txt)) {
        if (l.find(L';') != std::wstring::npos) continue;
        size_t c = l.find(L':'); if (c == std::wstring::npos) continue;
        std::wstring n = Trim(l.substr(0, c)), v = Trim(l.substr(c + 1));
        if (!n.empty()) listed.push_back({ n, !v.empty() && v[0] == L'1' });
    }
    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((ModsDir() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            if (Lower(fd.cFileName) == L"shared") continue;
            dirs.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    auto add = [&](const std::wstring& n, bool en) {
        std::wstring d = ModsDir() + L"\\" + n;
        mods.push_back({ n, en || FileExists(d + L"\\enabled.txt"), DirExists(d + L"\\dlls") });
    };
    for (auto& l : listed) for (auto& d : dirs) if (Lower(d) == Lower(l.first)) { add(d, l.second); break; }
    for (auto& d : dirs) {
        bool seen = false;
        for (auto& l : listed) if (Lower(d) == Lower(l.first)) seen = true;
        if (!seen) add(d, false);
    }
    return mods;
}

void SetModEnabled(const std::wstring& name, bool en) {
    std::wstring path = ModsDir() + L"\\mods.txt", txt;
    ReadUtf8(path, txt);
    auto lines = SplitLines(txt);
    bool found = false;
    for (auto& l : lines) {
        if (l.find(L';') != std::wstring::npos) continue;
        size_t c = l.find(L':'); if (c == std::wstring::npos) continue;
        if (Lower(Trim(l.substr(0, c))) == Lower(name)) { l = name + L" : " + (en ? L"1" : L"0"); found = true; }
    }
    if (!found) {
        size_t at = lines.size();
        for (size_t i = 0; i < lines.size(); i++) if (Lower(lines[i]).find(L"keybinds") != std::wstring::npos) { at = i; break; }
        while (at > 0 && Trim(lines[at - 1]).empty()) at--;
        lines.insert(lines.begin() + at, name + L" : " + (en ? L"1" : L"0"));
    }
    WriteUtf8(path, JoinLines(lines));
    std::wstring et = ModsDir() + L"\\" + name + L"\\enabled.txt";
    if (!en && FileExists(et)) DeleteFileW(et.c_str());

    auto mods = ReadMods();
    std::wstring json = L"[\n";
    for (size_t i = 0; i < mods.size(); i++) {
        json += L"    {\n        \"mod_name\": \"" + mods[i].name + L"\",\n        \"mod_enabled\": " + (mods[i].enabled ? L"true" : L"false") + L"\n    }";
        json += (i + 1 < mods.size() ? L",\n" : L"\n");
    }
    WriteUtf8(ModsDir() + L"\\mods.json", json + L"]");
}

void ReregisterUserMods() {
    for (auto& mo : ReadMods()) {
        std::wstring txt; ReadUtf8(ModsDir() + L"\\mods.txt", txt);
        if (Lower(txt).find(Lower(mo.name)) == std::wstring::npos) SetModEnabled(mo.name, true);
    }
}

static void FindModDirs(const std::wstring& dir, std::vector<std::wstring>& out, int depth = 0) {
    if (depth > 5) return;
    if (FileExists(dir + L"\\Scripts\\main.lua") || FileExists(dir + L"\\dlls\\main.dll")) { out.push_back(dir); return; }
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') FindModDirs(dir + L"\\" + fd.cFileName, out, depth + 1); } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void FindPaks(const std::wstring& dir, std::vector<std::wstring>& out, int depth = 0) {
    if (depth > 8) return;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) FindPaks(p, out, depth + 1);
        else if (ExtOf(p) == L".pak") out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static int InstallModDir(const std::wstring& src, const std::wstring& fallbackName) {
    std::wstring name = NameOf(src);
    if (StartsWithI(name, L"ue4ss_mod_")) name = fallbackName;
    std::wstring dest = ModsDir() + L"\\" + name;
    if (DirExists(dest)) DeleteTree(dest);
    MakeDirs(dest);
    if (!CopyTree(src + L"\\*", dest)) { Log(L"Couldn't copy mod '" + name + L"'.", LOG_ERR); return 0; }
    SetModEnabled(name, true);
    Log(L"Installed and enabled " + name, LOG_OK);
    return 1;
}
int InstallPak(const std::wstring& pak, bool logicMods) {
    std::wstring paks = PaksDir();
    if (paks.empty()) { Log(L"Choose your game on the Play page first so the launcher knows where its Paks folder is.", LOG_WARN); return 0; }
    std::wstring dest = paks + (logicMods ? L"\\LogicMods" : L"\\~mods");
    MakeDirs(dest);
    int n = 0;
    std::wstring stem = pak.substr(0, pak.size() - 4);
    for (auto ext : { L".pak", L".utoc", L".ucas", L".sig" }) {
        std::wstring f = stem + ext;
        if (FileExists(f) && CopyFileW(f.c_str(), (dest + L"\\" + NameOf(f)).c_str(), FALSE)) n++;
    }
    if (n) Log(L"Installed " + NameOf(pak) + L" to " + (logicMods ? L"LogicMods (Blueprint mod)" : L"~mods"), LOG_OK);
    else Log(L"Couldn't copy " + NameOf(pak) + L" into the game folder (try running as administrator).", LOG_ERR);
    return n ? 1 : 0;
}
void InstallFromFolder(const std::wstring& folder, const std::wstring& fallbackName) {
    std::vector<std::wstring> mods, paks;
    FindModDirs(folder, mods);
    FindPaks(folder, paks);
    int n = 0;
    for (auto& m : mods) n += InstallModDir(m, fallbackName);
    for (auto& p : paks) n += InstallPak(p, Lower(p).find(L"logicmods") != std::wstring::npos);
    if (!n) Log(L"No UE4SS mod (Scripts\\main.lua or dlls\\main.dll) or .pak file found in " + NameOf(folder) + L".", LOG_WARN);
    g_modsDirty = true;
}
void InstallLua(const std::wstring& f) {
    std::wstring name = StemOf(f);
    if (Lower(name) == L"main") name = Lower(NameOf(DirOf(f))) == L"scripts" ? NameOf(DirOf(DirOf(f))) : NameOf(DirOf(f));
    std::wstring dir = ModsDir() + L"\\" + name + L"\\Scripts";
    MakeDirs(dir);
    CopyFileW(f.c_str(), (dir + L"\\main.lua").c_str(), FALSE);
    SetModEnabled(name, true);
    Log(L"Installed and enabled " + name, LOG_OK);
    g_modsDirty = true;
}
static DWORD WINAPI ZipThread(LPVOID p) {
    std::wstring* zip = (std::wstring*)p;
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring out = std::wstring(tmp) + L"ue4ss_mod_" + std::to_wstring(GetTickCount64());
    MakeDirs(out);
    Log(L"Extracting " + NameOf(*zip) + L"...");
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" + *zip + L"' -DestinationPath '" + out + L"' -Force\"";
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 120000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        InstallFromFolder(out, StemOf(*zip));
    } else Log(L"Couldn't run PowerShell to extract the zip: " + ErrStr(GetLastError()), LOG_ERR);
    DeleteTree(out);
    delete zip;
    g_busy = false;
    if (g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0);
    return 0;
}
void InstallZipAsync(const std::wstring& zip) {
    g_busy = true;
    HANDLE t = CreateThread(nullptr, 0, ZipThread, new std::wstring(zip), 0, nullptr);
    if (t) CloseHandle(t); else g_busy = false;
}

std::wstring FindInjectedGameExe() {
    {
        std::lock_guard<std::mutex> lk(g_lastMx);
        if (!g_lastInjectedExe.empty()) {
            // still running?
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            PROCESSENTRY32W pe{ sizeof(pe) }; bool alive = false;
            if (snap != INVALID_HANDLE_VALUE) {
                if (Process32FirstW(snap, &pe)) do {
                    if (Lower(pe.szExeFile) == Lower(NameOf(g_lastInjectedExe))) { alive = true; break; }
                } while (Process32NextW(snap, &pe));
                CloseHandle(snap);
            }
            if (alive) return g_lastInjectedExe;
        }
    }
    // Otherwise look for any Unreal game with UE4SS loaded (e.g. injected before the launcher was opened).
    for (auto& p : EnumProcs()) {
        if (!p.isUnreal || p.path.empty()) continue;
        if (ModuleLoaded(p.pid, L"UE4SS.dll")) return p.path;
    }
    return L"";
}

DWORD FindProcessIdByPath(const std::wstring& exe) {
    if (exe.empty()) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) }; DWORD found = 0;
    if (Process32FirstW(snap, &pe)) do {
        if (Lower(pe.szExeFile) != Lower(NameOf(exe))) continue;
        std::wstring p = ProcPath(pe.th32ProcessID);
        if (p.empty() || Lower(p) == Lower(exe)) { found = pe.th32ProcessID; break; }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}
