#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

// Model import / export between Unreal games.
//  Export: FModel (downloaded on request) reads a game's packed files; you save models as .psk / .gltf and
//          animations as .psa from it.
//  Import: a .psk (+ .psa animations), .gltf / .glb or .obj is turned into a .uemodel file that the in-game mod
//          builds out of procedural mesh parts (one per bone, so animations play by moving the parts), coloured
//          from the model's texture.
struct ModelInfo {
    std::wstring file;            // the .uemodel
    std::string name, source;
    int parts = 0, verts = 0, tris = 0;
    std::vector<std::string> anims;
    bool textured = false;
    std::vector<std::string> slots;     // material slot names
    std::vector<std::string> slotUse;   // library material used in each slot ("" = the model's own)
};
std::wstring ModelsDir();                                   // Mods\LauncherBridge\models\games\<game> (the game models are imported for)
std::wstring ModelsRoot();                                  // Mods\LauncherBridge\models
void ModelSetGame(const std::wstring& game);                // which game imports are for (its name, as the launcher shows it)
std::vector<ModelInfo> ModelList();
bool ModelImport(const std::wstring& src, std::wstring& outFile, std::string& msg);     // any supported file
bool ModelAddAnims(const std::wstring& model, const std::vector<std::wstring>& psa, std::string& msg);
bool ModelDelete(const std::wstring& model);
// Export a static mesh (object path like /Game/Props/SM_Box.SM_Box) straight from the game's .pak files into a .uemodel
bool ModelExportFromGame(const std::wstring& paksDir, const std::string& objectPath, std::wstring& outFile, std::string& msg);
// Export every mesh an object is made of. parts: "static|skeletal\tobjectPath\tx,y,z\tpitch,yaw,roll\tsx,sy,sz" (world);
// actorXf: the object's world transform in the same form (the model is saved relative to it)
// Every model in the game's files (each package's header is checked for a StaticMesh / SkeletalMesh). Remembered per game,
// so the second time is instant. done/total report progress; found is called for each model as it turns up.
struct GameMesh { std::string path; bool skeletal = false, exportable = true; };   // exportable: false for Unreal 5 packages
bool ModelListGameFiles(const std::wstring& paksDir, const std::function<bool(const std::string& fileName)>& want, std::vector<std::string>& out, std::string& note);
bool ModelListGameMeshes(const std::wstring& paksDir, std::vector<GameMesh>& out, std::string& note,
                         std::atomic<int>* done, std::atomic<int>* total, const std::function<void(const GameMesh&)>& found);
bool ModelOutlineMesh(const std::wstring& paksDir, const std::vector<std::string>& parts, std::vector<float>& xyz, std::vector<uint32_t>& idx, std::string& err);
// save a game object as .glb (fmt 1) or .fbx (fmt 2) for other tools
bool ModelExportPartsAs(const std::wstring& paksDir, const std::string& name, const std::vector<std::string>& parts, const std::string& actorXf, const std::wstring& outFile, int fmt, std::string& msg);
bool ModelExportParts(const std::wstring& paksDir, const std::string& name, const std::vector<std::string>& parts, const std::string& actorXf, std::wstring& outFile, std::string& msg);
// a small RGBA picture of a game texture (object path), read from the game's files
bool GameTexturePreview(const std::wstring& paksDir, const std::string& objectPath, int maxDim, std::vector<unsigned char>& rgba, int& w, int& h, std::string& err);
// one of the game's textures (object path) at full size, written as a PNG
bool GameTextureExport(const std::wstring& paksDir, const std::string& objectPath, const std::wstring& outPng, int& w, int& h, std::string& err);
bool PictureRGBA(const std::wstring& paksDir, const std::string& value, int maxDim, std::vector<unsigned char>& px, int& w, int& h, std::string& err);
bool WritePngRGBA(const std::wstring& path, const std::vector<unsigned char>& px, int w, int h);
bool ImageLoadAny(const std::wstring& path, std::vector<float>& rgba, int& w, int& h, bool& hdr, std::string& err);   // PNG/JPG/TGA/BMP (0..1) or .hdr (light values)
bool ImageWrite(const std::wstring& path, const std::vector<float>& rgba, int w, int h, int fmt, int channels, std::string& err); // 0 png 1 tga 2 jpg 3 hdr 4 bmp
bool ModelSetAesKey(const std::string& hexKey);
// imported models with more triangles than this are simplified down to it (50,000 - 3,000,000)
void ModelSetMaxTriangles(int n); int ModelMaxTriangles();
std::string ModelLastSimplified();   // "simplified from X to Y triangles" for the last model written, or ""
// Finds a game's AES key in the running game (pid) by trying everything that could be a key on its encrypted files.
// true + keyHex ("0x..." or empty when the files aren't encrypted) on success; msg says what happened.
bool ModelFindAesKey(unsigned long pid, const std::wstring& paksDir, std::string& keyHex, std::string& msg);   // 64 hex digits (0x... ok); empty = none. false if it isn't a valid key
bool ModelSetTexture(const std::wstring& model, const std::wstring& image, std::string& msg);   // re-colour from an image

// Materials library: every exported model's textures are kept as named materials (colour + normal map) in
// models\materials, and any of them can be put on any slot of any imported model.
struct MaterialInfo { std::string name, from, look; std::wstring dir, colour, normal, masks; };   // look: "blend\ttwoSided\tclip\tavgAlpha"   // masks: orm.png (ambient occlusion, roughness, metal)
std::wstring MaterialsDir();
std::vector<MaterialInfo> MaterialList();
bool MaterialAdd(const std::wstring& colourImage, std::string& msg);                         // a new material from a picture
bool MaterialSetNormal(const std::wstring& dir, const std::wstring& image, std::string& msg);
bool MaterialDelete(const std::wstring& dir);
bool MaterialPreview(const std::wstring& png, std::vector<unsigned char>& rgba, int& w, int& h);   // small picture for the UI
bool ModelSetSlotMaterial(const std::wstring& model, int slot, const std::wstring& matDir, std::string& msg);   // empty dir = own material

// FModel (the exporter)
std::wstring FModelExe();                                   // empty if not downloaded
std::wstring FModelExports();                               // FModel's Output\Exports folder
bool FModelDownload(std::string& err);
