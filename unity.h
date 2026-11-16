#pragma once
#include <string>

// Unity games: MelonLoader (https://github.com/LavaGang/MelonLoader) and the UnityExplorer build that matches
// the game (https://github.com/yukieiji/UnityExplorer). MelonLoader loads itself when the game starts
// (through version.dll next to the game's exe), so nothing is injected.
struct UnityInfo {
    bool isUnity = false;
    bool il2cpp = false;            // GameAssembly.dll: IL2CPP build, otherwise Mono
    bool x64 = true;
    std::string unityVersion;       // from UnityPlayer.dll, e.g. 2021.3.16
    std::wstring gameDir;
    bool mlInstalled = false;       // version.dll + MelonLoader\ next to the exe
    bool mlDisabled = false;        // version.dll renamed by "Turn off"
    std::string mlVersion;          // e.g. 0.7.3
    std::string explorerFile;       // Mods\UnityExplorer*.dll that's installed (empty if none)
    bool explorerWrong = false;     // installed, but the build doesn't match this game
    std::string explorerWant;       // the release asset that fits this game
    std::string antiCheat;          // e.g. "Easy Anti-Cheat": mods there can get an account banned
};
UnityInfo UnityDetect(const std::wstring& exe);    // cached per exe for a couple of seconds
bool IsUnityGame(const std::wstring& exe);

enum UnityJobPhase { UJ_IDLE, UJ_BUSY };
struct UnityJob { int phase = UJ_IDLE; std::string status, error, done; };
UnityJob UnityGetJob();
void UnityInstallMelonLoader(const std::wstring& exe);   // latest release, x64 / x86 to match the game
void UnityInstallExplorer(const std::wstring& exe);      // the build that matches the game and MelonLoader
void UnitySetMelonLoaderOn(const std::wstring& exe, bool on);
void UnityRemoveMelonLoader(const std::wstring& exe);    // version.dll + MelonLoader\ (your Mods / UserData stay)
bool UnityGameRunning(const std::wstring& exe);
