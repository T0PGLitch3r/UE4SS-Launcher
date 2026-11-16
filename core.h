#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

// ---- logging
enum LogLevel { LOG_INFO, LOG_OK, LOG_WARN, LOG_ERR };
struct LogLine { std::string time, text; int level; };
extern std::mutex g_logMx;
extern std::vector<LogLine> g_logs;
void Log(const std::wstring& msg, int level = LOG_INFO);

// ---- status of the game / injection
enum Status { ST_IDLE, ST_WAITING, ST_INJECTING, ST_INJECTED, ST_FAILED };
extern std::atomic<int> g_status;

// ---- strings & files
std::string W2U(const std::wstring& w);
std::wstring U2W(const std::string& s);
std::wstring Lower(std::wstring s);
std::wstring Trim(const std::wstring& s);
bool FileExists(const std::wstring& p);
bool DirExists(const std::wstring& p);
std::wstring DirOf(const std::wstring& p);
std::wstring NameOf(const std::wstring& p);
std::wstring StemOf(const std::wstring& p);
std::wstring ExtOf(const std::wstring& p);
std::wstring ExeDir();
void MakeDirs(const std::wstring& p);
void OpenFolder(const std::wstring& p);
void OpenUrl(const wchar_t* url);

// ---- UE4SS files
extern std::wstring g_dataDir, g_iniPath;
void SetupDataDir(HINSTANCE inst);
int ExtractBundle(bool force);
std::wstring DllPath();
std::wstring ModsDir();
bool RuntimeMissing();
bool IniGetBool(const std::wstring& key);
void IniSet(const std::vector<std::pair<std::wstring, std::wstring>>& kv);

// ---- game
extern std::wstring g_gameExe;   // what the user picked
std::wstring TargetExe();        // the real *-Shipping.exe (cached)
void InvalidateTarget();
std::wstring PaksDir();
std::wstring GameDisplayName();
bool LooksUnreal(const std::wstring& name, const std::wstring& path);

// ---- processes / injection
struct ProcInfo { DWORD pid; std::wstring name, path; bool is64, isUnreal; };
std::vector<ProcInfo> EnumProcs();
void InjectAsync(DWORD pid, DWORD delayMs);
bool LaunchGame();
void WatchTick();           // call ~every 500ms while watching
bool IsWatching();
extern bool g_autoInject;
extern ULONGLONG g_watchUntil;
extern std::vector<DWORD> g_handled;
extern HWND g_hwnd;
// The running game that has UE4SS.dll loaded (prefers the last one we injected); empty if none.
std::wstring FindInjectedGameExe();
DWORD FindProcessIdByPath(const std::wstring& exe);

// ---- mods
struct ModInfo { std::wstring name; bool enabled; bool isCpp; };
std::vector<ModInfo> ReadMods();
void SetModEnabled(const std::wstring& name, bool en);
int InstallPak(const std::wstring& pak, bool logicMods);
void InstallFromFolder(const std::wstring& folder, const std::wstring& fallbackName);
void InstallLua(const std::wstring& f);
void InstallZipAsync(const std::wstring& zip);
void ReregisterUserMods();
extern std::atomic<bool> g_modsDirty;
extern std::atomic<bool> g_busy;
bool IsUnityGame(const std::wstring& exe);   // a Unity game (not supported: Unreal only)
