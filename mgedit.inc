// ---- Material graph editing: undo / redo, copy / paste, the node library (palette), live node previews,
// automatic arrangement. (Included by mated.inc, after the graph's data and before the canvas.)


// ---- undo / redo: the whole graph is kept after every change (they're small)
struct MeState { std::vector<MeNode> nodes; std::map<int, int> links, linkOut; std::map<long long, std::pair<int, int>> nin; MeProps props; };
static std::vector<MeState> g_meUndo, g_meRedo;
static MeState g_meLast; static std::string g_meLastKey; static bool g_meHaveLast = false;
static std::string MeStateKey(const MeState& s) {
    std::string k = MePropsLine(s.props);
    for (auto& n : s.nodes) k += MeNodeLine(n);
    for (auto& l : s.links) k += "l" + std::to_string(l.first) + "," + std::to_string(l.second) + "," + std::to_string(s.linkOut.count(l.first) ? s.linkOut.at(l.first) : 0) + ";";
    for (auto& kv : s.nin) k += "i" + std::to_string(kv.first) + "," + std::to_string(kv.second.first) + "," + std::to_string(kv.second.second) + ";";
    return k;
}
static MeState MeCapture() { MeState s; s.nodes = M.nodes; s.links = M.links; s.linkOut = M.linkOut; s.nin = M.nin; s.props = M.props; return s; }
static void MeHistoryReset() { g_meUndo.clear(); g_meRedo.clear(); g_meHaveLast = false; }
// what a setting of the material gets from the graph (its node's value, or the material's own when nothing feeds it)
static std::string MePinValue(const MeState& s, int pin) {
    auto it = s.links.find(pin);
    if (it == s.links.end()) return M.pins[pin].kind == MEK_MAT ? "" : M.pins[pin].orig;
    for (auto& n : s.nodes) if (n.id == it->second) return MeParamValue(n);
    return M.pins[pin].orig;
}
// once a frame, when nothing is being dragged: if the graph changed since last time, that's one step to undo
static void MeHistoryTrack() {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) return;
    MeState cur = MeCapture(); std::string key = MeStateKey(cur);
    if (!g_meHaveLast) { g_meLast = cur; g_meLastKey = key; g_meHaveLast = true; return; }
    if (key == g_meLastKey) return;
    g_meUndo.push_back(std::move(g_meLast)); if (g_meUndo.size() > 200) g_meUndo.erase(g_meUndo.begin());
    g_meRedo.clear();
    g_meLast = std::move(cur); g_meLastKey = key;
    if (!M.asset.empty()) M.dirty = true;
}
static void MeRestore(const MeState& s) {
    MeState before = MeCapture();
    M.nodes = s.nodes; M.links = s.links; M.linkOut = s.linkOut; M.nin = s.nin; M.props = s.props;
    M.selSet.clear(); if (!MeFind(M.sel)) M.sel = -1;
    // the material's own settings follow the graph back
    for (int i = 0; i < M.stdStart && i < (int)M.pins.size(); i++) if (MePinValue(before, i) != MePinValue(s, i)) MeSend(i, MePinValue(s, i), true);
    MeStdChanged();
    g_meLast = s; g_meLastKey = MeStateKey(s); g_meHaveLast = true;
}
static void MeUndo() { if (g_meUndo.empty()) return; g_meRedo.push_back(MeCapture()); MeState s = g_meUndo.back(); g_meUndo.pop_back(); MeRestore(s); if (!M.asset.empty()) M.dirty = true; }
static void MeRedo() { if (g_meRedo.empty()) return; g_meUndo.push_back(MeCapture()); MeState s = g_meRedo.back(); g_meRedo.pop_back(); MeRestore(s); if (!M.asset.empty()) M.dirty = true; }

// ---- selection
static bool MeSelected(int id) { return M.sel == id || M.selSet.count(id) > 0; }
static std::vector<int> MeSelection() {
    std::set<int> s = M.selSet; if (M.sel >= 0) s.insert(M.sel);
    std::vector<int> v; for (int id : s) if (MeNode* n = MeFind(id)) if (!n->output) v.push_back(id);
    return v;
}
static void MeSelectOnly(int id) { M.selSet.clear(); M.sel = id; if (id >= 0) M.selSet.insert(id); }
static void MeDeleteSelected() { for (int id : MeSelection()) MeDeleteNode(id); M.selSet.clear(); M.sel = -1; }

// ---- copy / paste (as text on the clipboard, so a graph can go from one material to another)
static std::string MeCopyText(const std::vector<int>& ids) {
    std::set<int> in(ids.begin(), ids.end());
    std::string o = "UEGRAPHCLIP 1\n";
    for (int id : ids) if (MeNode* n = MeFind(id)) if (!n->output) o += MeNodeLine(*n);
    for (auto& kv : M.nin) if (in.count((int)(kv.first / 64)) && in.count(kv.second.first))
        o += "i\t" + std::to_string(kv.first / 64) + "\t" + std::to_string(kv.first % 64) + "\t" + std::to_string(kv.second.first) + "\t" + std::to_string(kv.second.second) + "\n";
    return o;
}
static void MeCopy() { auto ids = MeSelection(); if (!ids.empty()) ImGui::SetClipboardText(MeCopyText(ids).c_str()); }
// pasted around a point of the graph
static bool MePasteText(const std::string& text, float gx, float gy) {
    if (text.rfind("UEGRAPHCLIP", 0) != 0) return false;
    std::istringstream ss(text); std::string line; std::vector<MeNode> nodes; std::vector<std::vector<std::string>> ins;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto t = MeTabs(line); MeNode n;
        if (MeParseNode(t, n)) nodes.push_back(n); else if (t.size() >= 5 && t[0] == "i") ins.push_back(t);
    }
    if (nodes.empty()) return false;
    float x0 = 1e9f, y0 = 1e9f; for (auto& n : nodes) { x0 = std::min(x0, n.x); y0 = std::min(y0, n.y); }
    std::map<int, int> idMap; M.selSet.clear();
    for (auto& n : nodes) {
        if (n.kind == MEK_MAT && M.slots.empty()) continue;
        MeNode k = n; k.id = M.nextId++; k.x = gx + (n.x - x0); k.y = gy + (n.y - y0); k.output = false;
        idMap[n.id] = k.id; M.nodes.push_back(k); M.selSet.insert(k.id); M.sel = k.id;
    }
    for (auto& t : ins) {
        int a = atoi(t[1].c_str()), in = atoi(t[2].c_str()), s = atoi(t[3].c_str()), o = atoi(t[4].c_str());
        if (idMap.count(a) && idMap.count(s)) M.nin[MeInKey(idMap[a], in)] = { idMap[s], o };
    }
    MeStdChanged();
    return true;
}
static void MeDuplicate() {
    auto ids = MeSelection(); if (ids.empty()) return;
    float x0 = 1e9f, y0 = 1e9f; for (int id : ids) if (MeNode* n = MeFind(id)) { x0 = std::min(x0, n->x); y0 = std::min(y0, n->y); }
    MePasteText(MeCopyText(ids), x0 + 40, y0 + 40);
}

// ---- comments (boxes around nodes, with a title)
static const unsigned g_meCommentCols[] = { 0x3C5A78u, 0x4E6A3Cu, 0x7A5A2Cu, 0x6A3C5Au, 0x5A5A5Au, 0x2C6A6Au };
static int MeAddComment(float x, float y, float w, float h, const std::string& text) {
    MeNode n; n.id = M.nextId++; n.kind = MEK_COMMENT; n.x = x; n.y = y; n.w = std::max(160.f, w); n.h = std::max(90.f, h); n.name = text; n.col = g_meCommentCols[0];
    M.nodes.insert(M.nodes.begin(), n);              // (comments are drawn behind everything)
    MeSelectOnly(n.id);
    return n.id;
}
static float MeNodeW(const MeNode& n); static float MeNodeH(const MeNode& n);
static void MeCommentAroundSelection() {
    auto ids = MeSelection();
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; int count = 0;
    for (int id : ids) if (MeNode* n = MeFind(id)) if (n->kind != MEK_COMMENT) { x0 = std::min(x0, n->x); y0 = std::min(y0, n->y); x1 = std::max(x1, n->x + MeNodeW(*n)); y1 = std::max(y1, n->y + MeNodeH(*n)); count++; }
    if (!count) return;
    MeAddComment(x0 - 30, y0 - 60, x1 - x0 + 60, y1 - y0 + 90, "Comment");
}

// ---- the node library (Unreal's Palette): everything that can be added, by category
struct MePalItem { const char* label; int kind; int op; const char* param; const char* help; };
static const MePalItem g_mePal[] = {
    { "Constant", MEK_SCA, 0, nullptr, "A number (Unreal: Constant)" },
    { "Constant Vector", MEK_VEC, 0, nullptr, "A colour or vector (Unreal: Constant3Vector / Constant4Vector)" },
    { "Scalar Parameter", MEK_SCA, 0, "Scalar", "A named number that material instances can change" },
    { "Vector Parameter", MEK_VEC, 0, "Color", "A named colour that material instances can change" },
    { "Time", MEK_OP, OP_TIME, nullptr, nullptr },
    { "Texture Sample", MEK_TEX, 0, nullptr, "A texture: from the game, a picture file or your texture library" },
    { "Texture Sample Parameter", MEK_TEX, 0, "Texture", "A named texture that material instances can swap" },
    { "Texture Coordinate", MEK_OP, OP_TEXCOORD, nullptr, nullptr },
    { "Panner", MEK_OP, OP_PANNER, nullptr, nullptr },
    { "Rotator", MEK_OP, OP_ROTATOR, nullptr, nullptr },
    { "Bump Offset", MEK_OP, OP_BUMPOFFSET, nullptr, nullptr },
    { "Add", MEK_OP, OP_ADD, nullptr, nullptr }, { "Subtract", MEK_OP, OP_SUB, nullptr, nullptr }, { "Multiply", MEK_OP, OP_MUL, nullptr, nullptr },
    { "Divide", MEK_OP, OP_DIV, nullptr, nullptr }, { "Lerp", MEK_OP, OP_LERP, nullptr, nullptr }, { "Clamp", MEK_OP, OP_CLAMP, nullptr, nullptr },
    { "Power", MEK_OP, OP_POW, nullptr, nullptr }, { "One Minus", MEK_OP, OP_ONEMINUS, nullptr, nullptr }, { "Saturate", MEK_OP, OP_SATURATE, nullptr, nullptr },
    { "Abs", MEK_OP, OP_ABS, nullptr, nullptr }, { "Frac", MEK_OP, OP_FRAC, nullptr, nullptr }, { "Floor", MEK_OP, OP_FLOOR, nullptr, nullptr },
    { "Square Root", MEK_OP, OP_SQRT, nullptr, nullptr }, { "Min", MEK_OP, OP_MIN, nullptr, nullptr }, { "Max", MEK_OP, OP_MAX, nullptr, nullptr },
    { "Sine", MEK_OP, OP_SIN, nullptr, nullptr }, { "Cosine", MEK_OP, OP_COS, nullptr, nullptr }, { "Dot", MEK_OP, OP_DOT, nullptr, nullptr },
    { "Normalize", MEK_OP, OP_NORMALIZE, nullptr, nullptr }, { "If", MEK_OP, OP_IF, nullptr, nullptr },
    { "Fresnel", MEK_OP, OP_FRESNEL, nullptr, nullptr }, { "Normal Strength", MEK_OP, OP_NORMALSTR, nullptr, nullptr },
    { "Desaturation", MEK_OP, OP_DESAT, nullptr, nullptr }, { "Component Mask", MEK_OP, OP_MASK, nullptr, nullptr }, { "Append", MEK_OP, OP_APPEND, nullptr, nullptr },
    { "Layer Blend", MEK_OP, OP_LAYERBLEND, nullptr, nullptr },
    { "Vertex Normal WS", MEK_OP, OP_VNORMAL, nullptr, nullptr }, { "Camera Vector WS", MEK_OP, OP_CAMVEC, nullptr, nullptr },
    { "Absolute World Position", MEK_OP, OP_WORLDPOS, nullptr, nullptr },
    { "Material (swap the whole material)", MEK_MAT, 0, nullptr, "Swaps the object's material for another one the game has" },
    { "Comment", MEK_COMMENT, 0, nullptr, "A box with a title, to group nodes" },
};
static const struct { const char* name; int from, to; } g_mePalCats[] = {
    { "Constants and parameters", 0, 5 }, { "Textures", 5, 7 }, { "Coordinates", 7, 11 }, { "Math", 11, 31 }, { "Utility", 31, 37 },
    { "Vectors", 37, 40 }, { "Material", 40, 41 }, { "Comments", 41, 42 } };
static const char* MePalHelp(const MePalItem& it) { return it.help ? it.help : it.kind == MEK_OP ? g_meOpHelp[it.op] : ""; }
// a node from the library, at a point of the graph
static void MeOpenPicker(int which, int node); static void MePickFile(int node);
static int MeAddFromPalette(int i, float gx, float gy) {
    const MePalItem& it = g_mePal[i];
    if (it.kind == MEK_COMMENT) return MeAddComment(gx, gy, 400, 240, "Comment");
    if (it.kind == MEK_MAT && M.slots.empty()) return -1;
    int id = MeAddNode(it.kind, gx, gy, "", it.op);
    if (MeNode* n = MeFind(id)) {
        if (it.param) {                                // a name nobody else has
            std::string base = it.param, nm = base; int k = 1;
            for (;;) { bool used = false; for (auto& o : M.nodes) if (o.id != id && o.name == nm) used = true; if (!used) break; nm = base + std::to_string(++k); }
            n->name = nm;
        }
        if (it.kind == MEK_VEC && !it.param) n->value = "1.0000,1.0000,1.0000,1.0000";
        if (it.kind == MEK_SCA) n->value = it.param ? "0.5000" : "1.0000";
    }
    MeSelectOnly(id);
    if (it.kind == MEK_MAT) MeOpenPicker(2, id);
    MeStdChanged();
    return id;
}
static bool MePalMatches(int i, const std::string& f) {
    if (f.empty()) return true;
    std::string l = W2U(Lower(U2W(g_mePal[i].label))), h = W2U(Lower(U2W(MePalHelp(g_mePal[i]))));
    return l.find(f) != std::string::npos || h.find(f) != std::string::npos;
}
// the Palette panel: search, then the library by category; drag a node onto the graph (or double-click it)
static void MeTextureLibraryItems();
static void MePalettePanel() {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##palf", ICON_SEARCH "  Search nodes", M.palFilter, sizeof M.palFilter);
    std::string f = W2U(Lower(U2W(M.palFilter)));
    ImGui::BeginChild("##pallist", ImVec2(0, 0));
    for (auto& cat : g_mePalCats) {
        bool any = false; for (int i = cat.from; i < cat.to; i++) if (MePalMatches(i, f)) any = true;
        if (!any) continue;
        if (cat.from == 40 && M.slots.empty()) continue;
        ImGui::SetNextItemOpen(true, f.empty() ? ImGuiCond_Once : ImGuiCond_Always);
        if (!ImGui::CollapsingHeader(cat.name)) continue;
        for (int i = cat.from; i < cat.to; i++) {
            if (!MePalMatches(i, f)) continue;
            ImGui::PushID(i);
            ImGui::Selectable(g_mePal[i].label, false, ImGuiSelectableFlags_AllowDoubleClick);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) M.dragNew = "pal:" + std::to_string(i) + ":centre";
            if (ImGui::IsItemHovered() && *MePalHelp(g_mePal[i])) ImGui::SetTooltip("%s\n\nDrag it onto the graph, or double-click", MePalHelp(g_mePal[i]));
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("MENODE", &i, sizeof i);
                ImGui::TextUnformatted(g_mePal[i].label);
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
    }
    // textures from the texture library, ready to drop in
    MeTextureLibraryItems();
    ImGui::EndChild();
}

// ---- live node previews: each maths node shows what it makes, worked out on a small square in the background
struct MePrevPic { ID3D11ShaderResourceView* srv = nullptr; };
static std::map<int, MePrevPic> g_mePrev;
static std::mutex g_mePrevMx; static std::map<int, std::vector<unsigned char>> g_mePrevDone; static std::atomic<bool> g_mePrevBusy{ false };
static std::string g_mePrevKey; static bool g_meNodePreviews = true;
static const int ME_PREV = 48;
static void MePreviewsUpdate() {
    if (!g_meNodePreviews) return;
    // finished pictures -> textures
    {
        std::map<int, std::vector<unsigned char>> done;
        { std::lock_guard<std::mutex> lk(g_mePrevMx); done.swap(g_mePrevDone); }
        for (auto& kv : done) {
            MePrevPic& p = g_mePrev[kv.first]; MsRel(p.srv);
            D3D11_TEXTURE2D_DESC d{}; d.Width = d.Height = ME_PREV; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd{ kv.second.data(), ME_PREV * 4, 0 }; ID3D11Texture2D* t = nullptr;
            if (g_dev && SUCCEEDED(g_dev->CreateTexture2D(&d, &sd, &t))) { g_dev->CreateShaderResourceView(t, nullptr, &p.srv); t->Release(); }
        }
    }
    if (g_mePrevBusy) return;
    static ULONGLONG last = 0; if (GetTickCount64() - last < 250) return; last = GetTickCount64();
    std::string key; for (auto& n : M.nodes) key += MeNodeLine(n) + MeParamValue(n);
    for (auto& kv : M.nin) key += std::to_string(kv.first) + ">" + std::to_string(kv.second.first) + "," + std::to_string(kv.second.second) + ";";
    if (key == g_mePrevKey) return;
    g_mePrevKey = key;
    MgProg P; std::map<int, int> idx; P.paks = PaksDir();
    std::vector<int> want;
    for (auto& n : M.nodes) if (n.kind == MEK_OP) { if (MgAddNode(P, idx, n.id) >= 0) want.push_back(n.id); }
    if (want.empty()) return;
    std::map<int, int> place; for (int id : want) place[id] = idx[id];
    g_mePrevBusy = true;
    std::thread([P, place]() mutable {
        if (g_mgSrgbLut[255] == 0) for (int i = 0; i < 256; i++) g_mgSrgbLut[i] = (float)(i / 255.0 <= 0.04045 ? i / 255.0 / 12.92 : pow((i / 255.0 + 0.055) / 1.055, 2.4));
        for (auto& n : P.n) if (n.kind == MEK_TEX) n.img = MgLoadImg(n.tex, P.paks);
        std::map<int, std::vector<unsigned char>> out;
        for (auto& kv : place) out[kv.first].assign(ME_PREV * ME_PREV * 4, 255);
        std::vector<MgV> val(P.n.size());
        for (int y = 0; y < ME_PREV; y++) for (int x = 0; x < ME_PREV; x++) {
            MgEvalPixel(P, (x + 0.5f) / ME_PREV, (y + 0.5f) / ME_PREV, val);
            for (auto& kv : place) {
                const MgV& v = val[kv.second]; unsigned char* o = &out[kv.first][(y * ME_PREV + x) * 4];
                float c[3];
                if (v.n == 1) c[0] = c[1] = c[2] = v.v[0];
                else for (int k = 0; k < 3; k++) c[k] = k < v.n ? v.v[k] : 0.f;
                for (int k = 0; k < 3; k++) o[k] = (unsigned char)lround(MgToSrgb(c[k]) * 255);
                o[3] = 255;
            }
        }
        std::lock_guard<std::mutex> lk(g_mePrevMx); for (auto& kv : out) g_mePrevDone[kv.first] = std::move(kv.second);
        g_mePrevBusy = false; g_framesLeft = 3;
    }).detach();
}
static ID3D11ShaderResourceView* MePreviewOf(int id) { auto it = g_mePrev.find(id); return it == g_mePrev.end() ? nullptr : it->second.srv; }

// ---- automatic arrangement: columns by how far a node is from the output, ordered so wires cross less
static void MeArrangeLayered() {
    MeNode* out = MeOutput(); if (!out) return;
    std::map<int, int> depth;                  // node id -> column (1 = feeds the output directly)
    std::function<void(int, int)> walk = [&](int id, int d) {
        if (d > 60) return;
        auto it = depth.find(id); if (it != depth.end() && it->second >= d) return;
        depth[id] = d;
        for (auto& kv : M.nin) if ((int)(kv.first / 64) == id) walk(kv.second.first, d + 1);
    };
    for (int i = 0; i < (int)M.pins.size(); i++) { auto it = M.links.find(i); if (it != M.links.end()) walk(it->second, 1); }
    int maxD = 1; for (auto& kv : depth) maxD = std::max(maxD, kv.second);
    std::vector<int> loose; for (auto& n : M.nodes) if (!n.output && n.kind != MEK_COMMENT && !depth.count(n.id)) loose.push_back(n.id);
    // order inside a column: by the place of what they feed (the output's pin, or the node they plug into)
    std::map<int, float> key;
    for (int i = 0; i < (int)M.pins.size(); i++) { auto it = M.links.find(i); if (it != M.links.end() && !key.count(it->second)) key[it->second] = (float)i; }
    out->x = 0; out->y = 0;
    float right = -80;
    for (int d = 1; d <= maxD; d++) {
        std::vector<MeNode*> col;
        for (auto& n : M.nodes) if (depth.count(n.id) && depth[n.id] == d) col.push_back(&n);
        for (MeNode* n : col) if (!key.count(n->id)) {
            float best = 1e9f;
            for (auto& kv : M.nin) if (kv.second.first == n->id) { int to = (int)(kv.first / 64); if (key.count(to)) best = std::min(best, key[to] + (kv.first % 64) * 0.01f); }
            key[n->id] = best;
        }
        std::stable_sort(col.begin(), col.end(), [&](MeNode* a, MeNode* b) { return key[a->id] < key[b->id]; });
        float w = 0; for (MeNode* n : col) w = std::max(w, MeNodeW(*n));
        float y = 0;
        for (size_t i = 0; i < col.size(); i++) { col[i]->x = right - w; col[i]->y = y; key[col[i]->id] = (float)i; y += MeNodeH(*col[i]) + 30; }
        right -= w + 90;
    }
    float y = -60; for (int id : loose) if (MeNode* n = MeFind(id)) { n->x = right - MeNodeW(*n); n->y = y; y += MeNodeH(*n) + 30; }
    M.fitted = false;
}
