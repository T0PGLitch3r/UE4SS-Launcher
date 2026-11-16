#pragma once
#include <string>
#include <vector>

// Classic Unreal games (Unreal Engine 1, 2 and 3). UE4SS can't load into these (they're built a completely
// different way), so the launcher works on them from the outside: the engine generation is read from the
// game's own package files, and the console, cheat keys, window and resolution are set in the game's .ini
// files (the first change to each file makes a backup, and "Put back original settings" restores them).
struct ClassicInfo {
    int gen = 0;                       // 1, 2 or 3; 0 = not a classic Unreal game
    int pkgVersion = 0, licensee = 0;  // from a package header (e.g. UT99 = 69, UT2004 = 128, UDK = 868)
    std::wstring exe, root;
    std::wstring sysDir;               // UE1/2: System\   UE3: <Game>Game\ (e.g. BmGame)
    std::wstring gameFolder;           // UE3 only: e.g. "BmGame"
    std::wstring mainIni;              // UE1/2: <Game>.ini   UE3: the user <Game>Engine.ini (may be empty)
    std::wstring userIni;              // UE1/2: User.ini     UE3: the user <Game>Input.ini (may be empty)
    std::wstring defaultInput;         // UE3: <Game>Game\Config\DefaultInput.ini (may be empty)
    std::wstring configDir;            // the folder the game's settings are in (for "Open settings folder")
    std::vector<std::wstring> inis;    // every .ini found (game folder + Documents\My Games)
    bool coalesced = false;            // UE3 game that packs its settings (Coalesced) and ignores loose .ini files
    bool consoleOn = false;
    bool cheatKeysOn = false;
    bool windowed = false;             // UE1/2: from the ini
    int resX = 0, resY = 0;            // UE1/2: from the ini
    bool hasBackups = false;
};
int ClassicGen(const std::wstring& exe);               // 0 if not UE1-3 (cached, quick)
ClassicInfo ClassicDetect(const std::wstring& exe);    // full details (cached for a couple of seconds)
void ClassicForget();                                  // drop the detail cache (after editing files)
const char* ClassicGenName(int gen);                   // "Unreal Engine 1" ...
const char* ClassicExamples(int gen);                  // a few known games, for the UI
std::wstring ClassicGameName(const std::wstring& exe); // the game's folder name, e.g. "Unreal Tournament 2004"

bool ClassicSetConsole(const std::wstring& exe, bool on, std::string& msg);
bool ClassicSetCheatKeys(const std::wstring& exe, bool on, std::string& msg);
bool ClassicSetDisplay(const std::wstring& exe, bool windowed, int w, int h, std::string& msg);   // UE1/2 (UE3 uses launch options)
bool ClassicRestore(const std::wstring& exe, std::string& msg);                                   // put back the original .ini files
struct ClassicKey { const char* key; const char* what; };
std::vector<ClassicKey> ClassicCheatKeyList(int gen);  // what the cheat keys do, for the UI

std::vector<std::wstring> ClassicMaps(const std::wstring& exe);   // map names you can start in (no extension)
std::wstring ClassicLaunchArgs(int gen, const std::wstring& map, bool windowed, bool noMovies, bool log, int w, int h, const std::wstring& extra);
bool ClassicLaunch(const std::wstring& exe, const std::wstring& args);

// Looks through the Steam libraries and the usual game folders for UE1-3 games.
struct ClassicFound { std::wstring name, exe; int gen; };
std::vector<ClassicFound> ClassicScanPC();
