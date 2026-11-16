// LauncherInput.dll - loaded into the game by the launcher.
// While the in-game editor is open (the launcher holds a named mutex), it swallows raw mouse
// input (WM_INPUT) at the game's windows, so mouse movement can't turn the game's camera, and it hides
// the controller from the game (the launcher uses it to fly the editing camera instead).
// Everything else passes straight through, and nothing changes while the editor is closed.
#include <windows.h>
#include <tlhelp32.h>

static HANDLE g_ev = nullptr;
struct Hooked { HWND hwnd; WNDPROC orig; };
static Hooked g_hooked[32];
static volatile LONG g_count = 0;

// The launcher owns the mutex only while the editor is open. If the launcher closes or crashes,
// Windows releases it, so the game can never be left with its mouse blocked.
static bool Editing() {
    if (!g_ev) return false;
    DWORD r = WaitForSingleObject(g_ev, 0);
    if (r == WAIT_TIMEOUT) return true;
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(g_ev);
    return false;
}

static WNDPROC OrigFor(HWND h) {
    LONG n = g_count;
    for (LONG i = 0; i < n; i++) if (g_hooked[i].hwnd == h) return g_hooked[i].orig;
    return nullptr;
}

static LRESULT CALLBACK Sub(HWND h, UINT m, WPARAM w, LPARAM l) {
    WNDPROC orig = OrigFor(h);
    if (m == WM_INPUT && Editing()) {
        // let Windows clean up the raw input data, but don't give it to the game
        return DefWindowProcW(h, m, w, l);
    }
    return orig ? CallWindowProcW(orig, h, m, w, l) : DefWindowProcW(h, m, w, l);
}

// ---- controller: the game's XInputGetState calls are routed through here (import table patch)
typedef DWORD (WINAPI *XGetState)(DWORD, void*);
struct XPad { WORD wButtons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XState { DWORD packet; XPad pad; };
static XGetState g_realX[4] = {};
template <int N> static DWORD WINAPI HookX(DWORD user, void* st) {
    DWORD r = g_realX[N] ? g_realX[N](user, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (r == ERROR_SUCCESS && st && Editing()) memset(&((XState*)st)->pad, 0, sizeof(XPad));   // sticks centred, nothing pressed
    return r;
}
static XGetState HookFor(int n) { switch (n) { case 0: return HookX<0>; case 1: return HookX<1>; case 2: return HookX<2>; default: return HookX<3>; } }
static bool IsOurHook(void* p) { for (int i = 0; i < 4; i++) if ((void*)HookFor(i) == p) return true; return false; }
static XGetState HookReal(XGetState real) {
    for (int i = 0; i < 4; i++) if (g_realX[i] == real) return HookFor(i);
    for (int i = 0; i < 4; i++) if (!g_realX[i]) { g_realX[i] = real; return HookFor(i); }
    return nullptr;
}
// ---- the mouse: while editing, the game may not lock it inside its window (the editor shrinks the game into
// its viewport, and the mouse has to reach the panels around it)
typedef BOOL (WINAPI *ClipFn)(const RECT*);
static ClipFn g_realClip = nullptr;
static BOOL WINAPI HookClip(const RECT* r) {
    if (!g_realClip) return TRUE;
    return g_realClip(Editing() ? nullptr : r);
}
static void WriteSlot(void** slot, void* value) {
    DWORD old;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) { *slot = value; VirtualProtect(slot, sizeof(void*), old, &old); }
}
static bool IsXInputDll(const char* n) { return n && _strnicmp(n, "xinput", 6) == 0; }
static bool IsGetState(BYTE* base, ULONG_PTR thunk) {
    if (IMAGE_SNAP_BY_ORDINAL(thunk)) { WORD o = (WORD)IMAGE_ORDINAL(thunk); return o == 2 || o == 100; }   // XInputGetState / XInputGetStateEx
    auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + (DWORD)thunk);
    return strcmp((char*)ibn->Name, "XInputGetState") == 0 || strcmp((char*)ibn->Name, "XInputGetStateEx") == 0;
}
static void PatchModule(HMODULE mod) {
    auto* base = (BYTE*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base; if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew); if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    // normal imports
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress) {
        for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
            if (_stricmp((char*)(base + d->Name), "user32.dll") == 0 && d->FirstThunk && d->OriginalFirstThunk) {
                auto* names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
                auto* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
                for (; names->u1.AddressOfData; names++, iat++) {
                    if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                    if (strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + (DWORD)names->u1.AddressOfData))->Name, "ClipCursor") != 0) continue;
                    void* cur = (void*)iat->u1.Function;
                    if (cur == (void*)HookClip) continue;
                    if (!g_realClip) g_realClip = (ClipFn)cur;
                    WriteSlot((void**)&iat->u1.Function, (void*)HookClip);
                }
                continue;
            }
            if (!IsXInputDll((char*)(base + d->Name)) || !d->FirstThunk) continue;
            auto* names = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
            auto* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
            for (; names->u1.AddressOfData; names++, iat++) {
                if (!d->OriginalFirstThunk || !IsGetState(base, names->u1.AddressOfData)) continue;
                void* cur = (void*)iat->u1.Function;
                if (IsOurHook(cur)) continue;
                if (XGetState h = HookReal((XGetState)cur)) WriteSlot((void**)&iat->u1.Function, (void*)h);
            }
        }
    }
    // delay-loaded imports (Unreal loads XInput this way): fill the slot with our hook straight away
    auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dd.VirtualAddress) {
        struct DelayDesc { DWORD attrs, dllName, hmod, iat, int_, boundIat, unloadIat, timestamp; };
        for (auto* d = (DelayDesc*)(base + dd.VirtualAddress); d->dllName; d++) {
            if (!(d->attrs & 1)) continue;                                   // RVA-based descriptors only
            const char* dll = (char*)(base + d->dllName);
            if (!IsXInputDll(dll) || !d->iat || !d->int_) continue;
            auto* names = (IMAGE_THUNK_DATA*)(base + d->int_);
            auto* iat = (IMAGE_THUNK_DATA*)(base + d->iat);
            for (; names->u1.AddressOfData; names++, iat++) {
                if (!IsGetState(base, names->u1.AddressOfData)) continue;
                void* cur = (void*)iat->u1.Function;
                if (IsOurHook(cur)) continue;
                HMODULE x = LoadLibraryA(dll);
                if (!x) continue;
                FARPROC real = IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) ? GetProcAddress(x, (LPCSTR)(ULONG_PTR)IMAGE_ORDINAL(names->u1.Ordinal))
                                                                         : GetProcAddress(x, (char*)((IMAGE_IMPORT_BY_NAME*)(base + (DWORD)names->u1.AddressOfData))->Name);
                if (!real) continue;
                if (XGetState h = HookReal((XGetState)real)) WriteSlot((void**)&iat->u1.Function, (void*)h);
            }
        }
    }
}
static void PatchAllModules() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{ sizeof(me) };
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&PatchAllModules, &self);
    if (Module32FirstW(snap, &me)) do {
        if (me.hModule == self) continue;
        PatchModule(me.hModule);
    } while (Module32NextW(snap, &me));
    CloseHandle(snap);
}


// ---------------------------------------------------------------- skinning for imported animated models
// The mod (Lua) can't bend a character's skin fast enough itself, so it asks this DLL to: it sends one line down a
// pipe per animated model per frame ("which model, which animation, what time, where the vertex buffers are") and
// waits for the answer, and the bent positions are written straight into the mesh component's own vertex buffers.
// The call is synchronous (the game thread waits), so the buffers can't go away while they're being written.
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
namespace skin {
struct M34 { float m[12]; };     // rotation (3x3, row-major) + translation in m[3], m[7], m[11]
static M34 FromQuat(float x, float y, float z, float w, float tx, float ty, float tz) {
    float n = std::sqrt(x * x + y * y + z * z + w * w); if (n < 1e-8f) { x = y = z = 0; w = 1; } else { x /= n; y /= n; z /= n; w /= n; }
    M34 r;
    r.m[0] = 1 - 2 * (y * y + z * z); r.m[1] = 2 * (x * y - z * w);     r.m[2] = 2 * (x * z + y * w);     r.m[3] = tx;
    r.m[4] = 2 * (x * y + z * w);     r.m[5] = 1 - 2 * (x * x + z * z); r.m[6] = 2 * (y * z - x * w);     r.m[7] = ty;
    r.m[8] = 2 * (x * z - y * w);     r.m[9] = 2 * (y * z + x * w);     r.m[10] = 1 - 2 * (x * x + y * y); r.m[11] = tz;
    return r;
}
static M34 FromRotator(float pitch, float yaw, float roll, float tx, float ty, float tz) {   // Unreal FRotator -> FQuat
    const float d = 3.14159265358979f / 360.f;
    float sp = std::sin(pitch * d), cp = std::cos(pitch * d), sy = std::sin(yaw * d), cy = std::cos(yaw * d), sr = std::sin(roll * d), cr = std::cos(roll * d);
    return FromQuat(cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy, tx, ty, tz);
}
struct Sec {
    int mat = -1;
    std::vector<float> pos, nrm, tan;          // xyz per vertex (bind pose)
    std::vector<unsigned> wOff;                // per vertex: first influence (n+1 entries)
    std::vector<unsigned short> wBone;         // palette index
    std::vector<float> wW;
};
struct Anim { float fps = 30; int frames = 0; std::vector<M34> b, k; };   // per frame: nskin bone matrices / nparts piece matrices
struct Model {
    std::string file; int nskin = 0, nparts = 0;
    std::vector<Sec> secs; std::vector<Anim> anims;
    std::vector<M34> pal;
};
static std::vector<Model*> g_models;

static const char* Num(const char* p, float& out) { char* e; out = std::strtof(p, &e); return e; }
static Model* Load(const std::string& file, std::string& err) {
    for (auto* m : g_models) if (m->file == file) return m;
    int wn = MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, nullptr, 0);
    std::wstring wf(wn, 0); MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, &wf[0], wn);
    FILE* f = _wfopen(wf.c_str(), L"rb");
    if (!f) { err = "open"; return nullptr; }
    auto* m = new Model; m->file = file;
    Sec* sec = nullptr; Anim* an = nullptr; int part = -1; bool weighted = true;
    std::vector<char> buf(1 << 16);
    auto closeVert = [&]() {      // a vertex with no weights of its own follows its piece
        if (sec && !weighted) { sec->wBone.push_back((unsigned short)0xFFFF); sec->wW.push_back((float)part); sec->wOff.push_back((unsigned)sec->wBone.size()); weighted = true; }
    };
    while (fgets(buf.data(), (int)buf.size(), f)) {
        const char* l = buf.data();
        if (l[0] == 'v' && l[1] == ' ') {
            closeVert();
            if (!sec) continue;
            float v[6]; const char* p = l + 2; for (int i = 0; i < 6; i++) p = Num(p, v[i]);
            float c[3], uv[2], t[3] = { 1, 0, 0 }; for (int i = 0; i < 3; i++) p = Num(p, c[i]); for (int i = 0; i < 2; i++) p = Num(p, uv[i]);
            const char* q = p; for (int i = 0; i < 3; i++) { float x; const char* e = Num(q, x); if (e == q) break; t[i] = x; q = e; }
            sec->pos.insert(sec->pos.end(), v, v + 3); sec->nrm.insert(sec->nrm.end(), v + 3, v + 6); sec->tan.insert(sec->tan.end(), t, t + 3);
            weighted = false;
        } else if (l[0] == 'w' && (l[1] == ' ' || l[1] == '\n' || l[1] == '\r')) {
            if (!sec || weighted) continue;
            const char* p = l + 1; size_t before = sec->wBone.size();
            for (;;) { float b, w; const char* e = Num(p, b); if (e == p) break; p = e; e = Num(p, w); if (e == p) break; p = e;
                       if (b >= 0 && b < 65000 && w > 0) { sec->wBone.push_back((unsigned short)b); sec->wW.push_back(w); } }
            if (sec->wBone.size() == before) continue;           // nothing usable: it follows its piece
            sec->wOff.push_back((unsigned)sec->wBone.size()); weighted = true;
        } else if (l[0] == 'b' && l[1] == ' ') {
            if (!an) continue;
            float v[7]; const char* p = l + 2; for (int i = 0; i < 7; i++) p = Num(p, v[i]);
            an->b.push_back(FromQuat(v[0], v[1], v[2], v[3], v[4], v[5], v[6]));
        } else if (l[0] == 'k' && l[1] == ' ') {
            if (!an) continue;
            float v[6]; const char* p = l + 2; for (int i = 0; i < 6; i++) p = Num(p, v[i]);
            an->k.push_back(FromRotator(v[3], v[4], v[5], v[0], v[1], v[2]));
        } else if (!strncmp(l, "part\t", 5)) {
            closeVert();
            part++;
            int mat = -1; const char* p = l + 5; int tabs = 0;
            for (; *p; p++) if (*p == '\t' && ++tabs == 3) { if (p[1] >= '0' && p[1] <= '9') mat = atoi(p + 1); break; }
            sec = nullptr;
            for (auto& s : m->secs) if (s.mat == mat) { sec = &s; break; }
            if (!sec) { m->secs.emplace_back(); sec = &m->secs.back(); sec->mat = mat; sec->wOff.push_back(0); }
        } else if (!strncmp(l, "anim\t", 5)) {
            closeVert();
            m->anims.emplace_back(); an = &m->anims.back();
            const char* p = strchr(l + 5, '\t');
            if (p) { float fps; const char* e = Num(p + 1, fps); if (e != p + 1 && fps > 0) an->fps = fps; }
        } else if (!strncmp(l, "skinbones\t", 10)) m->nskin = atoi(l + 10);
    }
    closeVert();
    fclose(f);
    m->nparts = part + 1;
    for (auto& a : m->anims) {
        int fb = m->nskin > 0 ? (int)(a.b.size() / m->nskin) : 0, fk = m->nparts > 0 ? (int)(a.k.size() / m->nparts) : 0;
        a.frames = m->nskin > 0 ? std::min(fb, fk) : fk;
    }
    // palette index of a piece-follower: after the skin bones
    for (auto& s : m->secs) for (size_t i = 0; i < s.wBone.size(); i++) if (s.wBone[i] == 0xFFFF) { s.wBone[i] = (unsigned short)(m->nskin + (int)s.wW[i]); s.wW[i] = 1; }
    m->pal.resize((size_t)m->nskin + m->nparts);
    g_models.push_back(m);
    return m;
}
static bool Writable(const void* p, size_t n) {
    const BYTE* a = (const BYTE*)p; const BYTE* end = a + n;
    if (!p || end < a) return false;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(a, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) return false;
        a = (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}
struct Layout { int stride, oP, oN, oT, dbl; };
template <class T> static inline void Put3(BYTE* at, float x, float y, float z) { T* d = (T*)at; d[0] = (T)x; d[1] = (T)y; d[2] = (T)z; }
static void Put(BYTE* at, int dbl, float x, float y, float z) { if (dbl) Put3<double>(at, x, y, z); else Put3<float>(at, x, y, z); }
// anim < 0: the model's own pose
static bool Pose(Model* m, int anim, double t, const Layout& L, const std::vector<std::pair<BYTE*, size_t>>& bufs) {
    if (bufs.size() != m->secs.size()) return false;
    bool rest = anim < 0 || anim >= (int)m->anims.size() || m->anims[anim].frames < 1;
    if (!rest) {
        Anim& a = m->anims[anim];
        double ft = t * a.fps; if (ft < 0) ft = 0;
        int f0 = (int)std::floor(ft); float u = (float)(ft - f0);
        f0 %= a.frames; int f1 = (f0 + 1) % a.frames;
        for (int j = 0; j < m->nskin; j++) { const M34& x = a.b[(size_t)f0 * m->nskin + j]; const M34& y = a.b[(size_t)f1 * m->nskin + j]; M34& o = m->pal[j]; for (int i = 0; i < 12; i++) o.m[i] = x.m[i] + (y.m[i] - x.m[i]) * u; }
        for (int j = 0; j < m->nparts; j++) { const M34& x = a.k[(size_t)f0 * m->nparts + j]; const M34& y = a.k[(size_t)f1 * m->nparts + j]; M34& o = m->pal[m->nskin + j]; for (int i = 0; i < 12; i++) o.m[i] = x.m[i] + (y.m[i] - x.m[i]) * u; }
    }
    const size_t np = m->pal.size();
    for (size_t si = 0; si < m->secs.size(); si++) {
        Sec& s = m->secs[si]; size_t n = s.pos.size() / 3, cnt = bufs[si].second; BYTE* base = bufs[si].first;
        if (cnt != n && cnt != 2 * n) return false;                       // (two-sided sections hold every vertex twice)
        if (!Writable(base, cnt * (size_t)L.stride)) return false;
        for (size_t v = 0; v < n; v++) {
            const float* P = &s.pos[v * 3]; const float* N = &s.nrm[v * 3]; const float* T = &s.tan[v * 3];
            float p[3], nn[3], tt[3];
            if (rest) { for (int i = 0; i < 3; i++) { p[i] = P[i]; nn[i] = N[i]; tt[i] = T[i]; } }
            else {
                float a[12] = {}; float tw = 0;
                for (unsigned i = s.wOff[v]; i < s.wOff[v + 1]; i++) { unsigned b = s.wBone[i]; if (b >= np) continue; const float* mm = m->pal[b].m; float w = s.wW[i]; tw += w; for (int k = 0; k < 12; k++) a[k] += mm[k] * w; }
                if (tw < 1e-6f) { for (int i = 0; i < 3; i++) { p[i] = P[i]; nn[i] = N[i]; tt[i] = T[i]; } }
                else {
                    for (int r = 0; r < 3; r++) {
                        const float* row = a + r * 4;
                        p[r] = (row[0] * P[0] + row[1] * P[1] + row[2] * P[2] + row[3]) / tw;
                        nn[r] = row[0] * N[0] + row[1] * N[1] + row[2] * N[2];
                        tt[r] = row[0] * T[0] + row[1] * T[1] + row[2] * T[2];
                    }
                    float ln = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]); if (ln > 1e-8f) { nn[0] /= ln; nn[1] /= ln; nn[2] /= ln; }
                    float lt = std::sqrt(tt[0] * tt[0] + tt[1] * tt[1] + tt[2] * tt[2]); if (lt > 1e-8f) { tt[0] /= lt; tt[1] /= lt; tt[2] /= lt; }
                }
            }
            BYTE* e = base + v * (size_t)L.stride;
            Put(e + L.oP, L.dbl, p[0], p[1], p[2]);
            if (L.oN >= 0) Put(e + L.oN, L.dbl, nn[0], nn[1], nn[2]);
            if (L.oT >= 0) Put(e + L.oT, L.dbl, tt[0], tt[1], tt[2]);
            if (cnt == 2 * n) {
                BYTE* e2 = e + n * (size_t)L.stride;
                Put(e2 + L.oP, L.dbl, p[0], p[1], p[2]);
                if (L.oN >= 0) Put(e2 + L.oN, L.dbl, -nn[0], -nn[1], -nn[2]);
                if (L.oT >= 0) Put(e2 + L.oT, L.dbl, -tt[0], -tt[1], -tt[2]);
            }
        }
    }
    return true;
}
static std::string Handle(const std::string& line) {
    if (line.size() > 2 && line[0] == 'L' && line[1] == '\t') {
        std::string err; Model* m = Load(line.substr(2), err);
        if (!m) return "err " + err;
        int id = 0; for (size_t i = 0; i < g_models.size(); i++) if (g_models[i] == m) id = (int)i;
        std::string r = "ok " + std::to_string(id) + " " + std::to_string(m->anims.size()) + " " + std::to_string(m->secs.size());
        for (auto& s : m->secs) r += " " + std::to_string(s.pos.size() / 3);
        return r;
    }
    if (line.size() > 2 && line[0] == 'S' && line[1] == ' ') {
        // S id anim time stride oP oN oT dbl nsec {addr count}...
        char* p = (char*)line.c_str() + 2;
        long id = strtol(p, &p, 10); long anim = strtol(p, &p, 10); double t = strtod(p, &p);
        Layout L; L.stride = (int)strtol(p, &p, 10); L.oP = (int)strtol(p, &p, 10); L.oN = (int)strtol(p, &p, 10); L.oT = (int)strtol(p, &p, 10); L.dbl = (int)strtol(p, &p, 10);
        long ns = strtol(p, &p, 10);
        if (id < 0 || id >= (long)g_models.size() || ns < 0 || ns > 4096 || L.stride < 12 || L.stride > 1024 || L.oP < 0) return "err args";
        int w = L.dbl ? 24 : 12;
        if (L.oP + w > L.stride || L.oN + w > L.stride || L.oT + w > L.stride) return "err layout";
        std::vector<std::pair<BYTE*, size_t>> bufs;
        for (long i = 0; i < ns; i++) { unsigned long long a = strtoull(p, &p, 10); unsigned long long c = strtoull(p, &p, 10); bufs.push_back({ (BYTE*)(uintptr_t)a, (size_t)c }); }
        return Pose(g_models[id], (int)anim, t, L, bufs) ? "ok" : "err pose";
    }
    if (line == "ping") return "ok pong";
    return "err what";
}
static DWORD WINAPI Serve(LPVOID) {
    for (;;) {
        HANDLE cmd = CreateNamedPipeW(L"\\\\.\\pipe\\UE4SSLauncherSkinCmd", PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16, 1 << 16, 0, nullptr);
        HANDLE rep = cmd != INVALID_HANDLE_VALUE ? CreateNamedPipeW(L"\\\\.\\pipe\\UE4SSLauncherSkinRep", PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16, 1 << 16, 0, nullptr) : INVALID_HANDLE_VALUE;
        if (cmd == INVALID_HANDLE_VALUE || rep == INVALID_HANDLE_VALUE) {      // another game has them: try again later
            if (cmd != INVALID_HANDLE_VALUE) CloseHandle(cmd);
            Sleep(3000); continue;
        }
        bool ok = (ConnectNamedPipe(cmd, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) && (ConnectNamedPipe(rep, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED);
        ULONG c1 = 0, c2 = 0;
        if (ok) ok = GetNamedPipeClientProcessId(cmd, &c1) && GetNamedPipeClientProcessId(rep, &c2) && c1 == GetCurrentProcessId() && c2 == c1;   // only this game's own mod
        std::string acc; char buf[4096];
        while (ok) {
            DWORD got = 0;
            if (!ReadFile(cmd, buf, sizeof buf, &got, nullptr) || !got) break;
            acc.append(buf, got);
            size_t nl;
            while (ok && (nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl); acc.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string r = Handle(line) + "\n";
                DWORD wr = 0; if (!WriteFile(rep, r.data(), (DWORD)r.size(), &wr, nullptr)) ok = false;
            }
        }
        DisconnectNamedPipe(cmd); DisconnectNamedPipe(rep); CloseHandle(cmd); CloseHandle(rep);
        Sleep(200);
    }
    return 0;
}
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    if (OrigFor(h) || g_count >= 32) return TRUE;
    WNDPROC orig = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (!orig || orig == Sub) return TRUE;
    // record it before swapping, so Sub can always find the original
    g_hooked[g_count].hwnd = h; g_hooked[g_count].orig = orig;
    InterlockedIncrement(&g_count);
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)Sub);
    return TRUE;
}

static DWORD WINAPI Watch(LPVOID) {
    wchar_t name[80];
    wsprintfW(name, L"Local\\UE4SSLauncherEditing_%lu", GetCurrentProcessId());
    for (int tick = 0;; tick++) {
        if (!g_ev) g_ev = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
        EnumWindows(EnumProc, 0);          // pick up the game window (and any new ones, e.g. after a fullscreen switch)
        if (tick % 20 == 0) PatchAllModules();   // controller hook (again every ~6 s for modules loaded later)
        Sleep(300);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        HMODULE self;   // stay loaded for the life of the game (our window procedure must never disappear)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&Sub, &self);
        HANDLE t = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        HANDLE t2 = CreateThread(nullptr, 0, skin::Serve, nullptr, 0, nullptr);
        if (t2) CloseHandle(t2);
    }
    return TRUE;
}
