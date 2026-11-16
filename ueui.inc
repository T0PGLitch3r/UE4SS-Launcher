// ---- Unreal-style editor layout
// The editor looks and works like Unreal Engine's level editor: a menu bar and toolbar along the top of the game,
// Place Actors on the left, the Outliner and Details on the right, the Content Browser along the bottom, and the
// game itself in the middle as the viewport (with its own little toolbar: transform tools, snapping, camera speed).
// The panels are drawn on the see-through layer over the game. Any panel can be undocked into the separate
// editor window (which can go on another screen) and docked back again.

#define UI_UNDOCK "\xef\x8b\x92"
#define UI_DOCK "\xef\x8b\x90"
#define UI_XMARK "\xef\x80\x8d"
#define UI_FOLDER "\xef\x81\xbb"
#define UI_BULB "\xef\x83\xab"
#define UI_SAVE "\xef\x83\x87"
#define UI_PAUSE "\xef\x81\x8c"
#define UI_DB "\xef\x87\x80"
#define UI_SQUARE "\xef\x83\x88"
#define UI_CARETUP "\xef\x83\x98"
#define UI_FONT "\xef\x80\xb1"
#define UI_MAGNET "\xef\x81\xb6"
#define UI_GLOBE "\xef\x82\xac"
#define UI_SLIDERS "\xef\x87\x9e"
#define UI_SITEMAP "\xef\x83\xa8"
#define UI_FORWARD "\xef\x81\x8e"
#define UI_SHAPES "\xef\x98\x9f"

static const char* g_upNames[UP_COUNT] = { "Outliner", "Details", "Place Actors", "Content Browser" };
static const char* g_upIcons[UP_COUNT] = { UI_SITEMAP, UI_SLIDERS, ICON_PLUS, UI_FOLDER };
static RECT g_ueLayerScreen{ 0, 0, 0, 0 };              // the layer over the game, in screen coords
static std::string g_dragName;
static ImGuiContext* g_dragCtx = nullptr;
static std::string g_olMap;
static float g_ueTime = 1.f;                            // game speed set from the toolbar
struct UeRecent { std::string icon, name, path; };
static std::vector<UeRecent> g_ueRecent;

static bool UeAnyFloat() { for (int i = 0; i < UP_COUNT; i++) if (g_upOpen[i] && g_upFloat[i]) return true; return false; }
static bool UeWantEdWindow() { return g_ovVisible && (M.open || UeAnyFloat()); }
static std::string UeLower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// ---- Unreal 5's dark theme
static int g_ueCols = 0, g_ueVars = 0;
static void UeThemePush() {
    auto C = [](ImGuiCol c, unsigned hex, float a = 1.f) { ImGui::PushStyleColor(c, Hex(hex, a)); g_ueCols++; };
    C(ImGuiCol_Text, 0xC0C0C0); C(ImGuiCol_TextDisabled, 0x6E6E6E);
    C(ImGuiCol_WindowBg, 0x1A1A1A); C(ImGuiCol_ChildBg, 0x1A1A1A, 0.f); C(ImGuiCol_PopupBg, 0x1F1F1F, 0.98f);
    C(ImGuiCol_Border, 0x050505); C(ImGuiCol_BorderShadow, 0, 0.f);
    C(ImGuiCol_FrameBg, 0x0F0F0F); C(ImGuiCol_FrameBgHovered, 0x161616); C(ImGuiCol_FrameBgActive, 0x161616);
    C(ImGuiCol_MenuBarBg, 0x151515); C(ImGuiCol_TitleBg, 0x151515); C(ImGuiCol_TitleBgActive, 0x151515);
    C(ImGuiCol_ScrollbarBg, 0x1A1A1A); C(ImGuiCol_ScrollbarGrab, 0x3A3A3A); C(ImGuiCol_ScrollbarGrabHovered, 0x575757); C(ImGuiCol_ScrollbarGrabActive, 0x6E6E6E);
    C(ImGuiCol_CheckMark, 0x0070E0); C(ImGuiCol_SliderGrab, 0x0070E0); C(ImGuiCol_SliderGrabActive, 0x2F8CF0);
    C(ImGuiCol_Button, 0x383838); C(ImGuiCol_ButtonHovered, 0x575757); C(ImGuiCol_ButtonActive, 0x262626);
    C(ImGuiCol_Header, 0x0070E0, 0.55f); C(ImGuiCol_HeaderHovered, 0x575757, 0.55f); C(ImGuiCol_HeaderActive, 0x0070E0, 0.8f);
    C(ImGuiCol_Separator, 0x050505); C(ImGuiCol_SeparatorHovered, 0x0070E0); C(ImGuiCol_SeparatorActive, 0x0070E0);
    C(ImGuiCol_ResizeGrip, 0, 0.f); C(ImGuiCol_ResizeGripHovered, 0x0070E0, 0.6f); C(ImGuiCol_ResizeGripActive, 0x0070E0);
    C(ImGuiCol_Tab, 0x151515); C(ImGuiCol_TabHovered, 0x2F2F2F); C(ImGuiCol_TabSelected, 0x242424); C(ImGuiCol_TabSelectedOverline, 0x0070E0);
    C(ImGuiCol_TabDimmed, 0x151515); C(ImGuiCol_TabDimmedSelected, 0x242424);
    C(ImGuiCol_TableHeaderBg, 0x242424); C(ImGuiCol_TableBorderStrong, 0x050505); C(ImGuiCol_TableBorderLight, 0x0F0F0F);
    C(ImGuiCol_TableRowBg, 0x1A1A1A, 0.f); C(ImGuiCol_TableRowBgAlt, 0xFFFFFF, 0.025f);
    C(ImGuiCol_TextSelectedBg, 0x0070E0, 0.5f); C(ImGuiCol_NavHighlight, 0x0070E0);
    auto V = [](ImGuiStyleVar v, float f) { ImGui::PushStyleVar(v, f); g_ueVars++; };
    auto V2v = [](ImGuiStyleVar v, ImVec2 f) { ImGui::PushStyleVar(v, f); g_ueVars++; };
    V(ImGuiStyleVar_WindowRounding, 0); V(ImGuiStyleVar_ChildRounding, 0); V(ImGuiStyleVar_FrameRounding, Px(3));
    V(ImGuiStyleVar_PopupRounding, Px(3)); V(ImGuiStyleVar_TabRounding, Px(3)); V(ImGuiStyleVar_GrabRounding, Px(2));
    V(ImGuiStyleVar_WindowBorderSize, 1.f); V(ImGuiStyleVar_FrameBorderSize, 0.f);
    V2v(ImGuiStyleVar_FramePadding, ImVec2(Px(6), Px(3))); V2v(ImGuiStyleVar_ItemSpacing, ImVec2(Px(6), Px(4)));
}
static void UeThemePop() { ImGui::PopStyleColor(g_ueCols); ImGui::PopStyleVar(g_ueVars); g_ueCols = g_ueVars = 0; }

// ---- placing: a game model path, "class:<actor class>" or "model:<imported .uemodel file>"; nx < 0 = where you're looking
static void UeSpawnAt(const std::string& path, float nx, float ny) {
    std::string at;
    if (nx >= 0) { char b[48]; snprintf(b, sizeof b, "\t%.4f,%.4f", nx, ny); at = b; }
    if (path.rfind("class:", 0) == 0) Ed("ed_spawnclass", path.substr(6) + at);
    else if (path.rfind("model:", 0) == 0) Ed("ed_modelspawn", path.substr(6) + at);
    else Ed("ed_spawn", path + at);
    g_olRefreshAt = GetTickCount64() + 800;              // the Outliner lists it a moment later
    g_edSelectTab = 2;
}
static void UeRemember(const char* icon, const std::string& name, const std::string& path) {
    for (size_t i = 0; i < g_ueRecent.size(); i++) if (g_ueRecent[i].path == path) { g_ueRecent.erase(g_ueRecent.begin() + i); break; }
    g_ueRecent.insert(g_ueRecent.begin(), { icon, name, path });
    if (g_ueRecent.size() > 12) g_ueRecent.pop_back();
}
static void UeStartPlace(const char* icon, const std::string& name, const std::string& path) {
    g_placePath = path; g_placeName = name; UeRemember(icon, name, path);
    g_toasts.push_back({ "Click in the game to place " + name + "  (Esc cancels, hold Shift for more)", false, GetTickCount64() });
}
// start dragging the last item out of its panel (dropped on the game, it's placed there)
static void UeDragSource(const char* icon, const std::string& name, const std::string& path) {
    if (g_dragPath.empty() && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, Px(8))) {
        g_dragPath = path; g_dragName = name; g_dragCtx = ImGui::GetCurrentContext(); g_placePath.clear();
        UeRemember(icon, name, path);
    }
}
// the drag ends in the window it started in (the mouse belongs to it until let go)
static void UeDragEnd() {
    if (g_dragPath.empty() || g_dragCtx != ImGui::GetCurrentContext() || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    POINT sm = ScreenMouse();
    const RECT& L = g_ueLayerScreen;
    float lx = (float)(sm.x - L.left), ly = (float)(sm.y - L.top);
    if (L.right > L.left && lx >= g_ueVpMinF.x && lx < g_ueVpMaxF.x && ly >= g_ueVpMinF.y && ly < g_ueVpMaxF.y && g_ueGameSize.x > 0)
        UeSpawnAt(g_dragPath, (lx - g_ueGamePos.x) / g_ueGameSize.x, (ly - g_ueGamePos.y) / g_ueGameSize.y);
    g_dragPath.clear(); g_dragCtx = nullptr;
}

// ---- Outliner: every actor in the level, in folders by type
static char g_olFilter[128] = "";
static std::string g_olLastSel;
static void UeOutliner() {
    float bw = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-(bw + Px(6)) * 2);
    ImGui::InputTextWithHint("##olf", ICON_SEARCH "  Search...", g_olFilter, sizeof g_olFilter);
    ImGui::SameLine(0, Px(6));
    if (ImGui::Button(ICON_ROTATE "##olr", ImVec2(bw, 0))) Ed("ed_actors", "");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh the list");
    ImGui::SameLine(0, Px(6));
    if (ImGui::Button(ICON_CROSSHAIRS "##olp", ImVec2(bw, 0))) { Ed("ed_pick"); g_edSelectTab = 2; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select whatever is in the middle of the screen");
    std::string f = UeLower(g_olFilter);
    std::map<std::string, std::vector<int>> groups;
    for (int i = 0; i < (int)g_actors.size(); i++) {
        auto& a = g_actors[i];
        if (!f.empty() && UeLower(a.name).find(f) == std::string::npos && UeLower(a.cls).find(f) == std::string::npos) continue;
        groups[a.cls].push_back(i);
    }
    bool newSel = g_ed.sel != g_olLastSel; g_olLastSel = g_ed.sel;
    float footer = ImGui::GetTextLineHeightWithSpacing() + Px(2);
    if (ImGui::BeginTable("##ol", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                          ImVec2(0, ImGui::GetContentRegionAvail().y - footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item Label", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableHeadersRow();
        for (auto& g : groups) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            bool hasSel = false; for (int i : g.second) if (g_actors[i].path == g_ed.sel) hasSel = true;
            if ((newSel && hasSel) || !f.empty()) ImGui::SetNextItemOpen(true);
            ImVec2 ip = ImGui::GetCursorScreenPos();
            bool open = ImGui::TreeNodeEx(g.first.c_str(), ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick, "      %s", g.first.c_str());
            ImGui::GetWindowDrawList()->AddText(ImVec2(ip.x + ImGui::GetTreeNodeToLabelSpacing(), ip.y + ImGui::GetStyle().FramePadding.y * 0), ImGui::GetColorU32(Hex(0xC8A050)), UI_FOLDER);
            ImGui::TableNextColumn(); ImGui::TextDisabled("Folder (%d)", (int)g.second.size());
            if (!open) continue;
            for (int i : g.second) {
                auto& a = g_actors[i];
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::PushID(i);
                bool sel = a.path == g_ed.sel;
                if (sel && newSel) ImGui::SetScrollHereY(0.4f);
                if (ImGui::Selectable(a.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    Ed("ed_select", a.path); g_edSelectTab = 2;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Ed("ed_focus", a.path);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%.0f m away\nDouble-click to fly to it (F)", a.path.c_str(), a.dist / 100);
                if (ImGui::BeginPopupContextItem("##olm")) {
                    if (ImGui::MenuItem("Select")) { Ed("ed_select", a.path); g_edSelectTab = 2; }
                    if (ImGui::MenuItem("Focus", "F")) Ed("ed_focus", a.path);
                    if (ImGui::MenuItem("Teleport the player there")) Ed("ed_goto", a.path);
                    if (ImGui::MenuItem("Bring in front of me")) Ed("ed_bring", a.path);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) { Ed("ed_dupe", a.path); g_olRefreshAt = GetTickCount64() + 800; }
                    if (ImGui::MenuItem("Hide")) Ed("ed_hide", a.path + "\t1");
                    if (ImGui::MenuItem("Show")) Ed("ed_hide", a.path + "\t0");
                    if (ImGui::MenuItem(ICON_TRASH "  Delete")) { Ed("ed_destroy", a.path); g_olRefreshAt = GetTickCount64() + 800; }
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", a.cls.c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::EndTable();
    }
    ImGui::PushFont(F_small);
    if (g_actorTotal > (int)g_actors.size()) ImGui::TextDisabled("%d actors (nearest %d listed - search to narrow it down)", g_actorTotal, (int)g_actors.size());
    else ImGui::TextDisabled("%d actors", (int)g_actors.size());
    ImGui::PopFont();
}

// ---- Details: the selected object (plus the player and the camera, as tabs like Unreal's World Settings)
static void UeDetails() {
    if (ImGui::BeginTabBar("##dt")) {
        struct { const char* name; void (*fn)(); } tabs[] = { { "Details", TabInspector }, { "Player", TabPlayerInGame }, { "Camera", TabCamera } };
        for (int i = 0; i < 3; i++) {
            ImGuiTabItemFlags fl = (i == 0 && g_edSelectTab == 2) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(tabs[i].name, nullptr, fl)) {
                ImGui::BeginChild("##dtb", ImVec2(0, 0), 0);
                tabs[i].fn();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

// ---- Place Actors
struct UePlaceItem { const char* icon; std::string name, path, tip; };
static int g_placeCat = 1;
static char g_placeFilter[96] = "";
static std::vector<UePlaceItem> UePlaceList(int cat) {
    std::vector<UePlaceItem> v;
    switch (cat) {
    case 0: for (auto& r : g_ueRecent) v.push_back({ nullptr, r.name, r.path, "" }); break;
    case 1:
        v.push_back({ ICON_CUBE, "Cube", "/Engine/BasicShapes/Cube.Cube", "Unreal's basic cube (if the game includes it)" });
        v.push_back({ ICON_CIRCLE, "Sphere", "/Engine/BasicShapes/Sphere.Sphere", "Unreal's basic sphere (if the game includes it)" });
        v.push_back({ UI_DB, "Cylinder", "/Engine/BasicShapes/Cylinder.Cylinder", "Unreal's basic cylinder (if the game includes it)" });
        v.push_back({ UI_CARETUP, "Cone", "/Engine/BasicShapes/Cone.Cone", "Unreal's basic cone (if the game includes it)" });
        v.push_back({ UI_SQUARE, "Plane", "/Engine/BasicShapes/Plane.Plane", "Unreal's basic flat plane (if the game includes it)" });
        break;
    case 2:
        v.push_back({ UI_BULB, "Point Light", "class:/Script/Engine.PointLight", "Light shining in every direction" });
        v.push_back({ UI_BULB, "Spot Light", "class:/Script/Engine.SpotLight", "Cone of light (rotate it to aim)" });
        v.push_back({ UI_BULB, "Rect Light", "class:/Script/Engine.RectLight", "Light from a flat rectangle (Unreal 4.22+)" });
        v.push_back({ ICON_SUN, "Directional Light", "class:/Script/Engine.DirectionalLight", "Sunlight over the whole level" });
        break;
    case 3:
        v.push_back({ ICON_CAMERA, "Camera Actor", "class:/Script/Engine.CameraActor", "A camera placed in the level" });
        v.push_back({ UI_FONT, "Text Render", "class:/Script/Engine.TextRenderActor", "Text floating in the level" });
        v.push_back({ UI_GLOBE, "Exponential Height Fog", "class:/Script/Engine.ExponentialHeightFog", "Fog that thickens lower down" });
        break;
    case 4:
        ImpPoll();
        for (auto& m : g_imp) v.push_back({ ICON_BOX_OPEN, m.name, "model:" + W2U(m.file), std::to_string(m.tris) + " triangles" + (m.anims.empty() ? "" : ", animated") });
        break;
    }
    return v;
}
static void UePlaceRow(const UePlaceItem& it, int id) {
    ImGui::PushID(id);
    const char* icon = it.icon ? it.icon : ICON_CLOCK;
    bool placing = g_placePath == it.path;
    float h = Px(30);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##pl", placing, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, h))) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { UeSpawnAt(it.path, -1, -1); UeRemember(icon, it.name, it.path); g_placePath.clear(); }
        else UeStartPlace(icon, it.name, it.path);
    }
    UeDragSource(icon, it.name, it.path);
    if (ImGui::IsItemHovered() && g_dragPath.empty())
        ImGui::SetTooltip("%s%sClick, then click in the game to place it.\nOr drag it onto the game. Double-click: where you're looking.", it.tip.c_str(), it.tip.empty() ? "" : "\n");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float box = h - Px(4);
    dl->AddRectFilled(ImVec2(p0.x + Px(2), p0.y + Px(2)), ImVec2(p0.x + Px(2) + box, p0.y + Px(2) + box), ImGui::GetColorU32(Hex(0x0F0F0F)), Px(3));
    ImVec2 is = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p0.x + Px(2) + (box - is.x) / 2, p0.y + Px(2) + (box - is.y) / 2), ImGui::GetColorU32(Hex(0xD0D0D0)), icon);
    dl->AddText(ImVec2(p0.x + box + Px(12), p0.y + (h - ImGui::GetTextLineHeight()) / 2), ImGui::GetColorU32(ImGuiCol_Text), it.name.c_str());
    ImGui::PopID();
}
static void UePlaceActors(bool popup = false) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##plf", ICON_SEARCH "  Search Classes", g_placeFilter, sizeof g_placeFilter);
    struct { const char* icon; const char* name; } cats[] = { { ICON_CLOCK, "Recently Placed" }, { ICON_CUBE, "Basic" }, { UI_BULB, "Lights" },
                                                              { ICON_EYE, "Visual Effects" }, { ICON_BOX_OPEN, "Imported Models" } };
    std::string f = UeLower(g_placeFilter);
    if (!f.empty()) {                                     // search: every category
        ImGui::BeginChild("##plr", ImVec2(0, popup ? Px(300) : 0));
        int n = 0;
        for (int c = 1; c < 5; c++) for (auto& it : UePlaceList(c)) if (UeLower(it.name).find(f) != std::string::npos) UePlaceRow(it, n++);
        if (!n) ImGui::TextDisabled("Nothing called that.");
        ImGui::EndChild();
        return;
    }
    float cw = Px(popup ? 150 : 46);
    ImGui::BeginChild("##plc", ImVec2(cw, popup ? Px(300) : 0), ImGuiChildFlags_Borders);
    for (int c = 0; c < 5; c++) {
        ImGui::PushID(c);
        bool s = g_placeCat == c;
        if (popup) { if (ImGui::Selectable((std::string(cats[c].icon) + "  " + cats[c].name).c_str(), s)) g_placeCat = c; }
        else {
            if (ImGui::Selectable(cats[c].icon, s, 0, ImVec2(0, Px(30)))) g_placeCat = c;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cats[c].name);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, Px(4));
    ImGui::BeginChild("##pll", ImVec2(0, popup ? Px(300) : 0));
    ImGui::PushFont(F_small); ImGui::TextDisabled("%s", cats[g_placeCat].name); ImGui::PopFont();
    auto list = UePlaceList(g_placeCat);
    for (int i = 0; i < (int)list.size(); i++) UePlaceRow(list[i], i);
    if (list.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", g_placeCat == 0 ? "Things you place show up here." : "No imported models yet. Import one from the Content Browser.");
        ImGui::PopTextWrapPos();
    }
    if (g_placeCat == 4) { ImGui::Dummy(ImVec2(0, Px(4))); if (ImGui::Button(ICON_FILE_IMPORT "  Import a model...", ImVec2(-1, 0))) ImpImportPick(); }
    ImGui::EndChild();
}

// ---- Content Browser: the game's models in their folders, your imported models and materials
struct CbDir { std::string name, full; std::map<std::string, int> kids; std::vector<int> items; int total = 0; int parent = -1; };
static std::vector<CbDir> g_cb;
static size_t g_cbBuiltFor = (size_t)-1;
static int g_cbSel = -2;                                 // a folder, or -2 imported models, -3 materials, -4 material library, -5 texture library
static char g_cbFilter[128] = "";
static float g_cbZoom = 1.f;
// Back / Forward like Unreal's Content Browser: every folder you open goes on the history
static std::vector<int> g_cbBack, g_cbFwd;
static void CbGo(int d) {
    if (d == g_cbSel) return;
    g_cbBack.push_back(g_cbSel); if (g_cbBack.size() > 100) g_cbBack.erase(g_cbBack.begin());
    g_cbFwd.clear();
    g_cbSel = d; g_cbFilter[0] = 0;
}
static void CbBackStep() { if (g_cbBack.empty()) return; g_cbFwd.push_back(g_cbSel); g_cbSel = g_cbBack.back(); g_cbBack.pop_back(); g_cbFilter[0] = 0; }
static void CbFwdStep() { if (g_cbFwd.empty()) return; g_cbBack.push_back(g_cbSel); g_cbSel = g_cbFwd.back(); g_cbFwd.pop_back(); g_cbFilter[0] = 0; }
static void CbResetForNewGame() {
    g_cb.clear(); g_cbBuiltFor = (size_t)-1;
    if (g_cbSel >= 0) g_cbSel = 0;       // a folder of the old game: back to the top
    g_cbBack.clear(); g_cbFwd.clear(); g_cbFilter[0] = 0;
}
static int CbParent() { return g_cbSel > 0 && g_cbSel < (int)g_cb.size() ? g_cb[g_cbSel].parent : -1; }
static void CbBuild() {
    std::lock_guard<std::mutex> lk(g_pakModelsMx);
    if (g_pakModels.size() == g_cbBuiltFor) return;
    g_cbBuiltFor = g_pakModels.size();
    g_cb.clear(); g_cb.push_back({ "All", "" });
    for (int i = 0; i < (int)g_pakModels.size(); i++) {
        std::string p = g_pakModels[i];
        size_t dot = p.find('.', p.find_last_of('/') + 1); if (dot != std::string::npos) p = p.substr(0, dot);
        int cur = 0; g_cb[0].total++;
        size_t a = 0;
        while (true) {
            while (a < p.size() && p[a] == '/') a++;
            size_t b = p.find('/', a);
            if (b == std::string::npos) break;            // the last part is the model's own name
            std::string seg = p.substr(a, b - a);
            auto it = g_cb[cur].kids.find(seg);
            int k;
            if (it == g_cb[cur].kids.end()) { k = (int)g_cb.size(); std::string full = g_cb[cur].full + "/" + seg; g_cb[cur].kids[seg] = k; g_cb.push_back({ seg, full }); g_cb.back().parent = cur; }
            else k = it->second;
            cur = k; g_cb[cur].total++;
            a = b + 1;
        }
        g_cb[cur].items.push_back(i);
    }
    if (g_cbSel >= (int)g_cb.size()) g_cbSel = 0;
    g_cbBack.clear(); g_cbFwd.clear();                  // folder numbers changed
}
static void CbTree(int d) {
    CbDir& n = g_cb[d];
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | (n.kids.empty() ? ImGuiTreeNodeFlags_Leaf : 0) | (g_cbSel == d ? ImGuiTreeNodeFlags_Selected : 0);
    if (d == 0) fl |= ImGuiTreeNodeFlags_DefaultOpen;
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)(d + 1), fl, UI_FOLDER "  %s", n.name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) CbGo(d);
    if (open) { for (auto& k : n.kids) CbTree(k.second); ImGui::TreePop(); }
}
static void CbFolderTile(int d, float cw) {
    ImGui::PushID(-d - 10);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeight() + Px(6);
    ImGui::InvisibleButton("##fd", ImVec2(cw, cw + lh));
    bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) CbGo(d);
    if (hov) ImGui::SetTooltip("%s\n%d models\nDouble-click to open", g_cb[d].full.c_str(), g_cb[d].total);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + cw, p0.y + cw + lh), ImGui::GetColorU32(hov ? Hex(0x2F2F2F) : Hex(0x242424)), Px(4));
    ImGui::PushFont(F_h2);
    ImVec2 is = ImGui::CalcTextSize(UI_FOLDER);
    float sc = cw * 0.45f / std::max(1.f, is.x);
    dl->AddText(F_h2, F_h2->FontSize * sc, ImVec2(p0.x + (cw - is.x * sc) / 2, p0.y + (cw - is.y * sc) / 2), ImGui::GetColorU32(Hex(0xC8A050)), UI_FOLDER);
    ImGui::PopFont();
    ImGui::PushFont(F_small);
    std::string label = g_cb[d].name; float maxw = cw - Px(8);
    while (label.size() > 1 && ImGui::CalcTextSize(label.c_str()).x > maxw) label.pop_back();
    ImVec2 ls = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(p0.x + (cw - ls.x) / 2, p0.y + cw + Px(1)), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
    ImGui::PopFont();
    ImGui::PopID();
}
static void UeContentBrowser() {
    // toolbar: Import, Export ticked, Models folder
    if (AccentButton(ICON_FILE_IMPORT "  Import")) ImpImportPick();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import a model: .fbx, .psk (with .psa animations), .gltf / .glb or .obj");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_pakTicked.empty() || g_impBusy > 0);
    if (ImGui::Button(ICON_DOWNLOAD "  Export ticked")) {
        std::vector<std::pair<std::string, int>> l; for (auto& p : g_pakTicked) l.push_back({ p, 0 });
        ExportModelsFromGame(l); g_pakTicked.clear();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+click models to tick them, then export them all into Imported Models");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FOLDER_OPEN "  Models folder")) { MakeDirs(ModelsDir()); OpenFolder(ModelsDir()); }
    if (g_impBusy > 0) { ImGui::SameLine(); ImGui::TextDisabled(ICON_HOURGLASS " Working..."); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(90));
    ImGui::SliderFloat("##cbz", &g_cbZoom, 0.6f, 2.0f, "Size");
    // left: folders
    if (g_pakModelsState == 0) LoadGameModels();
    CbBuild();
    float treeW = Px(210);
    ImGui::BeginChild("##cbt", ImVec2(treeW, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    if (ImGui::Selectable(ICON_BOX_OPEN "  Imported Models", g_cbSel == -2)) CbGo(-2);
    if (ImGui::Selectable(ICON_PALETTE "  Materials", g_cbSel == -3)) CbGo(-3);
    if (ImGui::Selectable(ICON_CUBES "  Material Library", g_cbSel == -4)) CbGo(-4);
    if (ImGui::Selectable(ICON_IMAGE "  Texture Library", g_cbSel == -5)) CbGo(-5);
    ImGui::Separator();
    ImGui::TextDisabled(ICON_GAMEPAD "  Game");
    if (!g_cb.empty()) CbTree(0);
    if (g_pakModelsState == 1) { ImGui::PushFont(F_small); ImGui::TextDisabled("Reading the game's files... %d / %d", (int)g_pakDone, (int)g_pakTotal); ImGui::PopFont(); }
    else if (g_pakModelsState == 2 && g_cb.size() <= 1) {
        ImGui::PushFont(F_small); ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", g_pakModelsNote.empty() ? "No models found in the game's files." : g_pakModelsNote.c_str());
        ImGui::PopTextWrapPos(); ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, Px(4));
    ImGui::BeginChild("##cbr", ImVec2(0, 0));
    // Back / Forward / Up, and the mouse's back and forward buttons, Alt+Left / Alt+Right and Backspace
    {
        ImGuiIO& io = ImGui::GetIO();
        bool here = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        bool typing = io.WantTextInput;
        if (here && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle + 1) || (!typing && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)))) CbBackStep();
        if (here && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle + 2) || (!typing && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)))) CbFwdStep();
        if (here && !typing && ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && CbParent() >= 0) CbGo(CbParent());
        ImGui::BeginDisabled(g_cbBack.empty());
        if (ImGui::Button(ICON_ARROW_LEFT "##cbback")) CbBackStep();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Back to the previous folder (mouse back button, Alt+Left)");
        ImGui::SameLine(0, Px(2));
        ImGui::BeginDisabled(g_cbFwd.empty());
        if (ImGui::Button(ICON_ARROW_RIGHT "##cbfwd")) CbFwdStep();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Forward (mouse forward button, Alt+Right)");
        ImGui::SameLine(0, Px(2));
        ImGui::BeginDisabled(CbParent() < 0);
        if (ImGui::Button(ICON_ARROW_UP "##cbup")) CbGo(CbParent());
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Up to the folder this one is in (Backspace)");
        ImGui::SameLine();
        // the path: click any part of it to go there
        if (g_cbSel < -1) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", g_cbSel == -2 ? "Imported Models" : g_cbSel == -3 ? "Materials" : g_cbSel == -4 ? "Material Library" : "Texture Library"); }
        else if (g_cbSel >= 0 && g_cbSel < (int)g_cb.size()) {
            std::vector<int> chain; for (int c = g_cbSel; c >= 0; c = g_cb[c].parent) chain.insert(chain.begin(), c);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(4), ImGui::GetStyle().FramePadding.y));
            for (size_t i = 0; i < chain.size(); i++) {
                if (i) { ImGui::SameLine(0, 0); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled(">"); ImGui::SameLine(0, 0); }
                ImGui::PushID((int)i);
                const char* nm = chain[i] == 0 ? "Game" : g_cb[chain[i]].name.c_str();
                if (ImGui::Button(nm) && chain[i] != g_cbSel) CbGo(chain[i]);
                ImGui::PopID();
            }
            ImGui::PopStyleVar(); ImGui::PopStyleColor();
        }
    }
    if (g_cbSel == -2) { ImportedModelsUI(true); ImGui::EndChild(); return; }
    if (g_cbSel == -3) { MaterialsUI(true); ImGui::EndChild(); return; }
    if (g_cbSel == -4) { UeMaterialLibraryUI(); ImGui::EndChild(); return; }
    if (g_cbSel == -5) { UeTextureLibraryUI(); ImGui::EndChild(); return; }
    if (g_cbSel < 0 || g_cbSel >= (int)g_cb.size()) { ImGui::EndChild(); return; }
    // path bar + search
    CbDir& d = g_cb[g_cbSel];
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##cbf", ICON_SEARCH "  Search this folder", g_cbFilter, sizeof g_cbFilter);
    // what to show: sub-folders then models here; a search looks through every folder below this one
    std::vector<int> items; std::vector<int> dirs;
    std::string f = UeLower(g_cbFilter);
    std::vector<std::string> paths; std::vector<uint8_t> flags;
    {
        std::lock_guard<std::mutex> lk(g_pakModelsMx);
        if (f.empty()) { items = d.items; for (auto& k : d.kids) dirs.push_back(k.second); }
        else {
            std::string pre = UeLower(d.full) + "/";
            for (int i = 0; i < (int)g_pakLower.size() && items.size() < 600; i++)
                if ((d.full.empty() || g_pakLower[i].rfind(pre, 0) == 0) && g_pakLower[i].find(f, g_pakLower[i].find_last_of('/')) != std::string::npos) items.push_back(i);
        }
        for (int i : items) { paths.push_back(i < (int)g_pakModels.size() ? g_pakModels[i] : ""); flags.push_back(i < (int)g_pakFlags.size() ? g_pakFlags[i] : 0); }
    }
    ImGui::BeginChild("##cbg", ImVec2(0, 0));
    float cw = Px(92) * g_cbZoom, sp = Px(8);
    int cols = std::max(1, (int)((ImGui::GetContentRegionAvail().x + sp) / (cw + sp)));
    int n = (int)dirs.size() + (int)paths.size();
    int rows = (n + cols - 1) / cols;
    float rowH = cw + ImGui::GetTextLineHeight() + Px(6) + sp;
    ImGuiListClipper clip; clip.Begin(rows, rowH);
    while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
        for (int c = 0; c < cols; c++) {
            int k = r * cols + c; if (k >= n) break;
            if (c) ImGui::SameLine(0, sp);
            if (k < (int)dirs.size()) CbFolderTile(dirs[k], cw);
            else {
                int j = k - (int)dirs.size();
                ModelCard(100000 + j, paths[j], ShortName(paths[j]), cw, flags[j]);
                UeDragSource(ICON_CUBE, ShortName(paths[j]), paths[j]);
            }
        }
        ImGui::Dummy(ImVec2(0, sp - ImGui::GetStyle().ItemSpacing.y));
    }
    if (n == 0) ImGui::TextDisabled(f.empty() ? "This folder has no models." : "No models match.");
    ImGui::EndChild();
    ThumbFlush();
    ImGui::EndChild();
}

static void UePanelBody(int i) {
    switch (i) { case UP_OUTLINER: UeOutliner(); break; case UP_DETAILS: UeDetails(); break; case UP_PLACE: UePlaceActors(); break; case UP_CONTENT: UeContentBrowser(); break; }
}

// ---- a docked panel: Unreal's tab-style header (name, undock, close) over its contents
static void UeDockedPanel(int i, ImVec2 pos, ImVec2 size) {
    ImGui::SetNextWindowPos(pos); ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    char id[48]; snprintf(id, sizeof id, "##uep%d", i);
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float hh = Px(28);
    ImVec2 w0 = ImGui::GetWindowPos();
    dl->AddRectFilled(w0, ImVec2(w0.x + size.x, w0.y + hh), ImGui::GetColorU32(Hex(0x0F0F0F)));
    std::string t = std::string(g_upIcons[i]) + "  " + g_upNames[i];
    ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    ImVec2 t0(w0.x + Px(4), w0.y + Px(3)), t1(t0.x + ts.x + Px(24), w0.y + hh);
    dl->AddRectFilled(t0, t1, ImGui::GetColorU32(Hex(0x1A1A1A)), Px(4), ImDrawFlags_RoundCornersTop);
    dl->AddText(ImVec2(t0.x + Px(12), t0.y + (hh - Px(3) - ts.y) / 2), ImGui::GetColorU32(Hex(0xE0E0E0)), t.c_str());
    // undock / close
    float bs = Px(22);
    ImGui::SetCursorScreenPos(ImVec2(w0.x + size.x - bs * 2 - Px(10), w0.y + (hh - bs) / 2));
    ImGui::PushStyleColor(ImGuiCol_Button, Hex(0, 0.f));
    if (ImGui::Button(UI_UNDOCK "##ud", ImVec2(bs, bs))) { g_upFloat[i] = true; SaveSettings(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undock: move %s into its own window (you can put it on another screen)", g_upNames[i]);
    ImGui::SameLine(0, Px(2));
    if (ImGui::Button(UI_XMARK "##cl", ImVec2(bs, bs))) { g_upOpen[i] = false; SaveSettings(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close (Window menu opens it again)");
    ImGui::PopStyleColor();
    // contents
    ImGui::SetCursorScreenPos(ImVec2(w0.x, w0.y + hh));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(8), Px(8)));
    ImGui::BeginChild("##body", ImVec2(size.x, size.y - hh), ImGuiChildFlags_AlwaysUseWindowPadding);
    UePanelBody(i);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}
// a bar between two panels, dragged to resize them
static bool UeSplitter(const char* id, ImVec2 pos, ImVec2 size, bool vertical, float& value, float sign, float minV, float maxV, float scaleTo = 0) {
    ImGui::SetNextWindowPos(pos); ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Hex(0x050505));
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::InvisibleButton("##s", size);
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    if (hov || act) {
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(Hex(0x0070E0, act ? 1.f : 0.6f)));
    }
    bool changed = false;
    if (act) {
        float d = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        if (d != 0) { value = std::clamp(value + sign * (scaleTo > 0 ? d / scaleTo : d / S()), minV, maxV); changed = true; }
    }
    if (ImGui::IsItemDeactivated()) SaveSettings();
    ImGui::End();
    ImGui::PopStyleColor(); ImGui::PopStyleVar(3);
    return changed;
}

// ---- menu bar + main toolbar along the top
static void UeMenuBar(float w) {
    if (!ImGui::BeginMenuBar()) return;
    if (g_logo) { float d = ImGui::GetFrameHeight() - Px(4); ImGui::Image((ImTextureID)(intptr_t)g_logo, ImVec2(d, d)); }
    if (ImGui::BeginMenu("File")) {
        bool on = g_ed.layoutOn;
        if (ImGui::MenuItem(UI_SAVE "  Save my changes in this level", nullptr, on)) Ed("ed_layout", on ? "off" : "on");
        if (ImGui::MenuItem("Put saved changes back", nullptr, false, g_ed.layoutCount > 0)) Ed("ed_layout", "reapply");
        if (ImGui::MenuItem("Forget saved changes for this level", nullptr, false, g_ed.layoutCount > 0)) Ed("ed_layout", "reset");
        ImGui::Separator();
        if (ImGui::MenuItem("Share this level's layout...")) LayoutShare(false);
        if (ImGui::MenuItem("Share every level's layout...")) LayoutShare(true);
        if (ImGui::MenuItem("Load a shared layout...")) LayoutLoad();
        if (ImGui::MenuItem("Remove a loaded layout")) LayoutUnload();
        ImGui::Separator();
        if (ImGui::MenuItem("Import a model...")) ImpImportPick();
        ImGui::Separator();
        if (ImGui::MenuItem("Close editor", "F2")) OvHide(true);
        ImGui::EndMenu();
    }
    bool sel = !g_ed.sel.empty();
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, g_ed.undoCount > 0)) Ed("ed_undo");
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, sel)) { Ed("ed_dupe", g_ed.sel); g_olRefreshAt = GetTickCount64() + 800; }
        if (ImGui::MenuItem("Delete", ImGui::GetKeyName((ImGuiKey)g_edKeys.del), false, sel)) { Ed("ed_destroy", g_ed.sel); g_olRefreshAt = GetTickCount64() + 800; }
        if (ImGui::MenuItem(g_ed.shidden ? "Show" : "Hide", ImGui::GetKeyName((ImGuiKey)g_edKeys.hide), false, sel)) Ed("ed_hide", g_ed.sel + "\t" + (g_ed.shidden ? "0" : "1"));
        if (ImGui::MenuItem("Snap to the ground", ImGui::GetKeyName((ImGuiKey)g_edKeys.snapGround), false, sel)) Ed("ed_snap", g_ed.sel + "\tground");
        if (ImGui::MenuItem("Focus", "F", false, sel)) Ed("ed_focus", g_ed.sel);
        if (ImGui::MenuItem("Deselect", ImGui::GetKeyName((ImGuiKey)g_edKeys.deselect), false, sel)) Ed("ed_deselect");
        ImGui::Separator();
        if (ImGui::MenuItem("Select what's in the middle of the screen")) { Ed("ed_pick"); g_edSelectTab = 2; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        for (int i = 0; i < UP_COUNT; i++) if (ImGui::MenuItem((std::string(g_upIcons[i]) + "  " + g_upNames[i]).c_str(), nullptr, g_upOpen[i])) { g_upOpen[i] = !g_upOpen[i]; SaveSettings(); }
        ImGui::Separator();
        for (int i = 0; i < UP_COUNT; i++) {
            std::string l = (g_upFloat[i] ? "Dock " : "Undock ") + std::string(g_upNames[i]);
            if (ImGui::MenuItem(l.c_str(), nullptr, false, g_upOpen[i])) { g_upFloat[i] = !g_upFloat[i]; SaveSettings(); }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Fit the game between the panels", nullptr, g_ueFit)) { g_ueFit = !g_ueFit; g_ueFitBroken = false; g_ueFitFails = 0; SaveSettings(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: the game's picture shrinks and grows to fill the space between the panels, like Unreal's viewport.\nOff: the panels sit on top of the game.\n(Needs the game in Windowed or Borderless mode.)");
        ImGui::Separator();
        if (ImGui::MenuItem("Undock everything (one window for another screen)")) { for (int i = 0; i < UP_COUNT; i++) { g_upFloat[i] = true; g_upOpen[i] = true; } SaveSettings(); }
        if (ImGui::MenuItem("Reset layout")) {
            for (int i = 0; i < UP_COUNT; i++) { g_upOpen[i] = true; g_upFloat[i] = false; }
            g_upLeftW = 250; g_upRightW = 360; g_upBottomH = 240; g_upSplit = 0.42f; SaveSettings();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem(ICON_PALETTE "  Material Editor", nullptr, false, sel)) MatEdOpenFwd(g_ed.sel);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !sel) ImGui::SetTooltip("Select an object first");
        if (ImGui::BeginMenu("Game speed")) {
            for (float v : { 0.1f, 0.25f, 0.5f, 1.f, 2.f, 4.f }) {
                char b[16]; snprintf(b, sizeof b, "%gx", v);
                if (ImGui::MenuItem(b, nullptr, fabsf(g_ueTime - v) < 0.001f)) { g_ueTime = v; char a[16]; snprintf(a, sizeof a, "%g", v); Ed("ed_timescale", a); }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("Shortcuts");
        ImGui::Separator();
        auto row = [](const char* a, const char* b) { ImGui::TextUnformatted(a); ImGui::SameLine(Px(190)); ImGui::TextDisabled("%s", b); };
        row("Select / Move / Rotate / Scale", (std::string(ImGui::GetKeyName((ImGuiKey)g_edKeys.select)) + " / " + ImGui::GetKeyName((ImGuiKey)g_edKeys.move) + " / " +
                                               ImGui::GetKeyName((ImGuiKey)g_edKeys.rotate) + " / " + ImGui::GetKeyName((ImGuiKey)g_edKeys.scale)).c_str());
        row("Fly the camera", "hold right mouse + W A S D, Q / E");
        row("Focus on the selection", "F (or double-click in the Outliner)");
        row("Duplicate", "Ctrl+D");
        row("Undo", "Ctrl+Z");
        row("Snap while dragging", "Ctrl");
        row("Place more copies", "hold Shift while clicking");
        row("Close the editor", "F2");
        ImGui::EndMenu();
    }
    // right: game and level
    std::string g = W2U(GameDisplayName()), m = g_bridge.currentMap.substr(g_bridge.currentMap.find_last_of('/') + 1);
    std::string info = g + (m.empty() ? "" : "  \xC2\xB7  " + m);
    float iw = ImGui::CalcTextSize(info.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + Px(20), w - iw - Px(16)));
    ImGui::TextDisabled("%s", info.c_str());
    ImGui::EndMenuBar();
}
static void UeToolbar() {
    float h = ImGui::GetContentRegionAvail().y, bh = Px(28);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - bh) / 2);
    // save (the level's layout)
    bool on = g_ed.layoutOn;
    if (on ? AccentButton(UI_SAVE "##sv", ImVec2(bh + Px(6), bh)) : ImGui::Button(UI_SAVE "##sv", ImVec2(bh + Px(6), bh))) Ed("ed_layout", on ? "off" : "on");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(on ? "Saving your changes in this level (%d saved). Click to stop." : "Click to save what you change in this level\n(it comes back the next time the level loads)", g_ed.layoutCount);
    ImGui::SameLine(0, Px(10));
    // + Add (Place Actors as a menu)
    if (ImGui::Button(ICON_PLUS "  Add  " "\xef\x83\x97", ImVec2(0, bh))) ImGui::OpenPopup("##addm");
    if (ImGui::BeginPopup("##addm")) { ImGui::SetNextWindowSize(ImVec2(Px(420), 0)); UePlaceActors(true); ImGui::EndPopup(); }
    ImGui::SameLine(0, Px(4));
    if (ImGui::Button(ICON_FILE_IMPORT "  Import", ImVec2(0, bh))) ImpImportPick();
    // middle: game time
    float mid = ImGui::GetWindowWidth() / 2 - (bh * 3 + Px(12)) / 2;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + Px(16), mid));
    auto timeBtn = [&](const char* icon, float v, const char* tip) {
        bool cur = fabsf(g_ueTime - v) < 0.001f;
        ImGui::PushStyleColor(ImGuiCol_Text, cur ? Hex(0x2FD24B) : Hex(0xC0C0C0));
        if (ImGui::Button(icon, ImVec2(bh + Px(6), bh))) { g_ueTime = v; char a[16]; snprintf(a, sizeof a, "%g", v); Ed("ed_timescale", a); }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    timeBtn(ICON_PLAY, 1.f, "Game runs at normal speed");
    ImGui::SameLine(0, Px(4)); timeBtn(UI_PAUSE, 0.0001f, "Freeze the game (time stops; you can still edit)");
    ImGui::SameLine(0, Px(4)); timeBtn(UI_FORWARD, 0.25f, "Slow motion (quarter speed)");
    // right: editing camera, close
    float right = ImGui::GetWindowWidth() - Px(12);
    ImGui::SameLine(right - Px(200) - bh);
    bool fc = g_edFreecam;
    if (fc ? AccentButton(ICON_VIDEO "  Editing camera", ImVec2(Px(200), bh)) : ImGui::Button(ICON_VIDEO "  Editing camera", ImVec2(Px(200), bh))) {
        g_edFreecam = !fc; SaveSettings(); Ed("ed_editcam", g_edFreecam ? "1" : "0");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(fc ? "Flying camera on: hold the right mouse button on the game + W A S D" : "Use the game's own camera");
    ImGui::SameLine(0, Px(6));
    if (ImGui::Button(UI_XMARK "##close", ImVec2(bh, bh))) OvHide(true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close the editor (F2)");
}
// the viewport's own toolbar: transform tools, snapping, camera speed
static void UeViewportToolbar() {
    float bh = Px(26);
    struct { int mode; const char* icon; const char* name; int key; } tools[] = {
        { GZ_SELECT, ICON_CROSSHAIRS, "Select", g_edKeys.select }, { GZ_MOVE, ICON_ARROWS, "Move", g_edKeys.move },
        { GZ_ROTATE, ICON_ROTATE, "Rotate", g_edKeys.rotate }, { GZ_SCALE, ICON_EXPAND, "Scale", g_edKeys.scale } };
    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine(0, Px(2));
        auto& tl = tools[i];
        if (g_gzMode == tl.mode ? AccentButton(tl.icon, ImVec2(bh + Px(4), bh)) : ImGui::Button(tl.icon, ImVec2(bh + Px(4), bh))) { g_gzMode = tl.mode; SaveSettings(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (%s)", tl.name, ImGui::GetKeyName((ImGuiKey)tl.key));
    }
    // snapping: on/off + step, for move / rotate / scale (like Unreal's grid, rotation and scale snaps)
    static const float steps[3][7] = { { 1, 5, 10, 25, 50, 100, 500 }, { 5, 10, 15, 30, 45, 60, 90 }, { 0.0625f, 0.125f, 0.25f, 0.5f, 1, 2, 10 } };
    const char* icons[3] = { "\xef\x80\x8a", "\xef\x80\xa1", "\xef\x81\xa5" };     // (grid, rotate, expand)
    const char* what[3] = { "Grid snap (move, cm)", "Rotation snap (degrees)", "Scale snap" };
    for (int k = 0; k < 3; k++) {
        ImGui::SameLine(0, k == 0 ? Px(14) : Px(6));
        ImGui::PushID(k);
        ImGui::PushStyleColor(ImGuiCol_Text, g_snapOn[k] ? Hex(0x2F8CF0) : Hex(0x9A9A9A));
        if (ImGui::Button(icons[k], ImVec2(bh, bh))) { g_snapOn[k] = !g_snapOn[k]; SaveSettings(); }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s (Ctrl snaps while dragging either way)", what[k], g_snapOn[k] ? "on" : "off");
        ImGui::SameLine(0, 0);
        char cur[16]; snprintf(cur, sizeof cur, "%g", g_snapStep[k]);
        ImGui::SetNextItemWidth(Px(k == 2 ? 62 : 50));
        if (ImGui::BeginCombo("##st", cur, ImGuiComboFlags_NoArrowButton)) {
            for (float v : steps[k]) { char b[16]; snprintf(b, sizeof b, "%g", v); if (ImGui::Selectable(b, v == g_snapStep[k])) { g_snapStep[k] = v; g_snapOn[k] = 1; SaveSettings(); } }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    // camera speed
    ImGui::SameLine(0, Px(14));
    ImGui::PushStyleColor(ImGuiCol_Text, Hex(0x9A9A9A)); ImGui::TextUnformatted(ICON_VIDEO); ImGui::PopStyleColor();
    ImGui::SameLine(0, Px(4));
    ImGui::SetNextItemWidth(Px(90));
    if (ImGui::SliderFloat("##cs", &g_flySpeed, 0.05f, 30.f, "%.2gx", ImGuiSliderFlags_Logarithmic)) SaveSettings();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera speed (or the mouse wheel while flying)");
    if (!g_placePath.empty()) { ImGui::SameLine(0, Px(14)); ImGui::TextColored(Hex(0x2F8CF0), "Click to place %s  \xC2\xB7  Esc cancels", g_placeName.c_str()); }
}

static void UeToasts(ImVec2 br) {
    ULONGLONG now = GetTickCount64();
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(), [&](const Toast& t) { return now - t.t > 3500; }), g_toasts.end());
    float y = br.y - Px(12);
    for (int i = (int)g_toasts.size() - 1, n = 0; i >= 0 && n < 4; i--, n++) {
        auto& t = g_toasts[i];
        ImVec2 ts = ImGui::CalcTextSize(t.text.c_str());
        float tw = ts.x + Px(46), th = ts.y + Px(18);
        y -= th;
        ImGui::SetNextWindowPos(ImVec2(br.x - Px(12) - tw, y)); ImGui::SetNextWindowSize(ImVec2(tw, th));
        char id[32]; snprintf(id, sizeof id, "##uetoast%d", i);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(12), Px(9))); ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(4));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Hex(0x242424, 0.97f));
        ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushStyleColor(ImGuiCol_Text, t.err ? P.err : P.ok); ImGui::TextUnformatted(t.err ? ICON_WARN : ICON_CIRCLE_CHECK); ImGui::PopStyleColor();
        ImGui::SameLine(0, Px(8)); ImGui::TextUnformatted(t.text.c_str());
        ImGui::End();
        ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
        y -= Px(6);
    }
}

// ---- the whole layout, drawn on the layer over the game every frame
static void UeDrawLayout() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 o = vp->Pos; float W = vp->Size.x, H = vp->Size.y;
    { RECT r; if (OvHostRect(r)) g_ueLayerScreen = r; }
    // keep the Outliner fresh: after adding / deleting, and on a new level
    ULONGLONG now = GetTickCount64();
    if (g_olRefreshAt && now >= g_olRefreshAt) { g_olRefreshAt = 0; Ed("ed_actors", ""); }
    if (g_bridge.currentMap != g_olMap) { g_olMap = g_bridge.currentMap; if (!g_olMap.empty()) g_olRefreshAt = now + 1500; }

    UeThemePush();
    ImGui::PushFont(F_small);
    float menuH = ImGui::GetFrameHeight() + Px(4), toolH = Px(40), topH = menuH + toolH;
    // menu bar + toolbar
    ImGui::SetNextWindowPos(o); ImGui::SetNextWindowSize(ImVec2(W, topH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(8), 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Hex(0x151515));
    ImGui::Begin("##uetop", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor(); ImGui::PopStyleVar();
    UeMenuBar(W);
    UeToolbar();
    ImGui::End();

    bool L = g_upOpen[UP_PLACE] && !g_upFloat[UP_PLACE], R1 = g_upOpen[UP_OUTLINER] && !g_upFloat[UP_OUTLINER];
    bool R2 = g_upOpen[UP_DETAILS] && !g_upFloat[UP_DETAILS], B = g_upOpen[UP_CONTENT] && !g_upFloat[UP_CONTENT];
    float sp = Px(4);
    float rw = (R1 || R2) ? std::clamp(Px(g_upRightW), Px(200), W * 0.5f) : 0;
    float lw = L ? std::clamp(Px(g_upLeftW), Px(150), W * 0.35f) : 0;
    float bh = B ? std::clamp(Px(g_upBottomH), Px(110), (H - topH) * 0.65f) : 0;
    float colH = H - topH;
    // right column: Outliner over Details
    if (R1 && R2) {
        float oh = std::clamp(colH * g_upSplit, Px(80), colH - Px(120));
        UeDockedPanel(UP_OUTLINER, ImVec2(o.x + W - rw, o.y + topH), ImVec2(rw, oh));
        UeDockedPanel(UP_DETAILS, ImVec2(o.x + W - rw, o.y + topH + oh + sp), ImVec2(rw, colH - oh - sp));
        UeSplitter("##spo", ImVec2(o.x + W - rw, o.y + topH + oh), ImVec2(rw, sp), false, g_upSplit, 1, 0.1f, 0.9f, colH);
    } else if (R1) UeDockedPanel(UP_OUTLINER, ImVec2(o.x + W - rw, o.y + topH), ImVec2(rw, colH));
    else if (R2) UeDockedPanel(UP_DETAILS, ImVec2(o.x + W - rw, o.y + topH), ImVec2(rw, colH));
    float rEdge = W - rw - (rw > 0 ? sp : 0);
    if (rw > 0) UeSplitter("##spr", ImVec2(o.x + rEdge, o.y + topH), ImVec2(sp, colH), true, g_upRightW, -1, 200, 1200);
    // bottom: Content Browser (left edge to the right column)
    float bTop = H - bh;
    if (B) {
        UeDockedPanel(UP_CONTENT, ImVec2(o.x, o.y + bTop), ImVec2(rEdge, bh));
        UeSplitter("##spb", ImVec2(o.x, o.y + bTop - sp), ImVec2(rEdge, sp), false, g_upBottomH, -1, 110, 1400);
    }
    float vBottom = B ? bTop - sp : H;
    // left: Place Actors
    if (L) {
        UeDockedPanel(UP_PLACE, ImVec2(o.x, o.y + topH), ImVec2(lw, vBottom - topH));
        UeSplitter("##spl", ImVec2(o.x + lw, o.y + topH), ImVec2(sp, vBottom - topH), true, g_upLeftW, 1, 150, 900);
    }
    // the viewport: what's left of the game
    g_ueVpMinF = ImVec2(o.x + (L ? lw + sp : 0), o.y + topH);
    g_ueVpMaxF = ImVec2(o.x + rEdge, o.y + vBottom);
    ImGui::GetBackgroundDrawList()->AddRect(ImVec2(g_ueVpMinF.x, g_ueVpMinF.y), ImVec2(g_ueVpMaxF.x, g_ueVpMaxF.y), ImGui::GetColorU32(Hex(0x050505)), 0, 0, Px(2));
    // viewport toolbar (top-left of the viewport, like Unreal's)
    ImGui::SetNextWindowPos(ImVec2(g_ueVpMinF.x + Px(8), g_ueVpMinF.y + Px(8)));
    ImGui::SetNextWindowBgAlpha(0.82f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(4), Px(4))); ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(4));
    ImGui::Begin("##uevpt", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    UeViewportToolbar();
    ImGui::End();
    ImGui::PopStyleVar(2);
    UeToasts(ImVec2(g_ueVpMaxF.x, g_ueVpMaxF.y));
    ImGui::PopFont();
    FileBrowserDraw();                                   // (a file picked from a docked panel)
    UeThemePop();

    // dragging something onto the game: show what, and place it where it's let go
    if (!g_dragPath.empty()) {
        POINT sm = ScreenMouse();
        ImVec2 m((float)(sm.x - g_ueLayerScreen.left), (float)(sm.y - g_ueLayerScreen.top));
        bool inVp = m.x >= g_ueVpMinF.x && m.x < g_ueVpMaxF.x && m.y >= g_ueVpMinF.y && m.y < g_ueVpMaxF.y;
        std::string t = (inVp ? "Place " : "Drag onto the game to place ") + g_dragName;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        ImVec2 ts = ImGui::CalcTextSize(t.c_str()), p(m.x + Px(18), m.y + Px(16));
        fg->AddRectFilled(ImVec2(p.x - Px(8), p.y - Px(4)), ImVec2(p.x + ts.x + Px(8), p.y + ts.y + Px(4)), ImGui::GetColorU32(Hex(0x242424, 0.95f)), Px(4));
        fg->AddText(p, ImGui::GetColorU32(inVp ? Hex(0x2F8CF0) : Hex(0xC0C0C0)), t.c_str());
    }
    UeDragEnd();
}

// ---- the separate window: undocked panels as tabs (and the Material Editor when it's open)
static void UeFloatingPanels() {
    if (ImGui::BeginTabBar("##uefl", ImGuiTabBarFlags_Reorderable)) {
        for (int i = 0; i < UP_COUNT; i++) {
            if (!g_upOpen[i] || !g_upFloat[i]) continue;
            bool open = true;
            std::string t = std::string(g_upIcons[i]) + "  " + g_upNames[i];
            if (ImGui::BeginTabItem(t.c_str(), &open, (i == UP_DETAILS && g_edSelectTab == 2) ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (ImGui::SmallButton(UI_DOCK "  Dock back")) { g_upFloat[i] = false; SaveSettings(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put %s back around the game", g_upNames[i]);
                ImGui::BeginChild("##flb", ImVec2(0, 0));
                UePanelBody(i);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (!open) { g_upOpen[i] = false; SaveSettings(); }
        }
        ImGui::EndTabBar();
    }
    UeDragEnd();
}
