// ---- Material assets (.uemat) and material instances (.uemi), in LauncherBridge\material library
// A material asset is a whole graph (its nodes, wires and properties) that isn't tied to one object: open it, edit
// it, preview it, then apply it to any object's material slot (it's baked into textures there, like any graph).
// A material instance points at a material asset and changes only its parameters (Scalar / Vector / Texture
// Parameter nodes), like Unreal's Material Instance Constant: no new shader, just other values.

static std::wstring MeLibDir() { return DirOf(ModelsRoot()) + L"\\material library"; }
static std::wstring g_mePendingAsset; static bool g_mePendingBake = false;   // an asset to load into an object's slot once its graph is read
struct MeLibItem { std::wstring file; std::string name; bool inst = false; std::string parent; };
static std::vector<MeLibItem> g_meLib; static bool g_meLibLoaded = false;
static void MeLibReload() {
    g_meLib.clear(); g_meLibLoaded = true;
    for (const wchar_t* ext : { L"*.uemat", L"*.uemi" }) {
        WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((MeLibDir() + L"\\" + ext).c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            MeLibItem it; it.file = MeLibDir() + L"\\" + fd.cFileName; std::wstring n = fd.cFileName; it.inst = n.size() > 5 && Lower(n.substr(n.size() - 5)) == L".uemi";
            it.name = W2U(n.substr(0, n.find_last_of(L'.')));
            if (it.inst) { std::string d; if (MeReadAll(it.file, d)) { size_t p = d.find("parent\t"); if (p != std::string::npos) it.parent = d.substr(p + 7, d.find('\n', p) - p - 7); } }
            g_meLib.push_back(it);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(g_meLib.begin(), g_meLib.end(), [](const MeLibItem& a, const MeLibItem& b) { return a.inst != b.inst ? !a.inst : W2U(Lower(U2W(a.name))) < W2U(Lower(U2W(b.name))); });
}
static bool MeWriteFile(const std::wstring& f, const std::string& o) {
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = WriteFile(h, o.data(), (DWORD)o.size(), &w, nullptr) && w == o.size(); CloseHandle(h); return ok;
}
// the graph as a standalone material (the object's own settings nodes come along as ordinary nodes)
static std::string MeAssetText() {
    std::string o = "UEMAT 1\n" + MePropsLine(M.props);
    for (auto& n : M.nodes) if (!n.output && n.kind != MEK_MAT) { MeNode k = n; o += MeNodeLine(k); }
    for (auto& kv : M.nin) o += "i\t" + std::to_string(kv.first / 64) + "\t" + std::to_string(kv.first % 64) + "\t" + std::to_string(kv.second.first) + "\t" + std::to_string(kv.second.second) + "\n";
    for (auto& l : M.links) if (MeIsStd(l.first)) o += "s\t" + std::to_string(l.first - M.stdStart) + "\t" + std::to_string(l.second) + "\t" + std::to_string(M.linkOut.count(l.first) ? M.linkOut[l.first] : 0) + "\n";
    return o;
}
static bool MeParseAsset(const std::string& d, MeCustom& c) {
    if (d.rfind("UEMAT", 0) != 0) return false;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto t = MeTabs(line); MeNode n;
        if (MeParseNode(t, n)) c.nodes.push_back(n);
        else if (MeParseProps(t, c.props)) c.hasProps = true;
        else if (t.size() >= 5 && t[0] == "i") c.nin[MeInKey(atoi(t[1].c_str()), atoi(t[2].c_str()))] = { atoi(t[3].c_str()), atoi(t[4].c_str()) };
        else if (t.size() >= 4 && t[0] == "s") c.std[atoi(t[1].c_str())] = { atoi(t[2].c_str()), atoi(t[3].c_str()) };
    }
    return true;
}
static bool MeSaveAssetAs(const std::wstring& f) {
    MakeDirs(MeLibDir());
    if (!MeWriteFile(f, MeAssetText())) { g_toasts.push_back({ "Couldn't save " + W2U(f), true, GetTickCount64() }); return false; }
    g_meLibLoaded = false;
    return true;
}
// add an asset's nodes to the graph now open (to the left of everything), wired into the Unreal inputs as saved
static void MeMergeAsset(const MeCustom& c, bool replaceStd) {
    float minX = 0; for (auto& n : M.nodes) minX = std::min(minX, n.x);
    float ax = 1e9f, ay = 1e9f, bx = -1e9f; for (auto& n : c.nodes) { ax = std::min(ax, n.x); ay = std::min(ay, n.y); bx = std::max(bx, n.x + MeNodeW(n)); }
    float dx = M.slots.empty() ? 0 : (minX - 80) - bx, dy = M.slots.empty() ? 0 : -ay;
    std::map<int, int> idMap;
    for (auto& n : c.nodes) { MeNode k = n; k.id = M.nextId++; k.x += dx; k.y += dy; idMap[n.id] = k.id; if (k.kind == MEK_COMMENT) M.nodes.insert(M.nodes.begin(), k); else M.nodes.push_back(k); }
    for (auto& kv : c.nin) { int a = (int)(kv.first / 64), in = (int)(kv.first % 64); if (idMap.count(a) && idMap.count(kv.second.first)) M.nin[MeInKey(idMap[a], in)] = { idMap[kv.second.first], kv.second.second }; }
    for (auto& kv : c.std) if (idMap.count(kv.second.first) && kv.first < STD_N) {
        int pin = M.stdStart + kv.first;
        if (!replaceStd && M.links.count(pin)) continue;
        M.links[pin] = idMap[kv.second.first]; M.linkOut[pin] = kv.second.second;
    }
    if (c.hasProps) M.props = c.props;
    MeStdChanged(); M.fitted = false;
}
// ---- opening an asset on its own: the output node has only Unreal's inputs
static void MatEdClose();
static void MeOpenAsset(const std::wstring& file) {
    std::string d; MeCustom c;
    if (!MeReadAll(file, d) || !MeParseAsset(d, c)) { g_toasts.push_back({ "Couldn't open " + W2U(file), true, GetTickCount64() }); return; }
    if (M.open) MatEdClose();
    M.open = true; M.actor.clear(); M.asset = file; M.dirty = false; M.slots.clear(); M.cur = 0; M.loaded = true; M.err.clear(); M.name.clear(); M.model.clear();
    M.nodes.clear(); M.links.clear(); M.linkOut.clear(); M.nin.clear(); M.pins.clear(); M.pend.clear(); M.selSet.clear(); M.sel = -1; M.builtKey.clear();
    M.stdStart = 0;
    for (int k = 0; k < STD_N; k++) { MeParam sp; sp.kind = MEK_STD; sp.name = g_meStd[k]; M.pins.push_back(sp); }
    MeNode out; out.id = M.nextId++; out.output = true; M.nodes.push_back(out);
    M.props = MeProps();
    MeMergeAsset(c, true);
    M.builtFor = 0; M.fitted = false; M.tab = g_meInst.empty() ? 0 : 2;
    MeHistoryReset();
    g_meOverride.clear();
    // (the editor window: big, as for an object's material)
    if (g_edWnd && !M.resized) {
        GetWindowRect(g_edWnd, &M.oldRect);
        HMONITOR mon = MonitorFromWindow(g_edWnd, MONITOR_DEFAULTTONEAREST); MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(mon, &mi);
        RECT wa = mi.rcWork;
        int w = std::min<int>(wa.right - wa.left - 40, (int)Px(1500)), h = std::min<int>(wa.bottom - wa.top - 40, (int)Px(900));
        SetWindowPos(g_edWnd, nullptr, wa.left + 20, wa.top + 20, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        M.resized = true;
    }
}
static void MeSaveAsset() {
    if (M.asset.empty()) return;
    if (MeSaveAssetAs(M.asset)) { M.dirty = false; g_toasts.push_back({ "Saved " + W2U(M.asset.substr(M.asset.find_last_of(L"\\/") + 1)), false, GetTickCount64() }); }
}
static std::wstring MeFreeLibName(const std::string& base, const wchar_t* ext) {
    MakeDirs(MeLibDir());
    std::wstring b = MeSafeName(base), p = MeLibDir() + L"\\" + b + ext;
    for (int i = 2; FileExists(p); i++) p = MeLibDir() + L"\\" + b + L" " + std::to_wstring(i) + ext;
    return p;
}
static void MeNewAsset() {
    std::wstring f = MeFreeLibName("M_NewMaterial", L".uemat");
    std::string o = "UEMAT 1\n" + MePropsLine(MeProps());
    // a starting point like Unreal's: a Vector Parameter into Base Color, a Scalar into Roughness
    MeNode a; a.id = 1; a.kind = MEK_VEC; a.name = "BaseColor"; a.value = "0.8000,0.8000,0.8000,1.0000"; a.x = -320; a.y = 20;
    MeNode b; b.id = 2; b.kind = MEK_SCA; b.name = "Roughness"; b.value = "0.5000"; b.x = -320; b.y = 160;
    o += MeNodeLine(a) + MeNodeLine(b) + "s\t" + std::to_string(STD_BASE) + "\t1\t0\ns\t" + std::to_string(STD_ROUGH) + "\t2\t0\n";
    if (!MeWriteFile(f, o)) { g_toasts.push_back({ "Couldn't create a material in " + W2U(MeLibDir()), true, GetTickCount64() }); return; }
    g_meLibLoaded = false; g_meInst.clear();
    MeOpenAsset(f);
}
// ---- instances
static void MeSaveInstance() {
    if (g_meInst.empty() || M.asset.empty()) return;
    std::string o = "UEMI 1\nparent\t" + W2U(M.asset.substr(M.asset.find_last_of(L"\\/") + 1)) + "\n";
    for (auto& kv : g_meOverride) o += "ov\t" + MeEsc(kv.first) + "\t" + kv.second + "\n";
    if (MeWriteFile(g_meInst, o)) g_toasts.push_back({ "Saved " + W2U(g_meInst.substr(g_meInst.find_last_of(L"\\/") + 1)), false, GetTickCount64() });
    g_meLibLoaded = false;
}
static void MeOpenInstance(const std::wstring& file) {
    std::string d; if (!MeReadAll(file, d) || d.rfind("UEMI", 0) != 0) { g_toasts.push_back({ "Couldn't open " + W2U(file), true, GetTickCount64() }); return; }
    std::string parent; std::map<std::string, std::string> ov;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto t = MeTabs(line);
        if (t.size() >= 2 && t[0] == "parent") parent = t[1];
        else if (t.size() >= 3 && t[0] == "ov") ov[MeUnesc(t[1])] = t[2];
    }
    std::wstring pf = MeLibDir() + L"\\" + U2W(parent);
    if (!FileExists(pf)) { g_toasts.push_back({ "Its parent material " + parent + " isn't in the library", true, GetTickCount64() }); return; }
    g_meInst = file;
    MeOpenAsset(pf);
    g_meOverride = ov; M.tab = 2;
}
static void MeNewInstance() {
    if (M.asset.empty()) return;
    std::string base = W2U(M.asset.substr(M.asset.find_last_of(L"\\/") + 1)); base = base.substr(0, base.find_last_of('.'));
    if (base.rfind("M_", 0) == 0) base = base.substr(2);
    if (M.dirty) MeSaveAsset();
    std::wstring parent = M.asset;
    g_meInst = MeFreeLibName("MI_" + base, L".uemi");
    g_meOverride.clear(); MeSaveInstance();
    M.tab = 2;
}
// the parameters of the material, for the instance editor (unique by name, like Unreal)
static void MeInstancePanel() {
    if (g_meInst.empty()) {
        EdLabel("A material instance changes this material's parameters without changing its graph.");
        ImGui::BeginDisabled(M.asset.empty());
        if (AccentButton(ICON_PLUS "  Create a Material Instance of this material")) MeNewInstance();
        ImGui::EndDisabled();
        if (M.asset.empty()) EdLabel("(save this graph as a material asset first: File > Save as material asset)");
        return;
    }
    ImGui::PushFont(F_h2); ImGui::TextUnformatted(W2U(g_meInst.substr(g_meInst.find_last_of(L"\\/") + 1)).c_str()); ImGui::PopFont();
    EdLabel(("Parent: " + W2U(M.asset.substr(M.asset.find_last_of(L"\\/") + 1))).c_str());
    std::map<std::string, MeNode*> params;
    for (auto& n : M.nodes) if (MeIsParam(n) && !params.count(n.name)) params[n.name] = &n;
    if (params.empty()) { ImGui::PushTextWrapPos(0); EdLabel("The parent has no parameters. Turn a Constant, Constant Vector or Texture Sample into a parameter (its Details) to change it here."); ImGui::PopTextWrapPos(); }
    g_meLabelW = std::min(Px(170), ImGui::GetContentRegionAvail().x * 0.4f);
    bool ch = false;
    for (auto& kv : params) {
        MeNode& n = *kv.second;
        ImGui::PushID(kv.first.c_str());
        bool on = g_meOverride.count(kv.first) > 0;
        if (ImGui::Checkbox("##ov", &on)) { if (on) g_meOverride[kv.first] = n.value; else g_meOverride.erase(kv.first); ch = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Override the parent's value");
        ImGui::SameLine();
        ImGui::BeginDisabled(!on);
        std::string v = on ? g_meOverride[kv.first] : n.value;
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(kv.first.c_str()); ImGui::SameLine(g_meLabelW + Px(30)); ImGui::SetNextItemWidth(-1);
        if (n.kind == MEK_SCA) { float f = (float)atof(v.c_str()); if (ImGui::DragFloat("##v", &f, 0.005f, 0, 0, "%.3f")) { char t[32]; snprintf(t, sizeof t, "%.4f", f); g_meOverride[kv.first] = t; ch = true; } }
        else if (n.kind == MEK_VEC) { float c[4]; MeColour(v, c); if (ImGui::ColorEdit4("##v", c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) { char t[96]; snprintf(t, sizeof t, "%.4f,%.4f,%.4f,%.4f", c[0], c[1], c[2], c[3]); g_meOverride[kv.first] = t; ch = true; } }
        else if (n.kind == MEK_TEX) {
            ImVec2 p = ImGui::GetCursorScreenPos(); float s = Px(48);
            MeTexBox(ImGui::GetWindowDrawList(), v, p, ImVec2(p.x + s, p.y + s), Px(3)); ImGui::Dummy(ImVec2(s, s)); ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(v.rfind("file:", 0) == 0 ? MeFileName(v).c_str() : MeShort(v).c_str());
            if (ImGui::SmallButton("Texture library...")) { g_txPickNode = -2; g_txPickOpen = true; g_meInstTexParam = kv.first; if (!g_txLoaded) TxReload(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Picture file...")) { std::string pn = kv.first; OvBrowse(L"Pictures (*.png, *.jpg, *.tga, *.bmp)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0", L"A texture for the parameter", ImpStartDir(), false, L"", nullptr, [pn](std::wstring f) { if (!f.empty()) g_meOverride[pn] = "file:" + W2U(f); }); }
            ImGui::EndGroup();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (ch) MeStdChanged();
    ImGui::Dummy(ImVec2(0, Px(8)));
    if (AccentButton(ICON_FLOPPY "  Save instance")) MeSaveInstance();
    ImGui::SameLine();
    if (ImGui::Button("Close instance (edit the parent)")) { g_meInst.clear(); g_meOverride.clear(); M.tab = 0; }
}

// ---- export
static void MeExportShader() {
    MsCompiled c = MsGenerate();
    std::string base = !M.asset.empty() ? W2U(M.asset.substr(M.asset.find_last_of(L"\\/") + 1)) : (M.cur < (int)M.slots.size() ? MeShort(M.slots[M.cur].mat) : "material");
    base = base.substr(0, base.find_last_of('.') == std::string::npos ? base.size() : base.find_last_of('.'));
    std::string text = c.hlsl;
    OvBrowse(L"HLSL shader (*.hlsl)\0*.hlsl\0", L"Export the shader code", L"", true, MeSafeName(base), L"hlsl", [text](std::wstring f) {
        if (f.empty()) return;
        bool ok = MeWriteFile(f, text);
        g_toasts.push_back({ ok ? "Exported the shader to " + W2U(f) : "Couldn't write " + W2U(f), !ok, GetTickCount64() });
    });
}
// bake every Unreal input into textures in a folder you pick (the same textures the game gets)
static void MeExportBaked() {
    MgProg P; std::string made;
    if (!MgCompile(P, made)) { g_toasts.push_back({ "Nothing is wired into Base Color, Normal, Roughness... to bake", true, GetTickCount64() }); return; }
    std::string base = !M.asset.empty() ? W2U(M.asset.substr(M.asset.find_last_of(L"\\/") + 1)) : (M.cur < (int)M.slots.size() ? MeShort(M.slots[M.cur].mat) : "material");
    base = base.substr(0, base.find_last_of('.') == std::string::npos ? base.size() : base.find_last_of('.'));
    int res = M.bakeRes;
    OvBrowse(L"PNG (*.png)\0*.png\0", L"Export baked textures (one PNG per input: _basecolor, _normal, _orm...)", L"", true, MeSafeName(base), L"png", [P, made, res](std::wstring f) {
        if (f.empty()) return;
        std::wstring prefix = f; if (Lower(prefix).size() > 4 && Lower(prefix).substr(prefix.size() - 4) == L".png") prefix = prefix.substr(0, prefix.size() - 4);
        std::thread([P, made, res, prefix]() { MgBake(P, made, res, prefix, ""); }).detach();
    });
}
// ---- applying: a material asset (or instance) onto an object's material slot
static void MeLoadAssetIntoSlot(const std::wstring& file) {
    std::string d; MeCustom c;
    std::map<std::string, std::string> ov; std::wstring f = file;
    if (Lower(file).size() > 5 && Lower(file).substr(file.size() - 5) == L".uemi") {     // an instance: its parent, with its values written in
        if (!MeReadAll(file, d)) return;
        std::istringstream ss(d); std::string line, parent;
        while (std::getline(ss, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); auto t = MeTabs(line); if (t.size() >= 2 && t[0] == "parent") parent = t[1]; else if (t.size() >= 3 && t[0] == "ov") ov[MeUnesc(t[1])] = t[2]; }
        f = MeLibDir() + L"\\" + U2W(parent);
    }
    if (!MeReadAll(f, d) || !MeParseAsset(d, c)) { g_toasts.push_back({ "Couldn't read " + W2U(f), true, GetTickCount64() }); return; }
    for (auto& n : c.nodes) if (MeIsParam(n) && ov.count(n.name)) n.value = ov[n.name];
    MeMergeAsset(c, true);
    MeAutoArrange();
}
static void MeApplyAssetToSelected() {
    if (M.asset.empty()) return;
    if (g_ed.sel.empty()) { g_toasts.push_back({ "Select an object in the game (or the Outliner) first", true, GetTickCount64() }); return; }
    if (M.dirty) MeSaveAsset();
    std::wstring what = g_meInst.empty() ? M.asset : g_meInst;
    if (!g_meInst.empty()) MeSaveInstance();
    std::string actor = g_ed.sel;
    g_meInst.clear(); g_meOverride.clear();
    M.asset.clear(); M.resized = false; M.open = false;
    MatEdOpen(actor);
    g_mePendingAsset = what; g_mePendingBake = true;
}
// once the object's graph is built: put the waiting asset in and bake it
static void MePendingStep() {
    if (g_mePendingAsset.empty() || !M.loaded || M.slots.empty() || M.builtFor != M.cur) return;
    MeLoadAssetIntoSlot(g_mePendingAsset);
    g_mePendingAsset.clear();
    if (g_mePendingBake) { g_mePendingBake = false; MeStartBake(); }
}
// the File menu's "Load material from the library" list
static void MeLibraryMenu(bool forSlot) {
    if (!g_meLibLoaded) MeLibReload();
    if (g_meLib.empty()) { EdLabel("(the library is empty)"); return; }
    for (auto& it : g_meLib) {
        std::string l = std::string(it.inst ? ICON_SLIDERS "  " : ICON_CUBE "  ") + it.name + (it.inst ? "   (instance of " + it.parent + ")" : "");
        if (ImGui::MenuItem(l.c_str())) {
            if (forSlot) MeLoadAssetIntoSlot(it.file);
            else if (it.inst) MeOpenInstance(it.file); else { g_meInst.clear(); MeOpenAsset(it.file); }
        }
    }
}
// the texture editor needs the material editor open: an empty material to start with if nothing is open
static void MeNewAssetOrBlank() {
    if (!g_meLibLoaded) MeLibReload();
    for (auto& it : g_meLib) if (!it.inst) { MeOpenAsset(it.file); return; }
    MeNewAsset();
}
// ---- Content Browser: the material library
static void UeMaterialLibraryUI() {
    if (!g_meLibLoaded) MeLibReload();
    if (AccentButton(ICON_PLUS "  New Material")) MeNewAsset();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FOLDER_OPEN "  Library folder")) { MakeDirs(MeLibDir()); OpenFolder(MeLibDir()); }
    ImGui::SameLine();
    if (ImGui::Button(ICON_ROTATE "##mlr")) MeLibReload();
    ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim); ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_meLib.empty() ? "No material assets yet. Make one with New Material, or save any object's material graph from the material editor (File > Save as material asset)."
                                           : "Double-click to open in the material editor. Select an object in the game, then use Apply to object in the editor.");
    ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::PopFont();
    for (auto& it : g_meLib) {
        ImGui::PushID(W2U(it.file).c_str());
        std::string l = std::string(it.inst ? ICON_SLIDERS "  " : ICON_CUBE "  ") + it.name;
        if (ImGui::Selectable(l.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (it.inst) MeOpenInstance(it.file); else { g_meInst.clear(); MeOpenAsset(it.file); }
        }
        if (it.inst) { ImGui::SameLine(); ImGui::TextDisabled("instance of %s", it.parent.c_str()); }
        if (ImGui::BeginPopupContextItem("##mlm")) {
            if (ImGui::MenuItem("Open")) { if (it.inst) MeOpenInstance(it.file); else { g_meInst.clear(); MeOpenAsset(it.file); } }
            if (ImGui::MenuItem("Apply to the selected object")) { if (it.inst) MeOpenInstance(it.file); else { g_meInst.clear(); MeOpenAsset(it.file); } MeApplyAssetToSelected(); }
            if (ImGui::MenuItem("Show in folder")) OpenFolder(MeLibDir());
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

// ---- Content Browser: the texture library (opens the Texture Editor)
static void UeTextureLibraryUI() {
    TxPump();
    if (AccentButton(ICON_FILE_IMPORT "  Import texture")) TxImportPick();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FOLDER_OPEN "  Library folder")) { MakeDirs(TxDir()); OpenFolder(TxDir()); }
    ImGui::PushFont(F_small); ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
    ImGui::TextUnformatted(g_tx.empty() ? "No textures yet: import PNG, JPG, TGA, BMP or HDR pictures." : "Double-click a texture to edit it in the Texture Editor (material editor, Textures tab).");
    ImGui::PopStyleColor(); ImGui::PopFont();
    float cell = Px(96), avail = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)(avail / (cell + ImGui::GetStyle().ItemSpacing.x)));
    for (int i = 0; i < (int)g_tx.size(); i++) {
        if (i % cols) ImGui::SameLine();
        ImGui::PushID(i);
        ImGui::BeginGroup();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##t", ImVec2(cell, cell));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            g_txSel = i; TxDisplayRebuild();
            if (!M.open) MeNewAssetOrBlank();
            M.tab = 1;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", g_tx[i].name.c_str(), g_txTypeName[g_tx[i].s.type]);
        if (ID3D11ShaderResourceView* s = MatThumbFor(g_tx[i].out)) ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)s, p, ImVec2(p.x + cell, p.y + cell), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, Px(4));
        else ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), IM_COL32(40, 40, 46, 255), Px(4));
        ImGui::PushFont(F_small);
        std::string l = g_tx[i].name; while (l.size() > 1 && ImGui::CalcTextSize(l.c_str()).x > cell) l.pop_back();
        ImGui::TextUnformatted(l.c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::PopID();
    }
}
