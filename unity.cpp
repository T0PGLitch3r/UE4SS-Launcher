#include "core.h"
#include "unity.h"
#include "updater.h"
#include <tlhelp32.h>
#include <thread>
#include <mutex>
#include <cstdio>

static std::mutex g_umx;
static UnityJob g_job;
static void JobStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_umx); g_job.status = s; }
static void JobFinish(const std::string& done, const std::string& err) {
    std::lock_guard<std::mutex> lk(g_umx);
    g_job.phase = UJ_IDLE; g_job.status.clear(); g_job.done = done; g_job.error = err;
}
UnityJob UnityGetJob() { std::lock_guard<std::mutex> lk(g_umx); return g_job; }
static bool JobStart() {
    std::lock_guard<std::mutex> lk(g_umx);
    if (g_job.phase == UJ_BUSY) return false;
    g_job = UnityJob(); g_job.phase = UJ_BUSY; return true;
}

static std::string FileVer(const std::wstring& path, bool product) {
    // read VS_FIXEDFILEINFO straight from the resource (no version.lib needed)
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return "";
    std::string out;
    if (HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), RT_VERSION)) {
        const BYTE* p = (const BYTE*)LockResource(LoadResource(m, r)); DWORD sz = SizeofResource(m, r);
        for (DWORD i = 0; p && i + sizeof(VS_FIXEDFILEINFO) <= sz; i += 4) {
            auto* fi = (const VS_FIXEDFILEINFO*)(p + i);
            if (fi->dwSignature == 0xFEEF04BD) {
                DWORD ms = product ? fi->dwProductVersionMS : fi->dwFileVersionMS, ls = product ? fi->dwProductVersionLS : fi->dwFileVersionLS;
                char b[64]; snprintf(b, sizeof b, "%u.%u.%u", HIWORD(ms), LOWORD(ms), HIWORD(ls));
                out = b; break;
            }
        }
    }
    FreeLibrary(m);
    return out;
}
static bool PeIs64(const std::wstring& exe) {
    HANDLE h = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return true;
    BYTE buf[1024]; DWORD n = 0; ReadFile(h, buf, sizeof buf, &n, nullptr); CloseHandle(h);
    if (n < 0x40) return true;
    DWORD pe = *(DWORD*)(buf + 0x3c);
    if (pe + 6 > n) return true;
    return *(WORD*)(buf + pe + 4) != 0x14c;   // IMAGE_FILE_MACHINE_I386
}
static std::wstring FirstMatch(const std::wstring& dir, const std::wstring& pattern) {
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring n = fd.cFileName; FindClose(h); return n;
}
static int MlMajorMinor(const std::string& v) { int a = 0, b = 0; sscanf(v.c_str(), "%d.%d", &a, &b); return a * 100 + b; }

#include <map>
static std::mutex g_cmx; static std::map<std::wstring, std::pair<ULONGLONG, UnityInfo>> g_cache;
static void CacheClear() { std::lock_guard<std::mutex> lk(g_cmx); g_cache.clear(); }
UnityInfo UnityDetect(const std::wstring& exe) {
    {
        std::lock_guard<std::mutex> lk(g_cmx);
        auto it = g_cache.find(exe);
        if (it != g_cache.end() && GetTickCount64() - it->second.first < 3000) return it->second.second;
    }
    UnityInfo u;
    std::wstring dir = DirOf(exe);
    u.gameDir = dir;
    bool player = FileExists(dir + L"\\UnityPlayer.dll");
    bool data = DirExists(dir + L"\\" + StemOf(exe) + L"_Data");
    u.isUnity = !exe.empty() && (player || data);
    if (u.isUnity) {
        u.il2cpp = FileExists(dir + L"\\GameAssembly.dll");
        u.x64 = PeIs64(exe);
        if (DirExists(dir + L"\\EasyAntiCheat") || FileExists(dir + L"\\start_protected_game.exe")) u.antiCheat = "Easy Anti-Cheat";
        else if (DirExists(dir + L"\\BattlEye")) u.antiCheat = "BattlEye";
        u.unityVersion = player ? FileVer(dir + L"\\UnityPlayer.dll", true) : FileVer(exe, true);
        bool vdll = FileExists(dir + L"\\version.dll"), off = FileExists(dir + L"\\version.dll.melonloader-off");
        u.mlInstalled = DirExists(dir + L"\\MelonLoader") && (vdll || off);
        u.mlDisabled = u.mlInstalled && !vdll;
        if (u.mlInstalled) {
            for (auto sub : { L"\\MelonLoader\\net6\\MelonLoader.dll", L"\\MelonLoader\\net35\\MelonLoader.dll", L"\\MelonLoader\\MelonLoader.dll" })
                if (FileExists(dir + sub)) { u.mlVersion = FileVer(dir + sub, false); break; }
        }
        // the matching UnityExplorer build
        if (!u.il2cpp) u.explorerWant = "UnityExplorer.MelonLoader.Mono.zip";
        else if (u.mlVersion.empty() || MlMajorMinor(u.mlVersion) >= 6) u.explorerWant = "UnityExplorer.MelonLoader.IL2CPP.CoreCLR.zip";
        else u.explorerWant = "UnityExplorer.MelonLoader.IL2CPP.zip";
        std::wstring f = FirstMatch(dir + L"\\Mods", L"UnityExplorer*.dll");
        u.explorerFile = W2U(f);
        if (!f.empty()) {
            std::wstring lf = Lower(f);
            bool mono = lf.find(L"mono") != std::wstring::npos, core = lf.find(L"coreclr") != std::wstring::npos;
            bool ml = lf.find(L".ml.") != std::wstring::npos || lf.find(L"melonloader") != std::wstring::npos;
            if (!ml) u.explorerWrong = true;                               // BepInEx / standalone build
            else if (!u.il2cpp) u.explorerWrong = !mono;
            else if (u.explorerWant.find("CoreCLR") != std::string::npos) u.explorerWrong = mono || !core;
            else u.explorerWrong = mono || core;
        }
    }
    { std::lock_guard<std::mutex> lk(g_cmx); g_cache[exe] = { GetTickCount64(), u }; }
    return u;
}
bool IsUnityGame(const std::wstring& exe) { return UnityDetect(exe).isUnity; }

bool UnityGameRunning(const std::wstring& exe) {
    std::wstring want = Lower(exe);
    for (auto& p : EnumProcs()) if (Lower(p.path) == want) return true;
    return false;
}

// newest release's asset URL with exactly this file name (or "" if none)
static std::string AssetUrl(const std::string& json, const std::string& name, std::string* tag) {
    if (tag) {
        size_t t = json.find("\"tag_name\"");
        if (t != std::string::npos) { size_t a = json.find('"', json.find(':', t) + 1), b = json.find('"', a + 1); *tag = json.substr(a + 1, b - a - 1); }
    }
    const std::string key = "\"browser_download_url\"";
    for (size_t p = json.find(key); p != std::string::npos; p = json.find(key, p + 1)) {
        size_t q1 = json.find('"', json.find(':', p + key.size()) + 1), q2 = json.find('"', q1 + 1);
        std::string url = json.substr(q1 + 1, q2 - q1 - 1);
        if (url.size() > name.size() && url.compare(url.size() - name.size(), name.size(), name) == 0 && url[url.size() - name.size() - 1] == '/') return url;
    }
    return "";
}
static bool Download(const std::string& repo, const std::string& asset, std::string& data, std::string& tag, std::string& err) {
    std::string json;
    if (!NetGet(U2W("https://api.github.com/repos/" + repo + "/releases/latest"), json, err)) return false;
    std::string url = AssetUrl(json, asset, &tag);
    if (url.empty()) { err = "the newest " + repo + " release has no " + asset; return false; }
    JobStatus("Downloading " + asset + " (" + tag + ")...");
    return NetGet(U2W(url), data, err);
}

void UnityInstallMelonLoader(const std::wstring& exe) {
    if (!JobStart()) return;
    std::thread([exe] {
        UnityInfo u = UnityDetect(exe);
        if (!u.isUnity) { JobFinish("", "That isn't a Unity game."); return; }
        if (!u.antiCheat.empty()) { JobFinish("", "This game uses " + u.antiCheat + ", which blocks MelonLoader (and modding it online can get your account banned)."); return; }
        if (UnityGameRunning(exe)) { JobFinish("", "Close the game first, then install MelonLoader."); return; }
        JobStatus("Finding the newest MelonLoader...");
        std::string data, tag, err;
        if (!Download("LavaGang/MelonLoader", u.x64 ? "MelonLoader.x64.zip" : "MelonLoader.x86.zip", data, tag, err)) { JobFinish("", "Couldn't download MelonLoader: " + err); return; }
        JobStatus("Installing MelonLoader " + tag + "...");
        // a turned-off install: put version.dll back where the new one goes
        DeleteFileW((u.gameDir + L"\\version.dll.melonloader-off").c_str());
        if (!ZipExtract(data, u.gameDir)) { JobFinish("", "Couldn't unpack MelonLoader into the game folder (is it read-only, or is the game running?)."); return; }
        CacheClear();
        Log(L"MelonLoader " + U2W(tag) + L" installed in " + u.gameDir + L". It loads when the game starts; the first start takes longer while it sets up.", LOG_OK);
        JobFinish("MelonLoader " + tag + " installed", "");
    }).detach();
}

void UnityInstallExplorer(const std::wstring& exe) {
    if (!JobStart()) return;
    std::thread([exe] {
        UnityInfo u = UnityDetect(exe);
        if (!u.isUnity) { JobFinish("", "That isn't a Unity game."); return; }
        if (!u.mlInstalled) { JobFinish("", "Install MelonLoader first."); return; }
        if (UnityGameRunning(exe)) { JobFinish("", "Close the game first, then install UnityExplorer."); return; }
        JobStatus("Finding the UnityExplorer build for this game...");
        std::string data, tag, err;
        if (!Download("yukieiji/UnityExplorer", u.explorerWant, data, tag, err)) { JobFinish("", "Couldn't download UnityExplorer: " + err); return; }
        // take out any other UnityExplorer / UniverseLib build first, so only the matching one loads
        for (auto sub : { L"\\Mods", L"\\UserLibs" }) {
            std::wstring d = u.gameDir + sub;
            WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((d + L"\\*.dll").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                std::wstring n = Lower(fd.cFileName);
                if (n.rfind(L"unityexplorer", 0) == 0 || n.rfind(L"universelib", 0) == 0) DeleteFileW((d + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        JobStatus("Installing " + u.explorerWant + "...");
        if (!ZipExtract(data, u.gameDir)) { JobFinish("", "Couldn't unpack UnityExplorer into the game folder."); return; }
        CacheClear();
        Log(L"UnityExplorer " + U2W(tag) + L" (" + U2W(u.explorerWant) + L") installed. Press F7 in the game to show or hide it.", LOG_OK);
        JobFinish("UnityExplorer " + tag + " installed", "");
    }).detach();
}

void UnitySetMelonLoaderOn(const std::wstring& exe, bool on) {
    UnityInfo u = UnityDetect(exe);
    if (!u.mlInstalled) return;
    std::wstring a = u.gameDir + L"\\version.dll", b = u.gameDir + L"\\version.dll.melonloader-off";
    bool ok = on ? MoveFileExW(b.c_str(), a.c_str(), MOVEFILE_REPLACE_EXISTING) : MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING);
    CacheClear();
    if (ok) Log(on ? L"MelonLoader turned on: it loads next time the game starts." : L"MelonLoader turned off: the game starts without mods.", LOG_OK);
    else Log(L"Couldn't switch MelonLoader (close the game first).", LOG_WARN);
}

void UnityRemoveMelonLoader(const std::wstring& exe) {
    UnityInfo u = UnityDetect(exe);
    if (!u.mlInstalled) return;
    if (UnityGameRunning(exe)) { Log(L"Close the game first, then remove MelonLoader.", LOG_WARN); return; }
    DeleteFileW((u.gameDir + L"\\version.dll").c_str());
    DeleteFileW((u.gameDir + L"\\version.dll.melonloader-off").c_str());
    DeleteFolderTree(u.gameDir + L"\\MelonLoader");
    CacheClear();
    Log(L"MelonLoader removed. Your Mods, Plugins, UserLibs and UserData folders were left in place.", LOG_OK);
}
