// ---- File browser inside the editor
// While the in-game editor is open the game has to keep focus: the input hooks feed the editor, and some games
// (Stray) crash when they lose focus with the editor's camera on. A Windows file dialog needs focus, so in the editor
// files are picked with this browser drawn in the editor window itself. Outside the editor (the launcher's own
// window) the normal Windows dialog is used.
#include <shlobj.h>

struct FbEntry { std::wstring name; bool dir = false; ULONGLONG size = 0; FILETIME when{}; };
static struct FileBrowser {
    bool open = false, save = false, wantOpen = false;
    std::string title;
    std::vector<std::wstring> exts;          // lower-case, with the dot; empty = every file
    std::wstring filterName, dir, saveExt;
    std::vector<FbEntry> list; std::wstring listedDir; std::string err;
    int sel = -1; char name[260] = ""; char pathBuf[1024] = ""; bool askReplace = false;
    std::function<void(std::wstring)> done;
    ImGuiContext* ctx = nullptr;             // drawn in the window that asked (the layer over the game, or the editor window)
} FB;
static std::wstring g_fbLastDir;

static std::wstring FbKnown(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}
static std::wstring FbLower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
// "Images (*.png, *.jpg)\0*.png;*.jpg\0All files\0*.*\0" -> the first filter's name and extensions
static void FbParseFilter(const wchar_t* f) {
    FB.exts.clear(); FB.filterName.clear();
    if (!f || !*f) return;
    FB.filterName = f;
    const wchar_t* pat = f + wcslen(f) + 1;
    std::wstring p = pat, cur;
    for (size_t i = 0; i <= p.size(); i++) {
        if (i == p.size() || p[i] == L';') {
            size_t dot = cur.find_last_of(L'.');
            if (cur.find(L"*.*") == std::wstring::npos && dot != std::wstring::npos) FB.exts.push_back(FbLower(cur.substr(dot)));
            cur.clear();
        } else if (p[i] != L' ') cur += p[i];
    }
}
static bool FbMatches(const std::wstring& n) {
    if (FB.exts.empty()) return true;
    std::wstring l = FbLower(n);
    for (auto& e : FB.exts) if (l.size() >= e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0) return true;
    return false;
}
static void FbList() {
    FB.list.clear(); FB.err.clear(); FB.sel = -1; FB.listedDir = FB.dir; FB.askReplace = false;
    if (FB.dir.empty()) return;              // "This PC": the drives
    WIN32_FIND_DATAW fd{};
    std::wstring d = FB.dir; if (d.back() != L'\\') d += L'\\';
    HANDLE h = FindFirstFileW((d + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { FB.err = "Can't open this folder."; return; }
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        FbEntry e; e.name = n; e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; e.when = fd.ftLastWriteTime;
        if (e.dir || FbMatches(n)) FB.list.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(FB.list.begin(), FB.list.end(), [](const FbEntry& a, const FbEntry& b) {
        if (a.dir != b.dir) return a.dir;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}
static void FbGo(const std::wstring& d) {
    std::wstring x = d;
    while (x.size() > 3 && (x.back() == L'\\' || x.back() == L'/')) x.pop_back();
    if (x.size() == 2 && x[1] == L':') x += L'\\';
    FB.dir = x;
    strncpy(FB.pathBuf, W2U(x).c_str(), sizeof FB.pathBuf - 1);
    FbList();
}
static void FbFinish(const std::wstring& path) {
    if (!path.empty()) { size_t sl = path.find_last_of(L"\\/"); if (sl != std::wstring::npos) g_fbLastDir = path.substr(0, sl); }
    FB.open = false;
    auto cb = FB.done; FB.done = nullptr;
    if (cb && !path.empty()) cb(path);
}
// Pick a file (or a place to save one); `done` gets the full path (never called if cancelled)
static void OvBrowse(const wchar_t* filter, const wchar_t* title, const std::wstring& initial, bool save, const std::wstring& saveName,
                     const wchar_t* saveExt, std::function<void(std::wstring)> done) {
    if (!g_ovVisible) {     // in the launcher's own window: the normal Windows dialog
        std::vector<wchar_t> flt;          // keep the double-0-terminated filter alive in the lambda
        if (filter) { const wchar_t* p = filter; while (*p) p += wcslen(p) + 1; flt.assign(filter, p + 1); }
        std::wstring t = title ? title : L"", ini = initial, sn = saveName, ext = saveExt ? saveExt : L"";
        Defer([flt, t, ini, sn, ext, save, done] {
            std::wstring f = save ? PickSaveFile(flt.empty() ? nullptr : flt.data(), t.c_str(), sn, ext.empty() ? nullptr : ext.c_str())
                                  : PickFile(flt.empty() ? nullptr : flt.data(), t.c_str(), ini);
            if (!f.empty() && done) done(f);
        });
        return;
    }
    FB.save = save; FB.title = W2U(title ? title : L""); FB.done = std::move(done);
    FbParseFilter(filter);
    FB.saveExt = saveExt ? saveExt : L"";
    strncpy(FB.name, W2U(saveName).c_str(), sizeof FB.name - 1);
    std::wstring start = initial;
    if (!start.empty() && (start.back() == L'\\' || start.back() == L'/')) start.pop_back();
    if (start.empty() || !DirExists(start)) start = g_fbLastDir;
    if (start.empty() || !DirExists(start)) start = FbKnown(FOLDERID_Downloads);
    FbGo(start);
    FB.open = true; FB.wantOpen = true; FB.ctx = ImGui::GetCurrentContext();
}
static void FbDrawPlaces() {
    struct Place { const char* icon; std::string name; std::wstring path; };
    std::vector<Place> places = {
        { ICON_FOLDER_OPEN, "Desktop", FbKnown(FOLDERID_Desktop) }, { ICON_DOWNLOAD, "Downloads", FbKnown(FOLDERID_Downloads) },
        { ICON_FOLDER_OPEN, "Documents", FbKnown(FOLDERID_Documents) }, { ICON_PALETTE, "Pictures", FbKnown(FOLDERID_Pictures) },
        { ICON_BOX_OPEN, "Models", ModelsDir() }, { ICON_PALETTE, "Exported textures", DirOf(ModelsRoot()) + L"\\exported textures" },
        { ICON_LAYER, "Materials", MaterialsDir() } };
    std::wstring fm = FModelExports(); if (DirExists(fm)) places.push_back({ ICON_FOLDER_OPEN, "FModel exports", fm });
    for (auto& p : places) if (!p.path.empty() && DirExists(p.path)) {
        if (ImGui::Selectable((std::string(p.icon) + "  " + p.name).c_str(), FbLower(FB.dir) == FbLower(p.path))) FbGo(p.path);
    }
    ImGui::Dummy(ImVec2(0, Px(6)));
    ImGui::PushFont(F_small); EdLabel("This PC"); ImGui::PopFont();
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) if (drives & (1u << i)) {
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT t = GetDriveTypeW(root); if (t == DRIVE_NO_ROOT_DIR) continue;
        char lbl[32]; snprintf(lbl, sizeof lbl, ICON_GAUGE "  %c:", 'A' + i);
        if (ImGui::Selectable(lbl, FbLower(FB.dir) == FbLower(root))) FbGo(root);
    }
}
static void FbPick(const FbEntry& e) {
    std::wstring d = FB.dir; if (d.back() != L'\\') d += L'\\';
    if (e.dir) { FbGo(d + e.name); return; }
    if (!FB.save) { FbFinish(d + e.name); return; }
    strncpy(FB.name, W2U(e.name).c_str(), sizeof FB.name - 1);
}
static void FbSave() {
    std::wstring n = U2W(FB.name);
    while (!n.empty() && n.back() == L' ') n.pop_back();
    if (n.empty()) return;
    if (!FB.saveExt.empty() && FbLower(n).find(L"." + FbLower(FB.saveExt)) == std::wstring::npos) n += L"." + FB.saveExt;
    std::wstring d = FB.dir; if (d.back() != L'\\') d += L'\\';
    std::wstring full = d + n;
    if (FileExists(full) && !FB.askReplace) { FB.askReplace = true; return; }
    FbFinish(full);
}
// drawn every frame from the editor window (opens itself when OvBrowse asked for it)
static void FileBrowserDraw() {
    if (!FB.open) return;
    if (FB.ctx && FB.ctx != ImGui::GetCurrentContext()) return;
    if (FB.wantOpen) { ImGui::OpenPopup("##fbrowse"); FB.wantOpen = false; }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - Px(40), Px(860)), std::min(vp->Size.y - Px(40), Px(600))), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##fbrowse", nullptr, ImGuiWindowFlags_NoTitleBar)) { if (!ImGui::IsPopupOpen("##fbrowse")) FB.wantOpen = true; return; }
    ImGui::PushFont(F_h2); ImGui::TextUnformatted(FB.title.empty() ? (FB.save ? "Save" : "Pick a file") : FB.title.c_str()); ImGui::PopFont();
    // path bar
    if (ImGui::Button("Up")) {
        std::wstring d = FB.dir;
        if (d.size() <= 3) FbGo(L""); else { size_t sl = d.find_last_of(L'\\'); FbGo(sl == std::wstring::npos ? L"" : d.substr(0, sl)); }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-Px(4));
    if (ImGui::InputText("##path", FB.pathBuf, sizeof FB.pathBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::wstring d = U2W(FB.pathBuf);
        if (DirExists(d)) FbGo(d);
        else if (!FB.save && FileExists(d) && FbMatches(d)) { FbFinish(d); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        else FB.err = "There's no folder called that.";
    }
    float footer = ImGui::GetFrameHeightWithSpacing() * (FB.save ? 2.2f : 1.2f) + Px(8);
    float bodyH = ImGui::GetContentRegionAvail().y - footer;
    // places
    ImGui::BeginChild("##fbplaces", ImVec2(Px(190), bodyH), ImGuiChildFlags_Borders);
    FbDrawPlaces();
    ImGui::EndChild();
    ImGui::SameLine();
    // the folder
    bool isImg = false; std::wstring selPath;
    ImGui::BeginChild("##fblist", ImVec2(-Px(210), bodyH), ImGuiChildFlags_Borders);
    if (FB.dir.empty()) {
        EdLabel("Pick a drive on the left.");
    } else if (!FB.err.empty() && FB.list.empty()) EdLabel(FB.err.c_str());
    else if (FB.list.empty()) EdLabel(FB.exts.empty() ? "This folder is empty." : ("No " + W2U(FB.filterName.substr(0, FB.filterName.find(L'('))) + "in this folder.").c_str());
    ImGuiListClipper clip; clip.Begin((int)FB.list.size());
    int pick = -1;
    while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
        const FbEntry& e = FB.list[i];
        ImGui::PushID(i);
        std::string label = std::string(e.dir ? ICON_FOLDER_OPEN "  " : "      ") + W2U(e.name);
        if (ImGui::Selectable(label.c_str(), FB.sel == i, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns)) {
            FB.sel = i; FB.askReplace = false;
            if (!e.dir && FB.save) strncpy(FB.name, W2U(e.name).c_str(), sizeof FB.name - 1);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) pick = i;
        }
        if (!e.dir) {
            char sz[32];
            if (e.size >= 1048576) snprintf(sz, sizeof sz, "%.1f MB", e.size / 1048576.0); else snprintf(sz, sizeof sz, "%.0f KB", std::max(1.0, e.size / 1024.0));
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(sz).x);
            ImGui::PushFont(F_small); EdLabel(sz); ImGui::PopFont();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // preview of the picked file (pictures)
    ImGui::BeginChild("##fbprev", ImVec2(0, bodyH), ImGuiChildFlags_Borders);
    if (FB.sel >= 0 && FB.sel < (int)FB.list.size() && !FB.list[FB.sel].dir) {
        std::wstring d = FB.dir; if (d.back() != L'\\') d += L'\\';
        selPath = d + FB.list[FB.sel].name;
        std::wstring l = FbLower(selPath);
        for (auto e : { L".png", L".jpg", L".jpeg", L".tga", L".bmp" }) if (l.size() > wcslen(e) && l.compare(l.size() - wcslen(e), wcslen(e), e) == 0) isImg = true;
        if (isImg) {
            if (ID3D11ShaderResourceView* srv = MatThumbFor(selPath)) {
                float w = ImGui::GetContentRegionAvail().x;
                ImGui::Image((ImTextureID)srv, ImVec2(w, w));
            }
        }
        ImGui::PushTextWrapPos(0); ImGui::PushFont(F_small); EdLabel(W2U(FB.list[FB.sel].name).c_str()); ImGui::PopFont(); ImGui::PopTextWrapPos();
    } else { ImGui::PushFont(F_small); ImGui::PushTextWrapPos(0); EdLabel("Click a file to see it here. Double-click a folder to go in, a file to pick it."); ImGui::PopTextWrapPos(); ImGui::PopFont(); }
    ImGui::EndChild();
    if (pick >= 0) {
        FbEntry e = FB.list[pick];
        FbPick(e);
        if (!FB.open) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    }
    // footer
    if (FB.save) {
        ImGui::SetNextItemWidth(-Px(220));
        bool enter = ImGui::InputTextWithHint("##fbname", "File name", FB.name, sizeof FB.name, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited()) FB.askReplace = false;
        ImGui::SameLine();
        if (AccentButton(FB.askReplace ? "Replace it" : "Save", ImVec2(Px(100), 0)) || enter) { FbSave(); if (!FB.open) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; } }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(-1, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { FbFinish(L""); ImGui::CloseCurrentPopup(); }
        if (FB.askReplace) { ImGui::PushFont(F_small); ImGui::TextColored(P.warn, "A file with that name is already there. Click Replace it to save over it."); ImGui::PopFont(); }
    } else {
        ImGui::PushFont(F_small);
        EdLabel(FB.filterName.empty() ? "Any file" : W2U(FB.filterName).c_str());
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - Px(200) - ImGui::GetStyle().ItemSpacing.x);
        ImGui::BeginDisabled(selPath.empty());
        if (AccentButton("Pick", ImVec2(Px(100), 0))) { std::wstring s = selPath; FbFinish(s); ImGui::CloseCurrentPopup(); ImGui::EndDisabled(); ImGui::EndPopup(); return; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(Px(100), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { FbFinish(L""); ImGui::CloseCurrentPopup(); }
    }
    ImGui::EndPopup();
}
