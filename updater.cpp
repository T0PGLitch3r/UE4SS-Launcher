// UE4SS auto-update: checks GitHub for a newer UE4SS, downloads the release zip, and installs it
// into the launcher's UE4SS folder while keeping the user's settings, mod on/off choices and the
// launcher's own LauncherBridge mod. The previous UE4SS is kept so the update can be undone.
#include "core.h"
#include "updater.h"
extern "C" {
#include "puff.h"   // zlib's small inflate (contrib/puff, zlib licence)
}
#include <winhttp.h>
#include <shellapi.h>
#include <thread>
#include <sstream>
#include <algorithm>
#include <map>
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#include <cstdio>

// The UE4SS build packed into this launcher (experimental-latest, identical UE4SS.dll).
static const char* kBundled = "v3.0.1-1196-g236adf26";
static const wchar_t* kRepoApi = L"/repos/UE4SS-RE/RE-UE4SS/releases";

static std::mutex g_mx;
static UpdInfo g_info;
static std::string g_latestUrl;
static bool g_autoInstallPending = false;
static std::atomic<bool> g_working{ false };

const char* BundledUE4SSVersion() { return kBundled; }

static std::wstring MarkerPath() { return g_dataDir + L"\\ue4ss_version.txt"; }
static std::wstring PrevDir() { return g_dataDir + L"\\_previous_ue4ss"; }

static bool ReadText(const std::wstring& p, std::string& out) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(f, nullptr), rd = 0;
    out.assign(sz, '\0'); if (sz) ReadFile(f, &out[0], sz, &rd, nullptr);
    out.resize(rd); CloseHandle(f);
    return true;
}
static bool WriteText(const std::wstring& p, const std::string& s) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0; bool ok = WriteFile(f, s.data(), (DWORD)s.size(), &wr, nullptr) && wr == s.size();
    CloseHandle(f); return ok;
}
static std::string TrimA(std::string s) {
    while (!s.empty() && strchr(" \t\r\n", s.back())) s.pop_back();
    size_t a = 0; while (a < s.size() && strchr(" \t\r\n", s[a])) a++;
    return s.substr(a);
}

std::string InstalledUE4SSVersion() {
    std::string v;
    if (ReadText(MarkerPath(), v) && !TrimA(v).empty()) return TrimA(v);
    return kBundled;
}

// "v3.0.1-1196-gabc" -> {3,0,1,1196}; "v3.0.1" -> {3,0,1,0}
static bool ParseVer(const std::string& s, int out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    const char* p = s.c_str(); if (*p == 'v' || *p == 'V') p++;
    int n = sscanf(p, "%d.%d.%d-%d", &out[0], &out[1], &out[2], &out[3]);
    return n >= 3;
}
static int CmpVer(const std::string& a, const std::string& b) {
    int x[4], y[4];
    if (!ParseVer(a, x) || !ParseVer(b, y)) return a == b ? 0 : -2;   // can't compare
    for (int i = 0; i < 4; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

bool UpdKeepInstalledDll() {
    std::string v;
    if (!ReadText(MarkerPath(), v) || TrimA(v).empty()) return false;
    return CmpVer(TrimA(v), kBundled) >= 0;   // an update at least as new as the bundled build is installed
}
void UpdForgetInstalled() { DeleteFileW(MarkerPath().c_str()); }

UpdInfo UpdGet() { std::lock_guard<std::mutex> lk(g_mx); return g_info; }
static void SetPhase(int ph) { std::lock_guard<std::mutex> lk(g_mx); g_info.phase = ph; }
static void SetError(const std::string& e) { std::lock_guard<std::mutex> lk(g_mx); g_info.error = e; g_info.phase = UPD_IDLE; }

// ---- HTTPS (WinHTTP; follows GitHub's redirect to its download servers)
static bool HttpGetVia(DWORD access, const std::wstring& url, std::string& out, std::string& err, bool showProgress, bool& connected);
// direct (or the machine's WinHTTP proxy) first; if that can't connect, try the system proxy settings
static bool HttpGet(const std::wstring& url, std::string& out, std::string& err, bool showProgress) {
    bool connected = false;
    if (HttpGetVia(WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, url, out, err, showProgress, connected) || connected) return err.empty();
    std::string err2;
    if (HttpGetVia(WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, url, out, err2, showProgress, connected)) { err.clear(); return true; }
    if (connected) err = err2;
    return false;
}
static bool HttpGetVia(DWORD access, const std::wstring& url, std::string& out, std::string& err, bool showProgress, bool& connected) {
    err.clear(); connected = false;
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    wchar_t extra[1024] = {}; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "bad address"; return false; }
    HINTERNET s = WinHttpOpen(L"UE4SS-Launcher", access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = "couldn't start a connection"; return false; }
    WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", (std::wstring(path) + extra).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json, */*\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        connected = true;
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        DWORD total = 0; len = sizeof(total);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX);
        out.clear(); if (total) out.reserve(total);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(r, &avail) && avail) {
            size_t at = out.size(); out.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(r, &out[at], avail, &got)) break;
            out.resize(at + got);
            if (showProgress && total) { std::lock_guard<std::mutex> lk(g_mx); g_info.progress = (float)out.size() / total; }
        }
        if (code == 200) ok = true;
        else if (code == 403 || code == 429) err = "GitHub is limiting requests right now; try again in a while";
        else err = "GitHub answered " + std::to_string(code);
    } else err = "no connection to GitHub (check your internet connection)";
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// Every "browser_download_url" in a GitHub release; the UE4SS one is UE4SS_v*.zip (not zDEV-/zCustom...).
static bool FindUE4SSAsset(const std::string& json, std::string& url, std::string& ver) {
    const std::string key = "\"browser_download_url\"";
    for (size_t p = json.find(key); p != std::string::npos; p = json.find(key, p + 1)) {
        size_t q1 = json.find('"', json.find(':', p + key.size()) + 1);
        size_t q2 = json.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) break;
        std::string u = json.substr(q1 + 1, q2 - q1 - 1), clean;
        for (size_t i = 0; i < u.size(); i++) { if (u[i] == '\\' && i + 1 < u.size()) i++; clean += u[i]; }
        std::string name = clean.substr(clean.find_last_of('/') + 1);
        if (name.rfind("UE4SS_v", 0) == 0 && name.size() > 11 && name.compare(name.size() - 4, 4, ".zip") == 0) {
            url = clean; ver = name.substr(6, name.size() - 10);   // "UE4SS_v3.0.1-1196-g...zip" -> "v3.0.1-1196-g..."
            return true;
        }
    }
    return false;
}

static void InstallWorker();

void UpdCheck(int channel, bool installIfNewer) {
    if (g_working.exchange(true)) return;
    { std::lock_guard<std::mutex> lk(g_mx); g_info.phase = UPD_CHECKING; g_info.error.clear(); }
    std::thread([channel, installIfNewer] {
        std::string body, err, url, ver;
        // (UE4SS_LAUNCHER_UPDATE_API can point this at a test server)
        wchar_t base[512] = L"https://api.github.com";
        GetEnvironmentVariableW(L"UE4SS_LAUNCHER_UPDATE_API", base, 512);
        std::wstring api = std::wstring(base) + kRepoApi + (channel == 1 ? L"/latest" : L"/tags/experimental-latest");
        if (!HttpGet(api, body, err, false)) { SetError("Couldn't check for updates: " + err + "."); g_working = false; return; }
        if (!FindUE4SSAsset(body, url, ver)) { SetError("Couldn't find a UE4SS download in the latest release."); g_working = false; return; }
        std::string inst = InstalledUE4SSVersion();
        bool newer = CmpVer(ver, inst) > 0;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_info.phase = UPD_IDLE; g_info.checked = true; g_info.latest = ver; g_info.available = newer; g_latestUrl = url;
        }
        if (!newer) { g_working = false; return; }
        Log(L"A newer UE4SS is available: " + U2W(ver) + L" (you have " + U2W(inst) + L").", LOG_INFO);
        if (installIfNewer) {
            if (!FindInjectedGameExe().empty()) {
                std::lock_guard<std::mutex> lk(g_mx); g_info.waitingForGame = true; g_autoInstallPending = true;
                Log(L"The UE4SS update will install once you close the game.", LOG_INFO);
            } else { InstallWorker(); return; }
        }
        g_working = false;
    }).detach();
}

// ---- install helpers
static bool RunHidden(std::wstring cmd) {
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}
static void DeleteTreeW(const std::wstring& path) {
    std::wstring f = path + L'\0';
    SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = f.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationW(&op);
}
// Built-in .zip reader (stored and deflate entries), so nothing external is needed.
static bool UnzipBuiltIn(const std::string& z, const std::wstring& out) {
    auto u16 = [&](size_t o) { return o + 2 <= z.size() ? (unsigned)((unsigned char)z[o] | (unsigned char)z[o + 1] << 8) : 0u; };
    auto u32 = [&](size_t o) { return o + 4 <= z.size() ? (unsigned)u16(o) | (unsigned)u16(o + 2) << 16 : 0u; };
    if (z.size() < 22) return false;
    size_t eocd = std::string::npos;
    for (size_t i = z.size() - 22 + 1; i-- > 0 && z.size() - i < 70000;) if (u32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) return false;
    unsigned count = u16(eocd + 10); size_t cd = u32(eocd + 16);
    int written = 0;
    for (unsigned n = 0; n < count; n++) {
        if (u32(cd) != 0x02014b50) return false;
        unsigned method = u16(cd + 10), csize = u32(cd + 20), usize = u32(cd + 24), nlen = u16(cd + 28), xlen = u16(cd + 30), clen = u16(cd + 32);
        size_t lho = u32(cd + 42);
        std::string name = z.substr(cd + 46, nlen);
        cd += 46 + nlen + xlen + clen;
        for (auto& c : name) if (c == '/') c = '\\';
        if (name.empty() || name.find("..") != std::string::npos || name[0] == '\\' || name.find(':') != std::string::npos) continue;   // never write outside the folder
        std::wstring dest = out + L"\\" + U2W(name);
        if (name.back() == '\\') { MakeDirs(dest); continue; }
        if (u32(lho) != 0x04034b50) return false;
        size_t data = lho + 30 + u16(lho + 26) + u16(lho + 28);
        if (data + csize > z.size()) return false;
        std::string buf(usize, '\0');
        if (method == 0) { if (csize != usize) return false; memcpy(&buf[0], z.data() + data, usize); }
        else if (method == 8) {
            unsigned long dl = usize, sl = csize;
            if (puff(usize ? (unsigned char*)&buf[0] : nullptr, &dl, (const unsigned char*)z.data() + data, &sl) != 0 || dl != usize) return false;
        } else return false;
        MakeDirs(DirOf(dest));
        if (!WriteText(dest, buf)) return false;
        written++;
    }
    return written > 0;
}
static bool Unzip(const std::wstring& zip, const std::wstring& out) {
    MakeDirs(out);
    std::string z;
    if (ReadText(zip, z) && UnzipBuiltIn(z, out)) return true;
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    if (FileExists(tar) && RunHidden(L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + out + L"\"")) return true;
    return RunHidden(L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" + zip + L"' -DestinationPath '" + out + L"' -Force\"");
}
// folder inside the extracted zip that holds UE4SS.dll (ue4ss\ in v3+, the root in older releases)
static std::wstring FindDllDir(const std::wstring& root) {
    if (FileExists(root + L"\\ue4ss\\UE4SS.dll")) return root + L"\\ue4ss";
    if (FileExists(root + L"\\UE4SS.dll")) return root;
    return L"";
}
static void ListFiles(const std::wstring& dir, const std::wstring& rel, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName; if (n == L"." || n == L"..") continue;
        std::wstring r = rel.empty() ? n : rel + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListFiles(dir + L"\\" + n, r, out); else out.push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// UE4SS-settings.ini: take the new file, but keep the value of every setting the old file also has.
static std::string MergeSettings(const std::string& newer, const std::string& older) {
    auto keyOf = [](const std::string& line, std::string& k) {
        std::string t = TrimA(line);
        if (t.empty() || t[0] == ';' || t[0] == '[') return false;
        size_t eq = t.find('='); if (eq == std::string::npos) return false;
        k = TrimA(t.substr(0, eq)); return !k.empty();
    };
    std::map<std::string, std::string> old;   // "section|key" -> whole line
    std::string sec, line, k;
    { std::istringstream ss(older); while (std::getline(ss, line)) { std::string t = TrimA(line); if (!t.empty() && t[0] == '[') sec = t; else if (keyOf(line, k)) old[sec + "|" + k] = line; } }
    std::ostringstream o; sec.clear();
    std::istringstream ss(newer);
    while (std::getline(ss, line)) {
        bool cr = !line.empty() && line.back() == '\r'; if (cr) line.pop_back();
        std::string t = TrimA(line);
        if (!t.empty() && t[0] == '[') sec = t;
        else if (keyOf(line, k)) { auto it = old.find(sec + "|" + k); if (it != old.end()) { line = it->second; if (!line.empty() && line.back() == '\r') line.pop_back(); } }
        o << line << (cr ? "\r\n" : "\n");
    }
    return o.str();
}
// mods.txt ("Name : 1"): keep existing lines, add mods that are new in this release.
static std::string MergeModsTxt(const std::string& newer, const std::string& older) {
    std::string out = older; if (!out.empty() && out.back() != '\n') out += "\r\n";
    std::istringstream ss(newer); std::string line;
    while (std::getline(ss, line)) {
        size_t c = line.find(':'); if (c == std::string::npos) continue;
        std::string name = TrimA(line.substr(0, c)); if (name.empty() || name[0] == ';') continue;
        bool have = false;
        std::istringstream os(older); std::string ol;
        while (std::getline(os, ol)) { size_t oc = ol.find(':'); if (oc != std::string::npos && _stricmp(TrimA(ol.substr(0, oc)).c_str(), name.c_str()) == 0) { have = true; break; } }
        if (!have) { std::string l = TrimA(line); out += l + "\r\n"; }
    }
    return out;
}
// mods.json ([{"mod_name": "...", "mod_enabled": true}, ...]): same idea.
static std::string MergeModsJson(const std::string& newer, const std::string& older) {
    std::string out = older;
    size_t p = 0;
    while ((p = newer.find('{', p)) != std::string::npos) {
        size_t e = newer.find('}', p); if (e == std::string::npos) break;
        std::string obj = newer.substr(p, e - p + 1); p = e + 1;
        size_t n = obj.find("\"mod_name\""); if (n == std::string::npos) continue;
        size_t q1 = obj.find('"', obj.find(':', n) + 1), q2 = obj.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) continue;
        std::string name = "\"" + obj.substr(q1 + 1, q2 - q1 - 1) + "\"";
        if (out.find(name) != std::string::npos) continue;
        size_t close = out.find_last_of(']'); if (close == std::string::npos) continue;
        size_t lastObj = out.find_last_of('}', close);
        out.insert(close, std::string(lastObj != std::string::npos ? ",\n    " : "\n    ") + obj + "\n");
    }
    return out;
}

static bool CopyOver(const std::wstring& from, const std::wstring& to) {
    MakeDirs(DirOf(to));
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

static void InstallWorker() {
    std::string url, ver;
    { std::lock_guard<std::mutex> lk(g_mx); url = g_latestUrl; ver = g_info.latest; g_info.phase = UPD_DOWNLOADING; g_info.progress = 0; g_info.error.clear(); g_info.waitingForGame = false; }
    g_autoInstallPending = false;
    auto done = [](const std::string& err) { if (!err.empty()) { SetError(err); Log(U2W(err), LOG_ERR); } g_working = false; };
    if (url.empty()) { done("Check for updates first."); return; }
    if (!FindInjectedGameExe().empty()) { done("Close the game first: it's using UE4SS, so it can't be updated right now."); return; }
    Log(L"Downloading UE4SS " + U2W(ver) + L"...", LOG_INFO);
    std::string zip, err;
    if (!HttpGet(U2W(url), zip, err, true) || zip.size() < 1000) { done("Couldn't download UE4SS: " + (err.empty() ? std::string("the download was empty") : err) + "."); return; }
    SetPhase(UPD_INSTALLING);
    std::wstring work = g_dataDir + L"\\_update_tmp";
    DeleteTreeW(work); MakeDirs(work);
    std::wstring zipPath = work + L"\\ue4ss.zip", out = work + L"\\x";
    if (!WriteText(zipPath, zip)) { done("Couldn't save the download (is the launcher's folder read-only?)."); return; }
    if (!Unzip(zipPath, out)) { DeleteTreeW(work); done("Couldn't unpack the UE4SS download."); return; }
    std::wstring src = FindDllDir(out);
    if (src.empty()) { DeleteTreeW(work); done("The download didn't contain UE4SS.dll."); return; }

    // keep the current UE4SS so the update can be undone
    std::wstring prev = PrevDir();
    DeleteTreeW(prev); MakeDirs(prev);
    CopyOver(g_dataDir + L"\\UE4SS.dll", prev + L"\\UE4SS.dll");
    CopyOver(g_dataDir + L"\\UE4SS-settings.ini", prev + L"\\UE4SS-settings.ini");
    WriteText(prev + L"\\version.txt", InstalledUE4SSVersion());

    // UE4SS.dll first: if it's locked, nothing else has changed yet
    if (!CopyOver(src + L"\\UE4SS.dll", g_dataDir + L"\\UE4SS.dll")) { DeleteTreeW(work); done("Couldn't replace UE4SS.dll. Close the game (and anything else using it) and try again."); return; }
    std::vector<std::wstring> files; ListFiles(src, L"", files);
    int copied = 1;
    for (auto& rel : files) {
        std::wstring low = Lower(rel);
        if (low == L"ue4ss.dll") continue;
        if (low.rfind(L"mods\\launcherbridge\\", 0) == 0) continue;           // the launcher's own mod
        std::wstring from = src + L"\\" + rel, to = g_dataDir + L"\\" + rel;
        if (low == L"ue4ss-settings.ini" || low == L"mods\\mods.txt" || low == L"mods\\mods.json") {
            std::string nw, ol; ReadText(from, nw);
            if (!ReadText(to, ol)) { CopyOver(from, to); copied++; continue; }
            std::string merged = low == L"ue4ss-settings.ini" ? MergeSettings(nw, ol) : low == L"mods\\mods.txt" ? MergeModsTxt(nw, ol) : MergeModsJson(nw, ol);
            if (WriteText(to, merged)) copied++;
            continue;
        }
        if (CopyOver(from, to)) copied++;
    }
    WriteText(MarkerPath(), ver);
    DeleteTreeW(work);
    { std::lock_guard<std::mutex> lk(g_mx); g_info.phase = UPD_IDLE; g_info.available = false; g_info.progress = 1; }
    Log(L"UE4SS updated to " + U2W(ver) + L" (" + std::to_wstring(copied) + L" files). Your settings and mods were kept; it's used next time a game starts.", LOG_OK);
    g_working = false;
}

void UpdInstall() {
    if (g_working.exchange(true)) return;
    std::thread(InstallWorker).detach();
}

bool UpdCanUndo() { return FileExists(PrevDir() + L"\\UE4SS.dll"); }

void UpdUndo() {
    if (!UpdCanUndo()) return;
    if (!FindInjectedGameExe().empty()) { Log(L"Close the game first: it's using UE4SS.", LOG_WARN); return; }
    std::wstring prev = PrevDir();
    if (!CopyFileW((prev + L"\\UE4SS.dll").c_str(), (g_dataDir + L"\\UE4SS.dll").c_str(), FALSE)) { Log(L"Couldn't put the previous UE4SS back (is a game using it?).", LOG_ERR); return; }
    CopyFileW((prev + L"\\UE4SS-settings.ini").c_str(), (g_dataDir + L"\\UE4SS-settings.ini").c_str(), FALSE);
    std::string v; ReadText(prev + L"\\version.txt", v); v = TrimA(v);
    if (v.empty() || v == kBundled) UpdForgetInstalled(); else WriteText(MarkerPath(), v);
    DeleteTreeW(prev);
    { std::lock_guard<std::mutex> lk(g_mx); g_info.checked = false; g_info.available = false; }
    Log(L"Went back to UE4SS " + U2W(v.empty() ? std::string(kBundled) : v) + L".", LOG_OK);
}

void UpdTick() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (!g_autoInstallPending || now - last < 5000) return;
    last = now;
    if (!FindInjectedGameExe().empty() || g_working.exchange(true)) return;
    g_autoInstallPending = false;
    std::thread(InstallWorker).detach();
}

// ---- shared with the Unity / MelonLoader installer
bool NetGet(const std::wstring& url, std::string& out, std::string& err) { return HttpGet(url, out, err, false); }
bool ZipExtract(const std::string& zipData, const std::wstring& outDir) { MakeDirs(outDir); return UnzipBuiltIn(zipData, outDir); }
void DeleteFolderTree(const std::wstring& path) { DeleteTreeW(path); }
