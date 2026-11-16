#pragma once
#include <string>

// UE4SS auto-update from https://github.com/UE4SS-RE/RE-UE4SS/releases
// Channel 0 = experimental-latest (newest builds, what the launcher ships with), 1 = stable releases.
enum UpdPhase { UPD_IDLE, UPD_CHECKING, UPD_DOWNLOADING, UPD_INSTALLING };

struct UpdInfo {
    int phase = UPD_IDLE;
    bool checked = false;          // a check has finished this session
    bool available = false;        // a newer UE4SS than the installed one exists on the chosen channel
    std::string latest;            // e.g. v3.0.1-1203-g1a2b3c4d
    std::string error;             // last problem, shown to the user
    float progress = 0;            // download progress 0..1
    bool waitingForGame = false;   // an update is ready but a game is using UE4SS right now
};

const char* BundledUE4SSVersion();
std::string InstalledUE4SSVersion();          // from the marker file, or the bundled version
UpdInfo UpdGet();                              // thread-safe snapshot
void UpdCheck(int channel, bool installIfNewer);   // runs in the background
void UpdInstall();                             // install the version found by the last check (background)
bool UpdCanUndo();                             // a previous UE4SS is saved
void UpdUndo();                                // put the previous UE4SS back
void UpdTick();                                // call regularly: installs a waiting update once the game is closed
bool UpdKeepInstalledDll();                    // ExtractBundle: don't put the bundled UE4SS.dll back over an update
void UpdForgetInstalled();                     // after "Reset files": back to the bundled version
// shared helpers (HTTPS download, .zip extraction, folder delete)
bool NetGet(const std::wstring& url, std::string& out, std::string& err);
bool ZipExtract(const std::string& zipData, const std::wstring& outDir);
void DeleteFolderTree(const std::wstring& path);
