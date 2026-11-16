#pragma once
#include "core.h"

struct LevelEntry { std::string path, name, folder; bool sub = false; };
struct BridgeStatus { bool connected = false; std::string currentMap; bool freecam = false; std::string fcMode, cam, rot, fcKey; float fcSpeed = 0; };
enum LevelSource { LS_NONE, LS_CACHE, LS_FILES };

std::wstring BridgeDir();
void BridgeInit();                       // create the folder and make sure the mod is enabled
void BridgeReset();                      // clear stale files (before launching a game)
BridgeStatus GetBridgeStatus();          // connected = game heartbeat seen in the last few seconds
int BridgeSend(const std::string& verb, const std::string& arg = "");
bool BridgeResult(int id, bool& ok, std::string& msg);
void BridgeCacheLevels();                // copy the game's latest scan into the per-game cache
LevelSource LoadLevels(std::vector<LevelEntry>& out, std::string& method);
// Turns whatever the user typed (file path, /Game path, Windows path, bare name) into a package path.
std::string NormalizeLevelPath(std::string in);
bool LooksLikeSubLevel(const std::string& path);
struct FreecamConfig { bool enabled = true; std::string key = "INS"; std::string mode = "auto"; bool hideText = true; float speed = 1.0f; };
void WriteFreecamConfig(const FreecamConfig& c);   // read live by the in-game mod

// ---- queued command channel
void BridgeFlush();                      // call regularly; writes queued commands once the game has read the last batch

// ---- in-game editor data (written by the LauncherBridge mod while the overlay is open)
struct EdMsg { int id; bool err; std::string text; };
struct EdState {
    bool valid = false; std::string pawn; float loc[3]{}, rot[3]{};
    bool hasMove = false; float walk = 0, jump = 0, gravity = 1, timescale = 1, fov = 90; int mode = 1; bool god = false, ghost = false;
    std::string sel, selName, selClass; float sloc[3]{}, srot[3]{}, sscale[3]{ 1, 1, 1 }; bool shidden = false;
    bool hasScreen = false; float sscreen[2]{};   // selected actor's position on screen (0-1)
    bool hasCenter = false; float scenter[3]{};   // middle of the selected actor's bounds (world)
    bool hasGizmo = false; float gz[8]{}; float gzLen = 100;   // screen pos of centre + X/Y/Z axis tips (0-1), axis length in world units
    std::vector<std::pair<std::string, std::string>> added;   // models added this session: path, name (newest first)
    int layoutCount = 0; bool layoutOn = true; bool hasLayout = false;   // saved editor changes for this level
    int undoCount = 0; std::string undoWhat;
    std::string layoutGame, layoutMap;                                    // whose saved layout this is (game key, level path)                              // edits that Undo can take back
    std::string lookingAs;                        // set while your character looks like another one
    std::string playingAs;                        // set while you're playing as another character
    std::string swapped;                          // model swap: the character model you are now
    bool hasSelCam = false; float selcam[9]{};    // camera (x,y,z, pitch,yaw,roll, fov, w,h) the game used for sscreen
    std::vector<EdMsg> msgs;
};
struct EdModel { std::string path, name; };
bool ReadEdModels(std::vector<EdModel>& out, int& total);       // models loaded in the game right now (true when new data)
std::vector<std::string> PakModels(std::string& note);          // model-looking assets in the game's .pak files (package paths)
struct EdCam { int seq = 0; double t = 0; float loc[3]{}, rot[3]{}; float fov = 90; int w = 0, h = 0; };
bool ReadEdCam(EdCam& c);                                       // true when a newer camera sample arrived
struct EdProp { std::string cls, name, kind, value; };
struct EdActor { std::string path, name, cls; float dist; };
bool ReadEdState(EdState& st);                                  // true when new data arrived
bool ReadEdProps(std::vector<EdProp>& out, std::string& forPath);
extern std::string g_edSelMesh, g_edSelMeshKind, g_edSelActorXf;   // the selected object's first model, and its own transform
extern std::vector<std::string> g_edSelParts;                         // every model piece: "kind\tpath\tx,y,z\tp,y,r\tsx,sy,sz"   // the selected object's model asset ("static" / "skeletal", object path)
bool ReadEdActors(std::vector<EdActor>& out, int& total);
bool ReadEdNpcs(std::vector<EdActor>& out, int& total);         // characters you can play as (npcs.txt)
void EdClearFiles();
// developer menu & cheats (cheats.txt, written by the mod on ed_devscan)
struct EdCheat { std::string name, where, params; };
struct EdDevMenu { std::string path, name; bool open = false; };
struct EdCheats { bool scanned = false, cheatMgr = false, console = false, fog = true; std::string consoleKeys, view = "normal", cheatCurrent; std::vector<EdCheat> cheats; std::vector<EdDevMenu> menus, cheatClasses; };
bool ReadEdCheats(EdCheats& out);                               // true when new data arrived
std::vector<std::string> PakDevMenus(std::string& note, std::vector<std::string>* cheatMgrs = nullptr);   // + cheat manager blueprints
// model swap (swaplist.txt): character types you can become
struct EdSwapModel { std::string path, name, where; int count = 0; };
bool ReadEdSwap(std::vector<EdSwapModel>& out, int& total);       // true when new data arrived
std::vector<std::string> PakCharacters(std::string& note);         // character blueprints in the game's .pak files (package paths)        // debug / cheat / developer menu widgets in the game's .pak files
