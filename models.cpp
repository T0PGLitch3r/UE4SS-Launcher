// Model import (psk/psa, glTF/GLB, OBJ -> .uemodel) and the FModel exporter download.
#include "core.h"
#include "models.h"
#include "third_party/meshopt/meshoptimizer.h"
#include "updater.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <array>
#include <set>
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include <acl/core/compressed_tracks.h>
#include <acl/decompression/decompress.h>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#include <thread>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <deque>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <tuple>

#define CGLTF_IMPLEMENTATION
#include "third_party/cgltf.h"
#include "third_party/ufbx/ufbx.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"
static const int s_pngFast = (stbi_write_png_compression_level = 3, 0);   // much faster than stb's default (8), files only a little bigger

// ---------------------------------------------------------------- small helpers
static std::string LowerA(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static bool ReadBytes(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear(); char b[1 << 16]; DWORD n;
    while (ReadFile(h, b, sizeof b, &n, nullptr) && n) out.append(b, n);
    CloseHandle(h); return true;
}
static bool WriteBytes(const std::wstring& p, const std::string& d) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = WriteFile(h, d.data(), (DWORD)d.size(), &w, nullptr) && w == d.size(); CloseHandle(h); return ok;
}
static std::vector<std::wstring> ListFiles(const std::wstring& dir, const std::wstring& pat, bool dirs = false) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\" + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (fd.cFileName[0] == L'.' || d != dirs) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h); return out;
}
static void Walk(const std::wstring& dir, int depth, const std::function<void(const std::wstring&)>& f, int& budget) {
    if (budget <= 0) return;
    for (auto& n : ListFiles(dir, L"*")) { if (--budget <= 0) return; f(dir + L"\\" + n); }
    if (depth <= 0) return;
    for (auto& d : ListFiles(dir, L"*", true)) Walk(dir + L"\\" + d, depth - 1, f, budget);
}
static std::wstring ExtLower(const std::wstring& p) { size_t d = p.find_last_of(L'.'); return d == std::wstring::npos ? L"" : Lower(p.substr(d)); }

// ---------------------------------------------------------------- maths (Unreal conventions)
struct V3 { double x = 0, y = 0, z = 0; };
struct Q { double x = 0, y = 0, z = 0, w = 1; };
struct Xf { Q r; V3 t; };
static V3 add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 mul(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
static V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static double len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
static V3 norm(V3 a) { double l = len(a); return l > 1e-12 ? mul(a, 1 / l) : V3{ 0, 0, 1 }; }
static Q qmul(Q a, Q b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static Q qconj(Q a) { return { -a.x, -a.y, -a.z, a.w }; }
static Q qnorm(Q a) { double l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w); return l > 1e-12 ? Q{ a.x / l, a.y / l, a.z / l, a.w / l } : Q{}; }
static V3 qrot(Q q, V3 v) { V3 u{ q.x, q.y, q.z }; V3 t = mul(cross(u, v), 2); return add(add(v, mul(t, q.w)), cross(u, t)); }   // FQuat::RotateVector
static Xf compose(const Xf& parent, const Xf& local) { return { qnorm(qmul(parent.r, local.r)), add(parent.t, qrot(parent.r, local.t)) }; }
static Xf inverse(const Xf& a) { Q ri = qconj(a.r); return { ri, mul(qrot(ri, a.t), -1) }; }
static Q qslerp(Q a, Q b, double t) {
    double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b = { -b.x, -b.y, -b.z, -b.w }; d = -d; }
    if (d > 0.9995) return qnorm({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
    double th = std::acos(d), s = std::sin(th), wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return { a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb };
}
// FQuat::Rotator (degrees)
static double NormAxis(double a) { a = std::fmod(a, 360.0); if (a < 0) a += 360; if (a > 180) a -= 360; return a; }
static void Rotator(Q q, double& pitch, double& yaw, double& roll) {
    const double R2D = 180.0 / 3.14159265358979323846;
    double sing = q.z * q.x - q.w * q.y;
    double yy = 2 * (q.w * q.z + q.x * q.y), yx = 1 - 2 * (q.y * q.y + q.z * q.z);
    yaw = std::atan2(yy, yx) * R2D;
    if (sing < -0.4999995) { pitch = -90; roll = NormAxis(-yaw - 2 * std::atan2(q.x, q.w) * R2D); }
    else if (sing > 0.4999995) { pitch = 90; roll = NormAxis(yaw - 2 * std::atan2(q.x, q.w) * R2D); }
    else { pitch = std::asin(2 * sing) * R2D; roll = std::atan2(-2 * (q.w * q.x + q.y * q.z), 1 - 2 * (q.x * q.x + q.y * q.y)) * R2D; }
}
// column-major 4x4 (glTF)
struct M4 { double m[16]; };
static M4 Ident() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
static M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) { double s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k]; r.m[c * 4 + rr] = s; }
    return r;
}
static M4 FromTRS(const double t[3], const double q[4], const double s[3]) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    M4 r = Ident();
    r.m[0] = (1 - 2 * (y * y + z * z)) * s[0]; r.m[1] = (2 * (x * y + z * w)) * s[0]; r.m[2] = (2 * (x * z - y * w)) * s[0];
    r.m[4] = (2 * (x * y - z * w)) * s[1]; r.m[5] = (1 - 2 * (x * x + z * z)) * s[1]; r.m[6] = (2 * (y * z + x * w)) * s[1];
    r.m[8] = (2 * (x * z + y * w)) * s[2]; r.m[9] = (2 * (y * z - x * w)) * s[2]; r.m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    r.m[12] = t[0]; r.m[13] = t[1]; r.m[14] = t[2];
    return r;
}
static V3 XformPt(const M4& a, V3 p) { return { a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12], a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13], a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14] }; }
static V3 XformDir(const M4& a, V3 p) { return { a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z, a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z, a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z }; }
// glTF (metres, Y up) <-> Unreal (cm, Z up): swap Y and Z, x100 (the same mapping FModel uses when it exports)
static V3 GlPos(V3 v) { return { v.x * 100, v.z * 100, v.y * 100 }; }
static V3 GlDir(V3 v) { return { v.x, v.z, v.y }; }
static Xf GlMatrix(const M4& j) {
    // linear part S*L*S, translation 100*S*t; then rotation from the (scale-normalised) axes
    double L[3][3];   // L[row][col]
    int sw[3] = { 0, 2, 1 };
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) L[r][c] = j.m[sw[c] * 4 + sw[r]];
    V3 cx{ L[0][0], L[1][0], L[2][0] }, cy{ L[0][1], L[1][1], L[2][1] }, cz{ L[0][2], L[1][2], L[2][2] };
    cx = norm(cx); cy = norm(cy); cz = norm(cz);
    double m00 = cx.x, m10 = cx.y, m20 = cx.z, m01 = cy.x, m11 = cy.y, m21 = cy.z, m02 = cz.x, m12 = cz.y, m22 = cz.z;
    Q q; double tr = m00 + m11 + m22;
    if (tr > 0) { double s = std::sqrt(tr + 1) * 2; q = { (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4 }; }
    else if (m00 > m11 && m00 > m22) { double s = std::sqrt(1 + m00 - m11 - m22) * 2; q = { s / 4, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s }; }
    else if (m11 > m22) { double s = std::sqrt(1 + m11 - m00 - m22) * 2; q = { (m01 + m10) / s, s / 4, (m12 + m21) / s, (m02 - m20) / s }; }
    else { double s = std::sqrt(1 + m22 - m00 - m11) * 2; q = { (m02 + m20) / s, (m12 + m21) / s, s / 4, (m10 - m01) / s }; }
    return { qnorm(q), { j.m[12] * 100, j.m[14] * 100, j.m[13] * 100 } };
}

// ---------------------------------------------------------------- textures
struct Image { int w = 0, h = 0; std::vector<unsigned char> px; };
static std::map<std::wstring, Image> g_imgCache;
static const Image* LoadImageFile(const std::wstring& p) {
    auto it = g_imgCache.find(Lower(p)); if (it != g_imgCache.end()) return it->second.w ? &it->second : nullptr;
    Image im; std::string bytes;
    if (ReadBytes(p, bytes)) {
        int w, h, n; unsigned char* d = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
        if (d) { im.w = w; im.h = h; im.px.assign(d, d + (size_t)w * h * 4); stbi_image_free(d); }
    }
    auto& slot = g_imgCache[Lower(p)] = std::move(im);
    return slot.w ? &slot : nullptr;
}
static const Image* LoadImageMem(const void* data, size_t size, const std::wstring& key) {
    auto it = g_imgCache.find(key); if (it != g_imgCache.end()) return it->second.w ? &it->second : nullptr;
    Image im; int w, h, n;
    unsigned char* d = stbi_load_from_memory((const unsigned char*)data, (int)size, &w, &h, &n, 4);
    if (d) { im.w = w; im.h = h; im.px.assign(d, d + (size_t)w * h * 4); stbi_image_free(d); }
    auto& slot = g_imgCache[key] = std::move(im);
    return slot.w ? &slot : nullptr;
}
struct Mat { std::string name; const Image* tex = nullptr; double factor[4] = { 1, 1, 1, 1 }; std::wstring texPath; const Image* nrm = nullptr;
             const Image* mk[3] = {}; int mkCh[3] = {};
             // how the original material draws: 0 opaque, 1 masked (cut-out), 2 translucent, 3 additive, 4 modulate; -1 unknown
             int blend = -1, twoSided = -1; float clip = 0.3333f, opacity = 1; const Image* opac = nullptr;
             std::string orig;
             // glow (self-illumination): its picture (or the colour picture, times a mask channel) and colour x strength
             const Image* emi = nullptr; const Image* emiMask = nullptr; int emiMaskCh = 2; double emiCol[3] = { 0, 0, 0 }; bool glow = false; };   // the game's own material (object path), used as-is when the model goes back into that game   // surface masks: 0 ambient occlusion, 1 roughness, 2 metal (image + channel)
static double SrgbToLin(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
// colour at a UV: averages a few texels around the point (texture detail ends up in vertex colours)
static void Sample(const Mat& m, double u, double v, unsigned char out[3]) {
    double rgb[3] = { 0.78, 0.78, 0.78 };
    if (m.tex) {
        const Image& im = *m.tex;
        u -= std::floor(u); v -= std::floor(v);
        int cx = (int)(u * im.w), cy = (int)(v * im.h), r = std::max(1, std::min(im.w, im.h) / 256);
        double acc[3] = { 0, 0, 0 }; int n = 0;
        for (int dy = -r; dy <= r; dy += r) for (int dx = -r; dx <= r; dx += r) {
            int x = ((cx + dx) % im.w + im.w) % im.w, y = ((cy + dy) % im.h + im.h) % im.h;
            const unsigned char* p = &im.px[((size_t)y * im.w + x) * 4];
            for (int k = 0; k < 3; k++) acc[k] += p[k] / 255.0;
            n++;
        }
        for (int k = 0; k < 3; k++) rgb[k] = acc[k] / n;
    }
    for (int k = 0; k < 3; k++) {
        double c = SrgbToLin(std::min(1.0, std::max(0.0, rgb[k] * m.factor[k])));   // vertex colours are read as linear
        out[k] = (unsigned char)std::lround(c * 255);
    }
}

// ---------------------------------------------------------------- the model being built
struct Bone { std::string name; int parent = -1; Xf bindGlobal; Xf bindLocal; };
struct Anim { std::string name; double fps = 30; std::vector<std::vector<Xf>> k; std::string path; };   // path: the game's own AnimSequence (game exports)   // k[frame][bone]: moves bind-pose points to that frame
struct Src {
    std::vector<V3> pos, nrm; std::vector<double> u, v; std::vector<int> vMat, vBone;
    std::vector<uint32_t> idx;
    std::vector<Mat> mats;
    std::vector<Bone> bones;
    std::vector<Anim> anims;
    std::vector<std::wstring> psaFiles;
    bool skinned = false;
    std::string skeleton;     // game exports: the Skeleton asset's package (its animations are found by it)
    std::vector<std::string> fx;   // effects: "kind\tasset\tx,y,z\tpitch,yaw,roll\tsx,sy,sz" relative to the model
    std::vector<std::vector<std::pair<int, float>>> vw;   // full skin weights per vertex (game exports)
    std::vector<std::string> real; // game exports: the original pieces "kind\tobject path\tx,y,z\tpitch,yaw,roll\tsx,sy,sz" relative to the model
    std::vector<std::vector<V3>> hulls;   // collision shapes (FBX: Unreal's UCX_ / UBX_ / USP_ / UCP_ meshes), each a convex hull's points
};

// ---------------------------------------------------------------- ActorX (.psk / .psa, as FModel writes them)
struct Chunk { std::string id; int type, size, count; size_t data; };
static std::vector<Chunk> Chunks(const std::string& b) {
    std::vector<Chunk> out; size_t p = 0;
    while (p + 32 <= b.size()) {
        Chunk c; c.id.assign(b.data() + p, strnlen(b.data() + p, 20));
        memcpy(&c.type, b.data() + p + 20, 4); memcpy(&c.size, b.data() + p + 24, 4); memcpy(&c.count, b.data() + p + 28, 4);
        c.data = p + 32;
        size_t bytes = (size_t)std::max(0, c.size) * (size_t)std::max(0, c.count);
        if (c.data + bytes > b.size()) break;
        out.push_back(c); p = c.data + bytes;
    }
    return out;
}
template <class T> static T Rd(const std::string& b, size_t at) { T v; memcpy(&v, b.data() + at, sizeof(T)); return v; }
// FModel mirrors Y (and flips the root's W) for the importers; undo that to get Unreal's own values back
static Xf ReadJoint(const std::string& b, size_t at, bool root) {
    Q q{ Rd<float>(b, at), -Rd<float>(b, at + 4), Rd<float>(b, at + 8), Rd<float>(b, at + 12) };
    if (root) q.w = -q.w;
    V3 t{ Rd<float>(b, at + 16), -Rd<float>(b, at + 20), Rd<float>(b, at + 24) };
    return { qnorm(q), t };
}
static std::string Name64(const std::string& b, size_t at) { return std::string(b.data() + at, strnlen(b.data() + at, 64)); }
static std::vector<std::string> PsaBoneNames(const std::wstring& f) {
    std::string b; std::vector<std::string> out;
    if (!ReadBytes(f, b)) return out;
    for (auto& c : Chunks(b)) if (c.id == "BONENAMES") { for (int i = 0; i < c.count; i++) out.push_back(LowerA(Name64(b, c.data + (size_t)i * c.size))); break; }
    return out;
}
static bool LoadPsa(const std::wstring& f, Src& s, std::string& msg) {
    std::string b; if (!ReadBytes(f, b)) return false;
    auto ch = Chunks(b);
    std::vector<std::string> names; struct Info { std::string name; int bones, first, frames; float rate; }; std::vector<Info> infos;
    const Chunk* keys = nullptr;
    for (auto& c : ch) {
        if (c.id == "BONENAMES") for (int i = 0; i < c.count; i++) names.push_back(LowerA(Name64(b, c.data + (size_t)i * c.size)));
        else if (c.id == "ANIMINFO") for (int i = 0; i < c.count; i++) {
            size_t a = c.data + (size_t)i * c.size;
            infos.push_back({ Name64(b, a), Rd<int>(b, a + 128), Rd<int>(b, a + 160), Rd<int>(b, a + 164), Rd<float>(b, a + 152) });   // TotalBones, FirstRawFrame, NumRawFrames, AnimRate
        } else if (c.id == "ANIMKEYS") keys = &c;
    }
    if (!keys || names.empty() || infos.empty()) return false;
    // which model bone each animation bone drives
    std::vector<int> map(names.size(), -1); int matched = 0;
    for (size_t i = 0; i < names.size(); i++) for (size_t j = 0; j < s.bones.size(); j++) if (LowerA(s.bones[j].name) == names[i]) { map[i] = (int)j; matched++; break; }
    if (matched < (int)std::min(names.size(), s.bones.size()) / 2) { msg += "skipped " + W2U(NameOf(f)) + " (different skeleton); "; return false; }
    size_t keyIndex = 0;
    for (auto& in : infos) {
        int nb = (int)names.size(); if (in.bones > 0 && in.bones != nb) nb = in.bones;
        int frames = std::min(in.frames, 900);
        Anim an; an.name = in.name.empty() ? W2U(StemOf(f)) : in.name; an.fps = in.rate > 0 ? in.rate : 30;
        for (int fr = 0; fr < frames; fr++) {
            std::vector<Xf> local(s.bones.size());
            for (size_t j = 0; j < s.bones.size(); j++) local[j] = s.bones[j].bindLocal;
            for (int bi = 0; bi < nb; bi++) {
                size_t at = keys->data + (keyIndex + (size_t)fr * nb + bi) * keys->size;
                if (at + 28 > b.size() || bi >= (int)map.size() || map[bi] < 0) continue;
                // key: position (3 floats) then rotation (4 floats)
                Q q{ Rd<float>(b, at + 12), -Rd<float>(b, at + 16), Rd<float>(b, at + 20), Rd<float>(b, at + 24) };
                if (bi == 0) q.w = -q.w;
                local[map[bi]] = { qnorm(q), { Rd<float>(b, at), -Rd<float>(b, at + 4), Rd<float>(b, at + 8) } };
            }
            std::vector<Xf> global(s.bones.size()); std::vector<Xf> k(s.bones.size());
            for (size_t j = 0; j < s.bones.size(); j++) {
                int p = s.bones[j].parent;
                global[j] = p >= 0 && p < (int)j ? compose(global[p], local[j]) : local[j];
                k[j] = compose(global[j], inverse(s.bones[j].bindGlobal));
            }
            an.k.push_back(std::move(k));
        }
        keyIndex += (size_t)in.frames * nb;
        if (!an.k.empty()) s.anims.push_back(std::move(an));
    }
    return true;
}
// FModel writes each material as <name>.json next to its package; look for its base colour texture there
static std::wstring ExportRoot(const std::wstring& file) {
    std::wstring d = DirOf(file);
    for (int i = 0; i < 12 && !d.empty(); i++) { if (Lower(NameOf(d)) == L"exports") return d; std::wstring p = DirOf(d); if (p == d) break; d = p; }
    return DirOf(DirOf(file));
}
static std::wstring FindTextureFor(const std::wstring& modelFile, const std::string& matName) {
    std::wstring root = ExportRoot(modelFile);
    std::string core = LowerA(matName);
    for (auto pre : { "mi_", "m_", "mat_", "mt_" }) if (core.rfind(pre, 0) == 0) { core = core.substr(strlen(pre)); break; }
    std::vector<std::wstring> images, jsons; int budget = 6000;
    Walk(DirOf(DirOf(modelFile)), 4, [&](const std::wstring& p) {
        std::wstring e = ExtLower(p);
        if (e == L".png" || e == L".tga" || e == L".jpg" || e == L".jpeg") images.push_back(p);
        else if (e == L".json" && Lower(StemOf(p)) == Lower(U2W(matName))) jsons.push_back(p);
    }, budget);
    // 1. the material's own file says which texture is the colour one
    for (auto& j : jsons) {
        std::string t; if (!ReadBytes(j, t)) continue;
        std::string lt = LowerA(t);
        for (auto key : { "\"diffuse\"", "\"basecolor\"", "\"base color\"", "\"basecolortexture\"", "\"albedo\"", "\"diffuse_texture\"", "\"color\"" }) {
            size_t k = lt.find(key); if (k == std::string::npos) continue;
            size_t q1 = t.find('"', t.find(':', k + strlen(key)) + 1), q2 = q1 == std::string::npos ? q1 : t.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) continue;
            std::string ref = t.substr(q1 + 1, q2 - q1 - 1);
            size_t dot = ref.find_last_of('.'); if (dot != std::string::npos && ref.find('/', dot) == std::string::npos) ref = ref.substr(0, dot);
            while (!ref.empty() && ref[0] == '/') ref = ref.substr(1);
            std::wstring rel = U2W(ref); for (auto& c : rel) if (c == L'/') c = L'\\';
            for (auto ext : { L".png", L".tga" }) if (FileExists(root + L"\\" + rel + ext)) return root + L"\\" + rel + ext;
            std::wstring stem = Lower(NameOf(rel));
            for (auto& im : images) if (Lower(StemOf(im)) == stem) return im;
        }
    }
    // 2. names: T_<material>_D / _BaseColor / _Albedo ...
    auto isColour = [](const std::string& n) {
        for (auto bad : { "_n", "normal", "_orm", "_mra", "rough", "metal", "_m.", "mask", "_ao", "_e.", "emiss", "height", "_h.", "_r.", "_s.", "spec", "opacity", "_alpha" })
            if ((n + ".").find(bad) != std::string::npos) return false;
        return true;
    };
    int best = -1; std::wstring bestP;
    for (auto& im : images) {
        std::string n = LowerA(W2U(StemOf(im)));
        int score = 0;
        if (!core.empty() && n.find(core) != std::string::npos) score += 10;
        for (auto good : { "_d", "_bc", "basecolor", "base_color", "diffuse", "albedo", "_color", "_col", "_c", "_diff", "_alb" })
            if (n.size() >= strlen(good) && n.find(good) != std::string::npos) { score += 5; break; }
        if (!isColour(n)) score -= 20;
        if (Lower(DirOf(im)) == Lower(DirOf(modelFile))) score += 1;
        if (score > best) { best = score; bestP = im; }
    }
    return best >= 5 ? bestP : L"";
}
static bool LoadPsk(const std::wstring& f, Src& s, std::string& err, const std::wstring& texOverride) {
    std::string b; if (!ReadBytes(f, b)) { err = "Couldn't read the file."; return false; }
    auto ch = Chunks(b);
    if (ch.empty() || ch[0].id != "ACTRHEAD") { err = "That isn't a .psk model file."; return false; }
    std::vector<V3> pts, ptNrm; struct W { uint32_t pt; float u, v; int mat; }; std::vector<W> wedges;
    std::vector<uint32_t> faces; std::vector<int> faceMat;
    std::vector<std::vector<std::pair<int, float>>> weights;
    for (auto& c : ch) {
        if (c.id == "PNTS0000") for (int i = 0; i < c.count; i++) { size_t a = c.data + (size_t)i * c.size; pts.push_back({ Rd<float>(b, a), -Rd<float>(b, a + 4), Rd<float>(b, a + 8) }); }
        else if (c.id == "VTXW0000" || c.id == "VTXW3200") for (int i = 0; i < c.count; i++) {
            size_t a = c.data + (size_t)i * c.size;
            uint32_t pi = Rd<uint32_t>(b, a); if (c.id == "VTXW0000" && pts.size() <= 65536) pi &= 0xFFFF;
            wedges.push_back({ pi, Rd<float>(b, a + 4), Rd<float>(b, a + 8), (int)(unsigned char)b[a + 12] });
        } else if (c.id == "FACE0000") for (int i = 0; i < c.count; i++) {
            size_t a = c.data + (size_t)i * c.size;
            uint16_t w0 = Rd<uint16_t>(b, a), w1 = Rd<uint16_t>(b, a + 2), w2 = Rd<uint16_t>(b, a + 4);
            faces.push_back(w1); faces.push_back(w0); faces.push_back(w2); faceMat.push_back((unsigned char)b[a + 6]);   // undo the mirror's swap
        } else if (c.id == "FACE3200") for (int i = 0; i < c.count; i++) {
            size_t a = c.data + (size_t)i * c.size;
            faces.push_back(Rd<uint32_t>(b, a + 4)); faces.push_back(Rd<uint32_t>(b, a)); faces.push_back(Rd<uint32_t>(b, a + 8)); faceMat.push_back((unsigned char)b[a + 12]);
        } else if (c.id == "MATT0000") for (int i = 0; i < c.count; i++) { Mat m; m.name = Name64(b, c.data + (size_t)i * c.size); s.mats.push_back(m); }
        else if (c.id == "REFSKELT" || c.id == "REFSKEL0") for (int i = 0; i < c.count; i++) {
            size_t a = c.data + (size_t)i * c.size;
            Bone bn; bn.name = Name64(b, a); bn.parent = Rd<int>(b, a + 72);
            if (bn.parent == i || i == 0) bn.parent = -1;
            bn.bindLocal = ReadJoint(b, a + 76, i == 0);
            s.bones.push_back(bn);
        } else if (c.id == "RAWWEIGHTS" || c.id == "RAWW0000") {
            weights.assign(pts.size(), {});
            for (int i = 0; i < c.count; i++) {
                size_t a = c.data + (size_t)i * c.size;
                float w = Rd<float>(b, a); int p = Rd<int>(b, a + 4), bo = Rd<int>(b, a + 8);
                if (p >= 0 && p < (int)weights.size()) weights[p].push_back({ bo, w });
            }
        } else if (c.id == "VTXNORMS") for (int i = 0; i < c.count; i++) { size_t a = c.data + (size_t)i * c.size; ptNrm.push_back({ Rd<float>(b, a), -Rd<float>(b, a + 4), Rd<float>(b, a + 8) }); }
    }
    if (pts.empty() || wedges.empty() || faces.empty()) { err = "That .psk has no mesh in it."; return false; }
    for (size_t i = 0; i < s.bones.size(); i++) {
        int p = s.bones[i].parent;
        s.bones[i].bindGlobal = p >= 0 && p < (int)i ? compose(s.bones[p].bindGlobal, s.bones[i].bindLocal) : s.bones[i].bindLocal;
    }
    s.skinned = !weights.empty() && s.bones.size() > 1;
    // one vertex per wedge
    for (auto& w : wedges) {
        uint32_t p = w.pt < pts.size() ? w.pt : 0;
        s.pos.push_back(pts[p]); s.nrm.push_back(p < ptNrm.size() ? ptNrm[p] : V3{}); s.u.push_back(w.u); s.v.push_back(w.v); s.vMat.push_back(w.mat);
        int bone = 0; float bw = -1;
        if (s.skinned) for (auto& pw : weights[p]) if (pw.second > bw) { bw = pw.second; bone = pw.first; }
        s.vBone.push_back(s.skinned ? bone : 0);
    }
    for (size_t i = 0; i < faces.size(); i++) s.idx.push_back(faces[i] < wedges.size() ? faces[i] : 0);
    for (size_t t = 0; t < faceMat.size(); t++) for (int k = 0; k < 3; k++) s.vMat[s.idx[t * 3 + k]] = faceMat[t];
    if (s.mats.empty()) s.mats.push_back(Mat{ "Material" });
    for (auto& m : s.mats) {
        m.texPath = !texOverride.empty() ? texOverride : FindTextureFor(f, m.name);
        if (!m.texPath.empty()) m.tex = LoadImageFile(m.texPath);
    }
    if (!ptNrm.empty() && ptNrm.size() < pts.size()) s.nrm.assign(s.pos.size(), V3{});
    if (!s.skinned) { s.bones.clear(); s.bones.push_back(Bone{ "Root" }); }
    return true;
}

// ---------------------------------------------------------------- glTF / GLB
static double Eval1(const cgltf_animation_sampler* sm, double t, int comps, double* out) {
    const cgltf_accessor* in = sm->input; const cgltf_accessor* o = sm->output;
    size_t n = in->count; if (!n) return 0;
    bool cubic = sm->interpolation == cgltf_interpolation_type_cubic_spline;
    auto val = [&](size_t k, double* v) { float tmp[4] = {}; cgltf_accessor_read_float(o, cubic ? k * 3 + 1 : k, tmp, comps); for (int i = 0; i < comps; i++) v[i] = tmp[i]; };
    float t0, t1;
    cgltf_accessor_read_float(in, 0, &t0, 1);
    if (t <= t0 || n == 1) { val(0, out); return 0; }
    cgltf_accessor_read_float(in, n - 1, &t1, 1);
    if (t >= t1) { val(n - 1, out); return 0; }
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) { size_t mid = (lo + hi) / 2; float tm; cgltf_accessor_read_float(in, mid, &tm, 1); if (tm <= t) lo = mid; else hi = mid; }
    float ta, tb; cgltf_accessor_read_float(in, lo, &ta, 1); cgltf_accessor_read_float(in, hi, &tb, 1);
    double a[4], bv[4]; val(lo, a); val(hi, bv);
    double f = tb > ta ? (t - ta) / (tb - ta) : 0;
    if (sm->interpolation == cgltf_interpolation_type_step) f = 0;
    if (comps == 4) { Q q = qslerp({ a[0], a[1], a[2], a[3] }, { bv[0], bv[1], bv[2], bv[3] }, f); out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w; }
    else for (int i = 0; i < comps; i++) out[i] = a[i] + (bv[i] - a[i]) * f;
    return 0;
}
struct NodeTRS { double t[3], q[4], s[3]; bool mat; double m[16]; };
static M4 NodeWorld(const cgltf_data* d, const cgltf_node* n, const std::vector<NodeTRS>& trs, std::vector<int>& done, std::vector<M4>& cache) {
    size_t i = n - d->nodes;
    if (done[i]) return cache[i];
    M4 local;
    if (trs[i].mat) for (int k = 0; k < 16; k++) local.m[k] = trs[i].m[k];
    else local = FromTRS(trs[i].t, trs[i].q, trs[i].s);
    cache[i] = n->parent ? Mul(NodeWorld(d, n->parent, trs, done, cache), local) : local;
    done[i] = 1;
    return cache[i];
}
static bool LoadGltf(const std::wstring& f, Src& s, std::string& err, const std::wstring& texOverride) {
    std::string bytes; if (!ReadBytes(f, bytes)) { err = "Couldn't read the file."; return false; }
    cgltf_options opt{}; cgltf_data* d = nullptr;
    if (cgltf_parse(&opt, bytes.data(), bytes.size(), &d) != cgltf_result_success) { err = "That glTF file couldn't be read."; return false; }
    std::string dir = W2U(DirOf(f)) + "\\";
    if (cgltf_load_buffers(&opt, d, (dir + W2U(NameOf(f))).c_str()) != cgltf_result_success) { cgltf_free(d); err = "The glTF's data file (.bin) is missing."; return false; }
    std::vector<NodeTRS> rest(d->nodes_count);
    for (size_t i = 0; i < d->nodes_count; i++) {
        auto& n = d->nodes[i]; auto& r = rest[i];
        r.mat = n.has_matrix != 0;
        for (int k = 0; k < 16; k++) r.m[k] = n.matrix[k];
        for (int k = 0; k < 3; k++) { r.t[k] = n.has_translation ? n.translation[k] : 0; r.s[k] = n.has_scale ? n.scale[k] : 1; }
        for (int k = 0; k < 4; k++) r.q[k] = n.has_rotation ? n.rotation[k] : (k == 3 ? 1 : 0);
    }
    std::vector<int> done(d->nodes_count, 0); std::vector<M4> cache(d->nodes_count);
    // materials
    std::map<const cgltf_material*, int> matIndex;
    auto matOf = [&](const cgltf_material* m) -> int {
        auto it = matIndex.find(m); if (it != matIndex.end()) return it->second;
        Mat mm; mm.name = m && m->name ? m->name : "Material";
        if (m && m->has_pbr_metallic_roughness) {
            for (int k = 0; k < 4; k++) mm.factor[k] = m->pbr_metallic_roughness.base_color_factor[k];
            const cgltf_texture* t = m->pbr_metallic_roughness.base_color_texture.texture;
            const cgltf_image* im = t ? t->image : nullptr;
            if (im && im->buffer_view) {
                const unsigned char* p = (const unsigned char*)im->buffer_view->buffer->data + im->buffer_view->offset;
                mm.tex = LoadImageMem(p, im->buffer_view->size, Lower(f) + L"#" + std::to_wstring(im - d->images));
            } else if (im && im->uri && strncmp(im->uri, "data:", 5) != 0) {
                std::string u = im->uri; cgltf_decode_uri(&u[0]); u = u.c_str();
                std::wstring p = DirOf(f) + L"\\" + U2W(u); for (auto& c : p) if (c == L'/') c = L'\\';
                mm.texPath = p; mm.tex = LoadImageFile(p);
            }
        }
        if (m && m->normal_texture.texture && m->normal_texture.texture->image) {
            const cgltf_image* im = m->normal_texture.texture->image;
            if (im->buffer_view) mm.nrm = LoadImageMem((const unsigned char*)im->buffer_view->buffer->data + im->buffer_view->offset, im->buffer_view->size, Lower(f) + L"#n" + std::to_wstring(im - d->images));
            else if (im->uri && strncmp(im->uri, "data:", 5) != 0) { std::string u = im->uri; cgltf_decode_uri(&u[0]); u = u.c_str(); std::wstring p = DirOf(f) + L"\\" + U2W(u); for (auto& c : p) if (c == L'/') c = L'\\'; mm.nrm = LoadImageFile(p); }
        }
        auto gimg = [&](const cgltf_texture* t, const wchar_t* tag) -> const Image* {
            const cgltf_image* im = t ? t->image : nullptr; if (!im) return nullptr;
            if (im->buffer_view) return LoadImageMem((const unsigned char*)im->buffer_view->buffer->data + im->buffer_view->offset, im->buffer_view->size, Lower(f) + tag + std::to_wstring(im - d->images));
            if (im->uri && strncmp(im->uri, "data:", 5) != 0) { std::string u = im->uri; cgltf_decode_uri(&u[0]); u = u.c_str(); std::wstring p = DirOf(f) + L"\\" + U2W(u); for (auto& c : p) if (c == L'/') c = L'\\'; return LoadImageFile(p); }
            return nullptr;
        };
        if (m && m->has_pbr_metallic_roughness) if (const Image* mr = gimg(m->pbr_metallic_roughness.metallic_roughness_texture.texture, L"#mr")) {   // glTF: G roughness, B metal
            mm.mk[1] = mr; mm.mkCh[1] = 1; mm.mk[2] = mr; mm.mkCh[2] = 2; }
        if (m) if (const Image* ao = gimg(m->occlusion_texture.texture, L"#ao")) { mm.mk[0] = ao; mm.mkCh[0] = 0; }
        if (m) {
            mm.blend = m->alpha_mode == cgltf_alpha_mode_mask ? 1 : m->alpha_mode == cgltf_alpha_mode_blend ? 2 : 0;
            mm.twoSided = m->double_sided ? 1 : 0; mm.clip = m->alpha_cutoff > 0 ? m->alpha_cutoff : 0.5f;
        }
        if (!texOverride.empty()) { mm.texPath = texOverride; mm.tex = LoadImageFile(texOverride); for (int k = 0; k < 3; k++) mm.factor[k] = 1; }
        s.mats.push_back(mm);
        return matIndex[m] = (int)s.mats.size() - 1;
    };
    // joints -> bones
    std::map<const cgltf_node*, int> boneOf; std::vector<M4> ibm;
    auto boneFor = [&](const cgltf_node* n, const cgltf_skin* skin, size_t j) -> int {
        auto it = boneOf.find(n); if (it != boneOf.end()) return it->second;
        Bone b; b.name = n->name ? n->name : ("Bone" + std::to_string(s.bones.size()));
        s.bones.push_back(b);
        M4 inv = Ident();
        if (skin && skin->inverse_bind_matrices) { float m[16]; cgltf_accessor_read_float(skin->inverse_bind_matrices, j, m, 16); for (int k = 0; k < 16; k++) inv.m[k] = m[k]; }
        ibm.push_back(inv);
        return boneOf[n] = (int)s.bones.size() - 1;
    };
    // geometry
    for (size_t ni = 0; ni < d->nodes_count; ni++) {
        const cgltf_node* n = &d->nodes[ni];
        if (!n->mesh) continue;
        M4 world = NodeWorld(d, n, rest, done, cache);
        bool skinned = n->skin != nullptr;
        int staticBone = -1;
        if (!skinned) {   // a rigid piece: its own part, and it moves with its node if that's animated
            Bone b; b.name = n->name ? n->name : ("Part" + std::to_string(s.bones.size()));
            s.bones.push_back(b); ibm.push_back(Ident()); staticBone = (int)s.bones.size() - 1;
            boneOf[n] = staticBone;
            // its points are stored in world space; the inverse of the node's world matrix is its "bind"
            M4 inv = Ident();
            { // invert a rigid+scale matrix generally
                double* m = world.m; double inv16[16];
                inv16[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
                inv16[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
                inv16[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
                inv16[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
                inv16[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
                inv16[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
                inv16[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
                inv16[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
                inv16[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
                inv16[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
                inv16[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
                inv16[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
                inv16[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
                inv16[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
                inv16[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
                inv16[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
                double det = m[0] * inv16[0] + m[1] * inv16[4] + m[2] * inv16[8] + m[3] * inv16[12];
                if (std::fabs(det) > 1e-20) for (int k = 0; k < 16; k++) inv.m[k] = inv16[k] / det;
            }
            ibm.back() = inv;
        } else s.skinned = true;
        for (size_t pi = 0; pi < n->mesh->primitives_count; pi++) {
            const cgltf_primitive& pr = n->mesh->primitives[pi];
            if (pr.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *ap = nullptr, *an = nullptr, *at = nullptr, *aj = nullptr, *aw = nullptr;
            for (size_t k = 0; k < pr.attributes_count; k++) {
                auto& a = pr.attributes[k];
                if (a.type == cgltf_attribute_type_position) ap = a.data;
                else if (a.type == cgltf_attribute_type_normal) an = a.data;
                else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) at = a.data;
                else if (a.type == cgltf_attribute_type_joints && a.index == 0) aj = a.data;
                else if (a.type == cgltf_attribute_type_weights && a.index == 0) aw = a.data;
            }
            if (!ap) continue;
            int mat = matOf(pr.material);
            uint32_t base = (uint32_t)s.pos.size();
            for (size_t v = 0; v < ap->count; v++) {
                float p[3] = {}, nn[3] = { 0, 0, 0 }, uv[2] = {};
                cgltf_accessor_read_float(ap, v, p, 3);
                if (an) cgltf_accessor_read_float(an, v, nn, 3);
                if (at) cgltf_accessor_read_float(at, v, uv, 2);
                V3 P{ p[0], p[1], p[2] }, N{ nn[0], nn[1], nn[2] };
                if (!skinned) { P = XformPt(world, P); N = norm(XformDir(world, N)); }
                s.pos.push_back(GlPos(P)); s.nrm.push_back(an ? GlDir(N) : V3{}); s.u.push_back(uv[0]); s.v.push_back(uv[1]); s.vMat.push_back(mat);
                int bone = staticBone;
                if (skinned && aj && aw) {
                    cgltf_uint j[4] = {}; float w[4] = {};
                    cgltf_accessor_read_uint(aj, v, j, 4); cgltf_accessor_read_float(aw, v, w, 4);
                    int bi = 0; for (int k = 1; k < 4; k++) if (w[k] > w[bi]) bi = k;
                    size_t jj = j[bi] < n->skin->joints_count ? j[bi] : 0;
                    bone = boneFor(n->skin->joints[jj], n->skin, jj);
                } else if (skinned) bone = boneFor(n->skin->joints[0], n->skin, 0);
                s.vBone.push_back(bone);
            }
            if (pr.indices) for (size_t k = 0; k < pr.indices->count; k++) s.idx.push_back(base + (uint32_t)cgltf_accessor_read_index(pr.indices, k));
            else for (size_t k = 0; k < ap->count; k++) s.idx.push_back(base + (uint32_t)k);
        }
    }
    if (s.pos.empty()) { cgltf_free(d); err = "That glTF file has no meshes in it."; return false; }
    // animations: sampled at 30 frames a second; each part's move = conv(world(frame) * inverse bind)
    for (size_t ai = 0; ai < d->animations_count; ai++) {
        const cgltf_animation& A = d->animations[ai];
        double dur = 0;
        for (size_t c = 0; c < A.samplers_count; c++) { auto* in = A.samplers[c].input; if (in && in->count) { float t; cgltf_accessor_read_float(in, in->count - 1, &t, 1); dur = std::max(dur, (double)t); } }
        if (dur <= 0) continue;
        Anim an; an.name = A.name && *A.name ? A.name : ("Animation " + std::to_string(ai + 1)); an.fps = 30;
        int frames = std::min(900, (int)std::ceil(dur * 30) + 1);
        for (int fr = 0; fr < frames; fr++) {
            double t = fr / 30.0;
            std::vector<NodeTRS> trs = rest;
            for (size_t c = 0; c < A.channels_count; c++) {
                auto& chn = A.channels[c]; if (!chn.target_node || !chn.sampler) continue;
                auto& r = trs[chn.target_node - d->nodes]; r.mat = false;
                if (chn.target_path == cgltf_animation_path_type_translation) Eval1(chn.sampler, t, 3, r.t);
                else if (chn.target_path == cgltf_animation_path_type_rotation) Eval1(chn.sampler, t, 4, r.q);
                else if (chn.target_path == cgltf_animation_path_type_scale) Eval1(chn.sampler, t, 3, r.s);
            }
            std::vector<int> dn(d->nodes_count, 0); std::vector<M4> ca(d->nodes_count);
            std::vector<Xf> k(s.bones.size());
            for (auto& bo : boneOf) k[bo.second] = GlMatrix(Mul(NodeWorld(d, bo.first, trs, dn, ca), ibm[bo.second]));
            an.k.push_back(std::move(k));
        }
        s.anims.push_back(std::move(an));
    }
    cgltf_free(d);
    return true;
}


// ---------------------------------------------------------------- FBX (read with ufbx, then converted the same way as glTF)
// ufbx turns the file into glTF's frame (metres, Y up, right-handed), so the glTF maths above applies unchanged:
// meshes, materials and their textures (embedded or next to the file), skinned characters and their animations.
static M4 FbxM4(const ufbx_matrix& m) {
    M4 r = Ident();
    for (int c = 0; c < 4; c++) { r.m[c * 4 + 0] = m.cols[c].x; r.m[c * 4 + 1] = m.cols[c].y; r.m[c * 4 + 2] = m.cols[c].z; }
    return r;
}
static const Image* FbxTexture(const ufbx_texture* t, const std::wstring& f, const wchar_t* tag, std::wstring* pathOut) {
    if (!t) return nullptr;
    if (t->type == UFBX_TEXTURE_LAYERED && t->layers.count) t = t->layers.data[0].texture;
    if (!t) return nullptr;
    if (t->content.size > 0)                                  // embedded in the .fbx
        return LoadImageMem(t->content.data, t->content.size, Lower(f) + tag + std::to_wstring(t->typed_id));
    // a file: where it says, next to the .fbx, or in a "textures" folder beside it
    std::vector<std::wstring> tries;
    auto add = [&](const ufbx_string& u) { if (u.length) { std::wstring w = U2W(std::string(u.data, u.length)); for (auto& c : w) if (c == L'/') c = L'\\'; tries.push_back(w); } };
    add(t->absolute_filename); add(t->filename);
    if (t->relative_filename.length) { std::wstring w = U2W(std::string(t->relative_filename.data, t->relative_filename.length)); for (auto& c : w) if (c == L'/') c = L'\\'; tries.push_back(DirOf(f) + L"\\" + w); }
    std::wstring nameOnly;
    for (auto& w : tries) { size_t sl = w.find_last_of(L'\\'); if (sl != std::wstring::npos) { nameOnly = w.substr(sl + 1); break; } else if (!w.empty()) nameOnly = w; }
    if (!nameOnly.empty()) for (auto d : { L"", L"\\textures", L"\\Textures", L"\\..\\textures", L"\\..\\Textures", L"\\tex" }) tries.push_back(DirOf(f) + d + L"\\" + nameOnly);
    for (auto& w : tries) if (w.size() > 1 && FileExists(w)) { if (const Image* im = LoadImageFile(w)) { if (pathOut) *pathOut = w; return im; } }
    return nullptr;
}
static bool LoadFbx(const std::wstring& f, Src& s, std::string& err, const std::wstring& texOverride) {
    std::string bytes; if (!ReadBytes(f, bytes)) { err = "Couldn't read the file."; return false; }
    ufbx_load_opts opt{};
    opt.target_axes = ufbx_axes_right_handed_y_up;           // glTF's frame...
    opt.target_unit_meters = 1.0;                             // ...and units
    opt.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opt.generate_missing_normals = true;
    opt.ignore_missing_external_files = true;
    ufbx_error e{};
    ufbx_scene* sc = ufbx_load_memory(bytes.data(), bytes.size(), &opt, &e);
    if (!sc) {
        char b[256]; ufbx_format_error(b, sizeof b, &e);
        err = std::string("That FBX file couldn't be read (") + (e.description.length ? std::string(e.description.data, e.description.length) : std::string(b)) + ").";
        return false;
    }
    // materials
    std::map<const ufbx_material*, int> matIndex;
    auto matOf = [&](const ufbx_material* m) -> int {
        auto it = matIndex.find(m); if (it != matIndex.end()) return it->second;
        Mat mm; mm.name = m && m->name.length ? std::string(m->name.data, m->name.length) : "Material";
        if (m) {
            const ufbx_material_map& bc = m->pbr.base_color.texture ? m->pbr.base_color : m->fbx.diffuse_color;
            mm.tex = FbxTexture(bc.texture, f, L"#c", &mm.texPath);
            if (!mm.tex) {                                     // no picture: its colour
                const ufbx_material_map& col = m->fbx.diffuse_color.has_value ? m->fbx.diffuse_color : m->pbr.base_color;
                if (col.has_value) for (int k = 0; k < 3; k++) mm.factor[k] = std::clamp((double)col.value_vec4.v[k], 0.0, 1.0);
            }
            const ufbx_texture* nt = m->pbr.normal_map.texture ? m->pbr.normal_map.texture : m->fbx.normal_map.texture ? m->fbx.normal_map.texture : m->fbx.bump.texture;
            mm.nrm = FbxTexture(nt, f, L"#n", nullptr);
            if (const Image* r = FbxTexture(m->pbr.roughness.texture, f, L"#r", nullptr)) { mm.mk[1] = r; mm.mkCh[1] = 0; }
            if (const Image* mt = FbxTexture(m->pbr.metalness.texture, f, L"#m", nullptr)) { mm.mk[2] = mt; mm.mkCh[2] = 0; }
            if (const Image* ao = FbxTexture(m->pbr.ambient_occlusion.texture, f, L"#a", nullptr)) { mm.mk[0] = ao; mm.mkCh[0] = 0; }
            if (const Image* op = FbxTexture(m->fbx.transparency_factor.texture ? m->fbx.transparency_factor.texture : m->pbr.opacity.texture, f, L"#o", nullptr)) { mm.opac = op; mm.blend = 1; }
            else mm.blend = 0;
            if (m->features.double_sided.enabled) mm.twoSided = 1;
        }
        if (!mm.tex && texOverride.empty()) {                 // a picture with the material's name next to the file
            std::wstring p = FindTextureFor(f, mm.name);
            if (!p.empty()) { mm.texPath = p; mm.tex = LoadImageFile(p); }
        }
        if (!texOverride.empty()) { mm.texPath = texOverride; mm.tex = LoadImageFile(texOverride); for (int k = 0; k < 3; k++) mm.factor[k] = 1; }
        s.mats.push_back(mm);
        return matIndex[m] = (int)s.mats.size() - 1;
    };
    // bones: the skeleton's joints, and one part per rigid (unskinned) mesh
    std::map<const ufbx_node*, int> boneOf; std::vector<M4> ibm; std::set<int> rigidParts;
    auto boneFor = [&](const ufbx_node* n, const M4& inv) -> int {
        auto it = boneOf.find(n); if (it != boneOf.end()) return it->second;
        Bone b; b.name = n && n->name.length ? std::string(n->name.data, n->name.length) : ("Bone" + std::to_string(s.bones.size()));
        s.bones.push_back(b); ibm.push_back(inv);
        return boneOf[n] = (int)s.bones.size() - 1;
    };
    // rigid parts start where the first animation starts (a file's stored pose can differ from it, in ways - scale -
    // that moving parts can't show); skinned meshes use their skin's own bind pose
    ufbx_scene* ev0 = nullptr;
    if (sc->anim_stacks.count) { ufbx_error ee{}; ev0 = ufbx_evaluate_scene(sc, sc->anim_stacks.data[0]->anim, sc->anim_stacks.data[0]->time_begin, nullptr, &ee); }
    std::vector<uint32_t> tri;
    for (size_t ni = 0; ni < sc->nodes.count; ni++) {
        const ufbx_node* n = sc->nodes.data[ni];
        const ufbx_mesh* mesh = n->mesh;
        if (!mesh || !mesh->num_faces || n->visible == false) continue;
        // Unreal's collision meshes (UCX_ convex, UBX_ box, USP_ sphere, UCP_ capsule): not drawn, each one a
        // convex shape to bump into (as when an FBX is imported into Unreal itself)
        {
            std::string nm = n->name.length ? std::string(n->name.data, n->name.length) : std::string();
            std::string up = nm.substr(0, 4); for (auto& c : up) c = (char)toupper((unsigned char)c);
            if (up == "UCX_" || up == "UBX_" || up == "USP_" || up == "UCP_") {
                const ufbx_matrix& gw = ev0 ? ev0->nodes.data[n->typed_id]->geometry_to_world : n->geometry_to_world;
                std::vector<V3> h;
                for (size_t vi = 0; vi < mesh->vertices.count && h.size() < 256; vi++) {
                    ufbx_vec3 p = ufbx_transform_position(&gw, mesh->vertices.data[vi]);
                    h.push_back(GlPos(V3{ p.x, p.y, p.z }));
                }
                if (h.size() >= 4) s.hulls.push_back(std::move(h));
                continue;
            }
        }
        const ufbx_skin_deformer* skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
        if (skin && !skin->clusters.count) skin = nullptr;
        if (skin) s.skinned = true;
        const ufbx_matrix& g2wU = ev0 ? ev0->nodes.data[n->typed_id]->geometry_to_world : n->geometry_to_world;
        M4 g2w = FbxM4(g2wU);
        int rigid = -1;
        auto rigidBone = [&]() { if (rigid < 0) { ufbx_matrix inv = ufbx_matrix_invert(&g2wU); rigid = boneFor(n, FbxM4(inv)); rigidParts.insert(rigid); } return rigid; };
        tri.resize(mesh->max_face_triangles * 3);
        for (size_t fi = 0; fi < mesh->faces.count; fi++) {
            ufbx_face face = mesh->faces.data[fi];
            if (face.num_indices < 3) continue;
            uint32_t nt = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
            uint32_t mi = mesh->face_material.count ? mesh->face_material.data[fi] : 0;
            const ufbx_material* um = mi < n->materials.count ? n->materials.data[mi] : mi < mesh->materials.count ? mesh->materials.data[mi] : nullptr;
            int mat = matOf(um);
            for (uint32_t k = 0; k < nt * 3; k++) {
                uint32_t ix = tri[k];
                ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, ix);
                ufbx_vec3 nn = mesh->vertex_normal.exists ? ufbx_get_vertex_vec3(&mesh->vertex_normal, ix) : ufbx_vec3{ 0, 0, 0 };
                ufbx_vec2 uv = mesh->vertex_uv.exists ? ufbx_get_vertex_vec2(&mesh->vertex_uv, ix) : ufbx_vec2{ 0, 0 };
                V3 P{ p.x, p.y, p.z }, N{ nn.x, nn.y, nn.z };
                int bone = -1;
                std::vector<std::pair<int, float>> wl;
                if (skin) {                                    // follows its most important bone, from where that bone was bound
                    uint32_t vtx = mesh->vertex_indices.data[ix];
                    const ufbx_skin_vertex& sv = skin->vertices.data[vtx];
                    if (sv.num_weights > 0) {
                        const ufbx_skin_cluster* cl = skin->clusters.data[skin->weights.data[sv.weight_begin].cluster_index];
                        if (cl->bone_node) {
                            ufbx_matrix bind = ufbx_matrix_mul(&cl->bind_to_world, &cl->geometry_to_bone);
                            M4 bm = FbxM4(bind);
                            P = XformPt(bm, P); N = norm(XformDir(bm, N));
                            ufbx_matrix inv = ufbx_matrix_invert(&cl->bind_to_world);
                            bone = boneFor(cl->bone_node, FbxM4(inv));
                            // and bends with up to 4 bones (the weights come strongest first)
                            for (uint32_t w = 0; w < sv.num_weights && wl.size() < 4; w++) {
                                const ufbx_skin_weight& sw = skin->weights.data[sv.weight_begin + w];
                                const ufbx_skin_cluster* c2 = skin->clusters.data[sw.cluster_index];
                                if (!c2->bone_node || sw.weight < 0.01) continue;
                                ufbx_matrix inv2 = ufbx_matrix_invert(&c2->bind_to_world);
                                wl.push_back({ boneFor(c2->bone_node, FbxM4(inv2)), (float)sw.weight });
                            }
                        }
                    }
                }
                if (bone < 0) { P = XformPt(g2w, P); N = norm(XformDir(g2w, N)); bone = rigidBone(); }
                s.pos.push_back(GlPos(P)); s.nrm.push_back(mesh->vertex_normal.exists ? GlDir(N) : V3{});
                s.u.push_back(uv.x); s.v.push_back(1.0 - uv.y);        // FBX's V runs up, Unreal's down
                s.vMat.push_back(mat); s.vBone.push_back(bone);
                s.vw.push_back(std::move(wl));
                s.idx.push_back((uint32_t)s.pos.size() - 1);
            }
        }
    }
    if (ev0) ufbx_free_scene(ev0);
    if (s.pos.empty()) { ufbx_free_scene(sc); err = "That FBX file has no meshes in it."; return false; }
    // animations: each take sampled at 30 frames a second; each part's move = conv(world(frame) * inverse bind)
    std::vector<std::pair<const ufbx_node*, int>> moving(boneOf.begin(), boneOf.end());
    for (size_t ai = 0; ai < sc->anim_stacks.count; ai++) {
        const ufbx_anim_stack* st = sc->anim_stacks.data[ai];
        double t0 = st->time_begin, dur = st->time_end - st->time_begin;
        if (!(dur > 1e-4)) continue;
        Anim an; an.name = st->name.length ? std::string(st->name.data, st->name.length) : ("Animation " + std::to_string(ai + 1)); an.fps = 30;
        size_t bar = an.name.find('|'); if (bar != std::string::npos && bar + 1 < an.name.size()) an.name = an.name.substr(bar + 1);   // ("Armature|Walk")
        int frames = std::min(900, (int)std::ceil(dur * 30) + 1);
        bool moves = false;
        for (int fr = 0; fr < frames; fr++) {
            ufbx_error ee{};
            ufbx_scene* ev = ufbx_evaluate_scene(sc, st->anim, t0 + fr / 30.0, nullptr, &ee);
            if (!ev) break;
            std::vector<Xf> k(s.bones.size());
            for (auto& mv : moving) {
                const ufbx_node* en = ev->nodes.data[mv.first->typed_id];
                const ufbx_matrix& w = rigidParts.count(mv.second) ? en->geometry_to_world : en->node_to_world;
                k[mv.second] = GlMatrix(Mul(FbxM4(w), ibm[mv.second]));
                if (fr > 0 && !moves) { const Xf& a = an.k[0][mv.second]; const Xf& b = k[mv.second];
                    if (len(sub(a.t, b.t)) > 0.01 || std::fabs(a.r.x - b.r.x) + std::fabs(a.r.y - b.r.y) + std::fabs(a.r.z - b.r.z) + std::fabs(a.r.w - b.r.w) > 1e-4) moves = true; }
            }
            ufbx_free_scene(ev);
            an.k.push_back(std::move(k));
        }
        if (moves && !an.k.empty()) s.anims.push_back(std::move(an));     // (a take where nothing moves is left out)
    }
    ufbx_free_scene(sc);
    return true;
}

// ---------------------------------------------------------------- OBJ
static bool LoadObj(const std::wstring& f, Src& s, std::string& err, const std::wstring& texOverride) {
    std::string t; if (!ReadBytes(f, t)) { err = "Couldn't read the file."; return false; }
    std::vector<V3> P, N; std::vector<std::pair<double, double>> T;
    std::map<std::string, int> mats; int cur = -1;
    std::map<std::string, std::pair<std::wstring, std::array<double, 3>>> mtl;
    auto loadMtl = [&](const std::string& name) {
        std::string m; if (!ReadBytes(DirOf(f) + L"\\" + U2W(name), m)) return;
        std::string curName; size_t p = 0;
        while (p < m.size()) {
            size_t e = m.find('\n', p); std::string l = m.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? m.size() : e + 1;
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (l.rfind("newmtl ", 0) == 0) { curName = l.substr(7); mtl[curName] = { L"", { 0.8, 0.8, 0.8 } }; }
            else if (l.rfind("Kd ", 0) == 0 && !curName.empty()) sscanf(l.c_str() + 3, "%lf %lf %lf", &mtl[curName].second[0], &mtl[curName].second[1], &mtl[curName].second[2]);
            else if (l.rfind("map_Kd ", 0) == 0 && !curName.empty()) { std::string tp = l.substr(7); size_t sp = tp.find_last_of(' '); if (sp != std::string::npos && tp[0] == '-') tp = tp.substr(sp + 1);
                std::wstring w = U2W(tp); for (auto& c : w) if (c == L'/') c = L'\\'; mtl[curName].first = w.size() > 1 && w[1] == L':' ? w : DirOf(f) + L"\\" + w; }
        }
    };
    std::map<std::tuple<int, int, int>, uint32_t> vmap;
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p); std::string l = t.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? t.size() : e + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.size() < 2) continue;
        if (l[0] == 'v' && l[1] == ' ') { V3 v; sscanf(l.c_str() + 2, "%lf %lf %lf", &v.x, &v.y, &v.z); P.push_back(v); }
        else if (l[0] == 'v' && l[1] == 'n') { V3 v; sscanf(l.c_str() + 3, "%lf %lf %lf", &v.x, &v.y, &v.z); N.push_back(v); }
        else if (l[0] == 'v' && l[1] == 't') { double u = 0, v = 0; sscanf(l.c_str() + 3, "%lf %lf", &u, &v); T.push_back({ u, 1 - v }); }
        else if (l.rfind("mtllib ", 0) == 0) loadMtl(l.substr(7));
        else if (l.rfind("usemtl ", 0) == 0) {
            std::string n = l.substr(7);
            auto it = mats.find(n);
            if (it == mats.end()) {
                Mat m; m.name = n;
                auto mi = mtl.find(n);
                if (mi != mtl.end()) { for (int k = 0; k < 3; k++) m.factor[k] = mi->second.second[k]; if (!mi->second.first.empty()) { m.texPath = mi->second.first; m.tex = LoadImageFile(m.texPath); if (m.tex) for (int k = 0; k < 3; k++) m.factor[k] = 1; } }
                if (!texOverride.empty()) { m.texPath = texOverride; m.tex = LoadImageFile(texOverride); for (int k = 0; k < 3; k++) m.factor[k] = 1; }
                s.mats.push_back(m); it = mats.insert({ n, (int)s.mats.size() - 1 }).first;
            }
            cur = it->second;
        } else if (l[0] == 'f' && l[1] == ' ') {
            if (cur < 0) { Mat m; m.name = "Material"; if (!texOverride.empty()) { m.texPath = texOverride; m.tex = LoadImageFile(texOverride); } s.mats.push_back(m); cur = (int)s.mats.size() - 1; }
            std::vector<uint32_t> poly;
            size_t q = 2;
            while (q < l.size()) {
                while (q < l.size() && l[q] == ' ') q++;
                if (q >= l.size()) break;
                size_t qe = l.find(' ', q); std::string tok = l.substr(q, qe == std::string::npos ? std::string::npos : qe - q); q = qe == std::string::npos ? l.size() : qe;
                int vi = 0, ti = 0, nI = 0;
                if (sscanf(tok.c_str(), "%d/%d/%d", &vi, &ti, &nI) < 3 && sscanf(tok.c_str(), "%d//%d", &vi, &nI) < 2 && sscanf(tok.c_str(), "%d/%d", &vi, &ti) < 2) sscanf(tok.c_str(), "%d", &vi);
                auto fix = [](int i, size_t n) { return i < 0 ? (int)n + i : i - 1; };
                vi = fix(vi, P.size()); ti = ti ? fix(ti, T.size()) : -1; nI = nI ? fix(nI, N.size()) : -1;
                if (vi < 0 || vi >= (int)P.size()) continue;
                auto key = std::make_tuple(vi, ti, nI);
                auto it = vmap.find(key);
                if (it == vmap.end()) {
                    s.pos.push_back(GlPos(P[vi]));
                    s.nrm.push_back(nI >= 0 && nI < (int)N.size() ? GlDir(N[nI]) : V3{});
                    s.u.push_back(ti >= 0 && ti < (int)T.size() ? T[ti].first : 0); s.v.push_back(ti >= 0 && ti < (int)T.size() ? T[ti].second : 0);
                    s.vMat.push_back(cur); s.vBone.push_back(0);
                    it = vmap.insert({ key, (uint32_t)s.pos.size() - 1 }).first;
                }
                poly.push_back(it->second);
            }
            for (size_t k = 1; k + 1 < poly.size(); k++) { s.idx.push_back(poly[0]); s.idx.push_back(poly[k]); s.idx.push_back(poly[k + 1]); }
        }
    }
    if (s.pos.empty() || s.idx.empty()) { err = "That .obj has no faces in it."; return false; }
    s.bones.push_back(Bone{ "Root" });
    return true;
}

// ---------------------------------------------------------------- writing the .uemodel
// Imported models belong to the game they were imported for: each game has its own folder (models / games / <game>)
static std::wstring g_modelGame;
static std::mutex g_modelGameMx;
void ModelSetGame(const std::wstring& game) {
    std::wstring safe; for (wchar_t c : game) safe += (wcschr(L"\\/:*?\"<>|", c) || c < 32) ? L'_' : c;
    std::lock_guard<std::mutex> lk(g_modelGameMx); g_modelGame = safe;
}
std::wstring ModelsRoot() { return ModsDir() + L"\\LauncherBridge\\models"; }
std::wstring ModelsDir() {
    std::lock_guard<std::mutex> lk(g_modelGameMx);
    return g_modelGame.empty() ? ModelsRoot() : ModelsRoot() + L"\\games\\" + g_modelGame;
}
// textures are loaded in the game as colour (sRGB) images; values meant to be read as plain numbers (normal maps,
// roughness / metal masks) are stored pre-encoded so the game's conversion gives back the right values
static double LinToSrgb(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }
static unsigned char LinToSrgbByte(int v) { return (unsigned char)std::lround(std::min(1.0, std::max(0.0, LinToSrgb(v / 255.0))) * 255); }
// small flat textures for the slots a borrowed material has that a model doesn't fill: flat normal, matte non-metal
static void WriteNeutralTextures(const std::wstring& dir) {
    struct N { const wchar_t* name; int r, g, b; } list[] = {
        { L"_neutral_normal.png", 128, 128, 255 }, { L"_neutral_orm.png", 255, 204, 0 }, { L"_neutral_mra.png", 0, 204, 255 },
        { L"_neutral_rma.png", 204, 0, 255 }, { L"_neutral_rough.png", 204, 204, 204 }, { L"_neutral_black.png", 0, 0, 0 }, { L"_neutral_white.png", 255, 255, 255 },
        { L"_neutral_metal.png", 0, 0, 0 }, { L"_neutral_ao.png", 255, 255, 255 }, { L"_neutral_spec.png", 128, 128, 128 } };
    for (auto& n : list) {
        std::wstring f = dir + L"\\" + n.name; if (FileExists(f)) continue;
        unsigned char px[16 * 16 * 4];
        for (int i = 0; i < 256; i++) { px[i * 4] = LinToSrgbByte(n.r); px[i * 4 + 1] = LinToSrgbByte(n.g); px[i * 4 + 2] = LinToSrgbByte(n.b); px[i * 4 + 3] = 255; }
        std::string data;
        stbi_write_png_to_func([](void* ctx, void* d, int sz) { ((std::string*)ctx)->append((const char*)d, sz); }, &data, 16, 16, 4, px, 64);
        WriteBytes(f, data);
    }
}
static std::string Fmt(double v) { char b[32]; snprintf(b, sizeof b, "%.3f", std::fabs(v) < 0.0005 ? 0.0 : v); return b; }
static std::string Clean(std::string s) { for (auto& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }
// ---------------------------------------------------------------- materials library
// Mods\LauncherBridge\models\materials\<name>\  colour.png, normal.png (optional), info.txt
// "blend<TAB>twoSided<TAB>clip<TAB>average alpha", e.g. "masked\t1\t0.333\t0.62"
static std::string LookStr(int blend, int two, float clip, double avgAlpha) {
    static const char* names[] = { "opaque", "masked", "translucent", "additive", "modulate" };
    char b[96]; snprintf(b, sizeof b, "%s\t%d\t%.3f\t%.3f", names[std::min(4, std::max(0, blend))], two > 0 ? 1 : 0, clip, std::fabs(avgAlpha));
    return b;
}
std::wstring MaterialsDir() { return ModelsRoot() + L"\\materials"; }   // shared by every game
static std::string SlotName(const std::string& raw, int i) {
    std::string n = raw;
    size_t sl = n.find_last_of('/'); if (sl != std::string::npos) n = n.substr(sl + 1);
    size_t dot = n.find('.'); if (dot != std::string::npos) n = n.substr(0, dot);
    if (n.empty() || n == "Material") n = "Material " + std::to_string(i + 1);
    return n;
}
static std::wstring SafeFolder(const std::string& name) {
    std::wstring w = U2W(name), o;
    for (wchar_t c : w) o += (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
    while (!o.empty() && (o.back() == L' ' || o.back() == L'.')) o.pop_back();
    if (o.empty()) o = L"Material";
    return o.substr(0, 80);
}
static void SaveLibraryMaterial(const std::string& name, const std::string& from, const std::string& colourPng, const std::string& normalPng,
                                const std::map<std::string, std::string>& masks = {}, const std::string& look = "", const std::string& orig = "") {
    std::wstring d = MaterialsDir() + L"\\" + SafeFolder(name);
    MakeDirs(d);
    DeleteFileW((d + L"\\alpha.png").c_str());
    for (auto& mk : masks) WriteBytes(d + (mk.first == "alpha" ? std::wstring(L"\\alpha") : L"\\orm" + (mk.first == "orm" ? L"" : L"_" + U2W(mk.first))) + L".png", mk.second);
    WriteBytes(d + L"\\colour.png", colourPng);
    if (!normalPng.empty()) WriteBytes(d + L"\\normal.png", normalPng);
    WriteBytes(d + L"\\info.txt", "name\t" + Clean(name) + "\nfrom\t" + Clean(from) + "\n" + (look.empty() ? "" : "look\t" + look + "\n") + (orig.empty() ? "" : "original\t" + Clean(orig) + "\n"));
}
std::vector<MaterialInfo> MaterialList() {
    std::vector<MaterialInfo> out;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((MaterialsDir() + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        MaterialInfo mi; mi.dir = MaterialsDir() + L"\\" + fd.cFileName;
        mi.colour = mi.dir + L"\\colour.png"; if (!FileExists(mi.colour)) continue;
        mi.normal = mi.dir + L"\\normal.png"; if (!FileExists(mi.normal)) mi.normal.clear();
        mi.masks = mi.dir + L"\\orm.png"; if (!FileExists(mi.masks)) mi.masks.clear();
        mi.name = W2U(fd.cFileName);
        std::string t;
        if (ReadBytes(mi.dir + L"\\info.txt", t)) {
            size_t p = 0;
            while (p < t.size()) {
                size_t e = t.find('\n', p); std::string l = t.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? t.size() : e + 1;
                if (l.rfind("name\t", 0) == 0) mi.name = l.substr(5);
                else if (l.rfind("from\t", 0) == 0) mi.from = l.substr(5);
                else if (l.rfind("look\t", 0) == 0) mi.look = l.substr(5);
            }
        }
        out.push_back(mi);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const MaterialInfo& a, const MaterialInfo& b) { return LowerA(a.name) < LowerA(b.name); });
    return out;
}
static bool ImageToPng(const std::wstring& file, bool asNormal, std::string& png) {
    int w = 0, h = 0, n = 0;
    std::string raw; if (!ReadBytes(file, raw)) return false;
    unsigned char* px = stbi_load_from_memory((const unsigned char*)raw.data(), (int)raw.size(), &w, &h, &n, 4);
    if (!px) return false;
    int step = 1; while (std::max(w, h) / step > 2048) step *= 2;
    int ow = std::max(1, w / step), oh = std::max(1, h / step);
    std::vector<unsigned char> o((size_t)ow * oh * 4);
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
        const unsigned char* sp = &px[((size_t)(y * step) * w + (size_t)x * step) * 4]; unsigned char* dp = &o[((size_t)y * ow + x) * 4];
        for (int k = 0; k < 3; k++) dp[k] = asNormal ? LinToSrgbByte(sp[k]) : sp[k];
        dp[3] = 255;
    }
    stbi_image_free(px);
    stbi_write_png_to_func([](void* ctx, void* d, int sz) { ((std::string*)ctx)->append((const char*)d, sz); }, &png, ow, oh, 4, o.data(), ow * 4);
    return !png.empty();
}
bool MaterialAdd(const std::wstring& colourImg, std::string& msg) {
    std::string png;
    if (!ImageToPng(colourImg, false, png)) { msg = "That picture couldn't be read."; return false; }
    std::string name = W2U(StemOf(colourImg));
    SaveLibraryMaterial(name, "your picture", png, "");
    msg = "Added material " + name + "."; return true;
}
bool MaterialSetNormal(const std::wstring& dir, const std::wstring& img, std::string& msg) {
    std::string png;
    if (!ImageToPng(img, true, png)) { msg = "That picture couldn't be read."; return false; }
    if (!WriteBytes(dir + L"\\normal.png", png)) { msg = "Couldn't save the normal map."; return false; }
    msg = "Normal map added."; return true;
}
bool MaterialDelete(const std::wstring& dir) {
    for (auto f : { L"colour.png", L"normal.png", L"info.txt", L"orm.png", L"orm_mra.png", L"orm_rma.png", L"orm_rough.png", L"orm_metal.png", L"orm_ao.png", L"alpha.png" }) DeleteFileW((dir + L"\\" + f).c_str());
    return RemoveDirectoryW(dir.c_str()) != 0;
}
bool MaterialPreview(const std::wstring& png, std::vector<unsigned char>& rgba, int& ow, int& oh) {
    std::string raw; if (!ReadBytes(png, raw)) return false;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory((const unsigned char*)raw.data(), (int)raw.size(), &w, &h, &n, 4);
    if (!px) return false;
    int step = 1; while (std::max(w, h) / step > 128) step++;
    ow = std::max(1, w / step); oh = std::max(1, h / step);
    rgba.assign((size_t)ow * oh * 4, 0);
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {      // box average, so the preview doesn't shimmer
        unsigned acc[4] = {}; int cnt = 0;
        for (int yy = 0; yy < step; yy++) for (int xx = 0; xx < step; xx++) {
            int sx = x * step + xx, sy = y * step + yy; if (sx >= w || sy >= h) continue;
            const unsigned char* sp = &px[((size_t)sy * w + sx) * 4]; for (int k = 0; k < 4; k++) acc[k] += sp[k]; cnt++;
        }
        for (int k = 0; k < 3; k++) rgba[((size_t)y * ow + x) * 4 + k] = (unsigned char)(acc[k] / std::max(1, cnt));
        rgba[((size_t)y * ow + x) * 4 + 3] = 255;
    }
    stbi_image_free(px);
    return true;
}
// which library material each slot of a model uses: <model>.mats, "slot\t<i>\t<name>\t<colour.png>\t<normal.png or ->"
static std::wstring SlotsFile(const std::wstring& model) { return model + L".mats"; }
bool ModelSetSlotMaterial(const std::wstring& model, int slot, const std::wstring& matDir, std::string& msg) {
    std::string t; ReadBytes(SlotsFile(model), t);
    std::string o, pre = "slot\t" + std::to_string(slot) + "\t";
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p); std::string l = t.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? t.size() : e + 1;
        if (!l.empty() && l.rfind(pre, 0) != 0) o += l + "\n";
    }
    if (!matDir.empty()) {
        std::wstring c = matDir + L"\\colour.png", n = matDir + L"\\normal.png", om = matDir + L"\\orm.png", al = matDir + L"\\alpha.png";
        std::string look, orig;
        { std::string t2; if (ReadBytes(matDir + L"\\info.txt", t2)) {
            auto field = [&](const char* k) { std::string key = std::string(k) + "\t"; size_t a = t2.find(key); if (a == std::string::npos) return std::string(); size_t e = t2.find('\n', a); return t2.substr(a + key.size(), e == std::string::npos ? std::string::npos : e - a - key.size()); };
            look = field("look"); orig = field("original"); } }
        if (look.empty()) look = "opaque\t0\t0.333\t1.000";
        if (!FileExists(c)) { msg = "That material's picture is missing."; return false; }
        o += pre + W2U(NameOf(matDir)) + "\t" + W2U(c) + "\t" + (FileExists(n) ? W2U(n) : std::string("-")) + "\t" + (FileExists(om) ? W2U(om) : std::string("-")) + "\t" + look + "\t" + (FileExists(al) ? W2U(al) : std::string("-")) + "\t" + (orig.empty() ? "-" : orig) + "\n";
    }
    if (o.empty()) DeleteFileW(SlotsFile(model).c_str());
    else if (!WriteBytes(SlotsFile(model), o)) { msg = "Couldn't save the material choice."; return false; }
    msg = matDir.empty() ? "Back to the model's own material." : "Material changed to " + W2U(NameOf(matDir)) + ".";
    return true;
}
static std::string JsonEsc(const std::string& t) {
    std::string o; for (unsigned char c : t) { if (c == '"' || c == '\\') { o += '\\'; o += (char)c; } else if (c < 32) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += (char)c; } return o;
}
// ---------------------------------------------------------------- writing .glb / .fbx (model export to other tools)
// The model's own space is Unreal's (cm, Z up). glTF is metres, Y up; the FBX is written Y up in cm. Pieces are
// kept apart by material; each material's colour (and normal) picture goes inside the file.
static std::string PngOf(const Image* im, const double* factor = nullptr) {
    std::string out;
    if (!im || im->w <= 0) return out;
    const std::vector<unsigned char>* px = &im->px; std::vector<unsigned char> tinted;
    if (factor && (factor[0] != 1 || factor[1] != 1 || factor[2] != 1)) {
        tinted = im->px;
        for (size_t i = 0; i + 3 < tinted.size(); i += 4) for (int k = 0; k < 3; k++) tinted[i + k] = (unsigned char)std::lround(std::min(255.0, tinted[i + k] * factor[k]));
        px = &tinted;
    }
    stbi_write_png_to_func([](void* ctx, void* data, int size) { ((std::string*)ctx)->append((const char*)data, size); }, &out, im->w, im->h, 4, px->data(), im->w * 4);
    return out;
}
struct OutGroup { int mat; std::vector<uint32_t> verts; std::vector<uint32_t> tris; };
static std::vector<OutGroup> GroupsByMaterial(const Src& s) {
    std::map<int, OutGroup> g; std::map<int, std::unordered_map<uint32_t, uint32_t>> remap;
    for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
        uint32_t a = s.idx[t]; if (a >= s.pos.size()) continue;
        int m = a < s.vMat.size() ? s.vMat[a] : 0;
        auto& G = g[m]; G.mat = m; auto& R = remap[m];
        for (int k = 0; k < 3; k++) {
            uint32_t v = s.idx[t + k]; if (v >= s.pos.size()) v = a;
            auto it = R.find(v);
            if (it == R.end()) { it = R.insert({ v, (uint32_t)G.verts.size() }).first; G.verts.push_back(v); }
            G.tris.push_back(it->second);
        }
    }
    std::vector<OutGroup> out; for (auto& kv : g) out.push_back(std::move(kv.second));
    return out;
}
static bool WriteGlb(const Src& s, const std::wstring& path, const std::string& name, std::string& msg) {
    std::vector<OutGroup> groups = GroupsByMaterial(s);
    if (groups.empty()) { msg = "That model has no triangles to save."; return false; }
    std::string bin, js;
    auto align4 = [&]() { while (bin.size() % 4) bin += '\0'; };
    struct View { size_t off, len; int target; }; std::vector<View> views;
    struct Acc { int view, comp, count; std::string type; std::string minmax; }; std::vector<Acc> accs;
    auto addView = [&](const void* d, size_t n, int target) { align4(); views.push_back({ bin.size(), n, target }); bin.append((const char*)d, n); return (int)views.size() - 1; };
    auto J = [](double v) { char b[40]; snprintf(b, sizeof b, "%.7g", std::isfinite(v) ? v : 0.0); return std::string(b); };
    std::string meshPrims, mats, texs, imgs;
    int nImg = 0;
    std::map<int, int> matOut;
    auto addImage = [&](const std::string& png) {
        int v = addView(png.data(), png.size(), 0);
        if (nImg) { imgs += ","; texs += ","; }
        imgs += "{\"bufferView\":" + std::to_string(v) + ",\"mimeType\":\"image/png\"}";
        texs += "{\"source\":" + std::to_string(nImg) + ",\"sampler\":0}";
        return nImg++;
    };
    for (auto& G : groups) {
        size_t nv = G.verts.size();
        std::vector<float> P(nv * 3), N(nv * 3), T(nv * 2);
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t i = 0; i < nv; i++) {
            uint32_t v = G.verts[i]; V3 p = s.pos[v], n = v < s.nrm.size() ? s.nrm[v] : V3{};
            float g[3] = { (float)(p.x / 100), (float)(p.z / 100), (float)(p.y / 100) };
            for (int k = 0; k < 3; k++) { P[i * 3 + k] = g[k]; mn[k] = std::min(mn[k], g[k]); mx[k] = std::max(mx[k], g[k]); }
            double l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z); if (l < 1e-9) { n = { 0, 0, 1 }; l = 1; }
            N[i * 3] = (float)(n.x / l); N[i * 3 + 1] = (float)(n.z / l); N[i * 3 + 2] = (float)(n.y / l);
            T[i * 2] = (float)(v < s.u.size() ? s.u[v] : 0); T[i * 2 + 1] = (float)(v < s.v.size() ? s.v[v] : 0);
        }
        int vp = addView(P.data(), P.size() * 4, 34962), vn = addView(N.data(), N.size() * 4, 34962), vt = addView(T.data(), T.size() * 4, 34962);
        int vi = addView(G.tris.data(), G.tris.size() * 4, 34963);
        accs.push_back({ vp, 5126, (int)nv, "VEC3", ",\"min\":[" + J(mn[0]) + "," + J(mn[1]) + "," + J(mn[2]) + "],\"max\":[" + J(mx[0]) + "," + J(mx[1]) + "," + J(mx[2]) + "]" });
        accs.push_back({ vn, 5126, (int)nv, "VEC3", "" });
        accs.push_back({ vt, 5126, (int)nv, "VEC2", "" });
        accs.push_back({ vi, 5125, (int)G.tris.size(), "SCALAR", "" });
        int a0 = (int)accs.size() - 4;
        // material
        if (!matOut.count(G.mat)) {
            int mi = (int)matOut.size(); matOut[G.mat] = mi;
            const Mat* m = G.mat >= 0 && G.mat < (int)s.mats.size() ? &s.mats[G.mat] : nullptr;
            std::string mj = "{\"name\":\"" + JsonEsc(m ? m->name : "Material") + "\",\"pbrMetallicRoughness\":{";
            double f[4] = { 1, 1, 1, 1 }; if (m) for (int k = 0; k < 4; k++) f[k] = m->factor[k];
            std::string png = m ? PngOf(m->tex) : "";
            mj += "\"baseColorFactor\":[" + J(f[0]) + "," + J(f[1]) + "," + J(f[2]) + "," + J(m ? m->opacity : 1) + "],\"metallicFactor\":0,\"roughnessFactor\":0.8";
            if (!png.empty()) mj += ",\"baseColorTexture\":{\"index\":" + std::to_string(addImage(png)) + "}";
            mj += "}";
            if (m && m->nrm) { std::string np = PngOf(m->nrm); if (!np.empty()) mj += ",\"normalTexture\":{\"index\":" + std::to_string(addImage(np)) + "}"; }
            if (m && m->blend == 1) mj += ",\"alphaMode\":\"MASK\",\"alphaCutoff\":" + J(m->clip);
            else if (m && m->blend >= 2) mj += ",\"alphaMode\":\"BLEND\"";
            if (m && m->twoSided == 1) mj += ",\"doubleSided\":true";
            mj += "}";
            mats += (mats.empty() ? "" : ",") + mj;
        }
        meshPrims += std::string(meshPrims.empty() ? "" : ",") + "{\"attributes\":{\"POSITION\":" + std::to_string(a0) + ",\"NORMAL\":" + std::to_string(a0 + 1) + ",\"TEXCOORD_0\":" + std::to_string(a0 + 2) +
                     "},\"indices\":" + std::to_string(a0 + 3) + ",\"material\":" + std::to_string(matOut[G.mat]) + "}";
    }
    align4();
    js = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"UE4SS Launcher\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"" + JsonEsc(name) + "\",\"mesh\":0}],"
         "\"meshes\":[{\"name\":\"" + JsonEsc(name) + "\",\"primitives\":[" + meshPrims + "]}],\"materials\":[" + mats + "],";
    if (nImg) js += "\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}],\"images\":[" + imgs + "],\"textures\":[" + texs + "],";
    js += "\"accessors\":[";
    for (size_t i = 0; i < accs.size(); i++) js += std::string(i ? "," : "") + "{\"bufferView\":" + std::to_string(accs[i].view) + ",\"componentType\":" + std::to_string(accs[i].comp) + ",\"count\":" + std::to_string(accs[i].count) + ",\"type\":\"" + accs[i].type + "\"" + accs[i].minmax + "}";
    js += "],\"bufferViews\":[";
    for (size_t i = 0; i < views.size(); i++) js += std::string(i ? "," : "") + "{\"buffer\":0,\"byteOffset\":" + std::to_string(views[i].off) + ",\"byteLength\":" + std::to_string(views[i].len) + (views[i].target ? ",\"target\":" + std::to_string(views[i].target) : "") + "}";
    js += "],\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";
    while (js.size() % 4) js += ' ';
    std::string o;
    auto u32 = [&](uint32_t x) { o.append((const char*)&x, 4); };
    u32(0x46546C67); u32(2); u32((uint32_t)(12 + 8 + js.size() + 8 + bin.size()));
    u32((uint32_t)js.size()); u32(0x4E4F534A); o += js;
    u32((uint32_t)bin.size()); u32(0x004E4942); o += bin;
    MakeDirs(DirOf(path));
    if (!WriteBytes(path, o)) { msg = "Couldn't save " + W2U(path) + "."; return false; }
    return true;
}

// ---- binary FBX 7.4 (what Blender, Maya, 3ds Max and Unreal read)
struct FbxNode {
    std::string name; std::string props; uint32_t nprops = 0; std::vector<FbxNode> kids;
    FbxNode(const std::string& n) : name(n) {}
    FbxNode& I(int32_t v) { props += 'I'; props.append((const char*)&v, 4); nprops++; return *this; }
    FbxNode& L(int64_t v) { props += 'L'; props.append((const char*)&v, 8); nprops++; return *this; }
    FbxNode& D(double v) { props += 'D'; props.append((const char*)&v, 8); nprops++; return *this; }
    FbxNode& S(const std::string& v) { props += 'S'; uint32_t n = (uint32_t)v.size(); props.append((const char*)&n, 4); props += v; nprops++; return *this; }
    FbxNode& R(const std::string& v) { props += 'R'; uint32_t n = (uint32_t)v.size(); props.append((const char*)&n, 4); props += v; nprops++; return *this; }
    template <class T> FbxNode& Arr(char t, const std::vector<T>& a) {
        props += t; uint32_t n = (uint32_t)a.size(), enc = 0, len = (uint32_t)(a.size() * sizeof(T));
        props.append((const char*)&n, 4); props.append((const char*)&enc, 4); props.append((const char*)&len, 4);
        props.append((const char*)a.data(), len); nprops++; return *this;
    }
    FbxNode& Kid(const FbxNode& k) { kids.push_back(k); return kids.back(); }
    FbxNode& Kid(const std::string& n) { kids.emplace_back(n); return kids.back(); }
};
static void FbxWrite(std::string& o, const FbxNode& n) {
    size_t at = o.size();
    uint32_t z = 0; o.append((const char*)&z, 4);
    uint32_t np = n.nprops, pl = (uint32_t)n.props.size(); o.append((const char*)&np, 4); o.append((const char*)&pl, 4);
    o += (char)n.name.size(); o += n.name; o += n.props;
    if (!n.kids.empty()) { for (auto& k : n.kids) FbxWrite(o, k); o.append(13, '\0'); }
    uint32_t end = (uint32_t)o.size(); memcpy(&o[at], &end, 4);
}
static FbxNode FbxP70() { return FbxNode("Properties70"); }
static void P70(FbxNode& p, const std::string& n, const std::string& t1, const std::string& t2, const std::string& f, std::vector<double> v) {
    FbxNode& k = p.Kid("P"); k.S(n).S(t1).S(t2).S(f); for (double d : v) k.D(d);
}
static void P70i(FbxNode& p, const std::string& n, const std::string& t1, int v) { FbxNode& k = p.Kid("P"); k.S(n).S(t1).S("").S("").I(v); }
static void P70s(FbxNode& p, const std::string& n, const std::string& v) { FbxNode& k = p.Kid("P"); k.S(n).S("KString").S("").S("").S(v); }
static bool WriteFbx(const Src& s, const std::wstring& path, const std::string& name, std::string& msg) {
    std::vector<OutGroup> groups = GroupsByMaterial(s);
    if (groups.empty()) { msg = "That model has no triangles to save."; return false; }
    // one mesh with a material index per triangle; positions Y up in cm
    std::vector<uint32_t> vmap(s.pos.size(), UINT32_MAX); std::vector<double> verts; std::vector<int32_t> pvi, matIdx; std::vector<double> nrm, uv; std::vector<int32_t> uvIdx;
    std::vector<int> usedMats;
    for (auto& G : groups) {
        int mi = (int)usedMats.size(); usedMats.push_back(G.mat);
        for (size_t t = 0; t + 2 < G.tris.size(); t += 3) {
            for (int k = 0; k < 3; k++) {
                uint32_t v = G.verts[G.tris[t + k]];
                if (vmap[v] == UINT32_MAX) { vmap[v] = (uint32_t)(verts.size() / 3); V3 p = s.pos[v]; verts.push_back(p.x); verts.push_back(p.z); verts.push_back(p.y); }
                int32_t id = (int32_t)vmap[v];
                pvi.push_back(k == 2 ? -id - 1 : id);
                V3 n = v < s.nrm.size() ? s.nrm[v] : V3{}; double l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z); if (l < 1e-9) { n = { 0, 0, 1 }; l = 1; }
                nrm.push_back(n.x / l); nrm.push_back(n.z / l); nrm.push_back(n.y / l);
                uvIdx.push_back((int32_t)(uv.size() / 2));
                uv.push_back(v < s.u.size() ? s.u[v] : 0); uv.push_back(1.0 - (v < s.v.size() ? s.v[v] : 0));   // FBX UVs start at the bottom
            }
            matIdx.push_back(mi);
        }
    }
    std::wstring dir = DirOf(path), stem = StemOf(path);
    int64_t nextId = 1000000;
    auto id = [&]() { return nextId++; };
    int64_t geoId = id(), modelId = id();
    FbxNode objects("Objects"), conns("Connections");
    // geometry
    {
        FbxNode& g = objects.Kid("Geometry"); g.L(geoId).S(name + std::string("\0\1", 2) + "Geometry").S("Mesh");
        g.Kid("Vertices").Arr('d', verts);
        g.Kid("PolygonVertexIndex").Arr('i', pvi);
        g.Kid("GeometryVersion").I(124);
        FbxNode& ln = g.Kid("LayerElementNormal"); ln.I(0);
        ln.Kid("Version").I(101); ln.Kid("Name").S(""); ln.Kid("MappingInformationType").S("ByPolygonVertex"); ln.Kid("ReferenceInformationType").S("Direct"); ln.Kid("Normals").Arr('d', nrm);
        FbxNode& lu = g.Kid("LayerElementUV"); lu.I(0);
        lu.Kid("Version").I(101); lu.Kid("Name").S("UVMap"); lu.Kid("MappingInformationType").S("ByPolygonVertex"); lu.Kid("ReferenceInformationType").S("IndexToDirect");
        lu.Kid("UV").Arr('d', uv); lu.Kid("UVIndex").Arr('i', uvIdx);
        FbxNode& lm = g.Kid("LayerElementMaterial"); lm.I(0);
        lm.Kid("Version").I(101); lm.Kid("Name").S(""); lm.Kid("MappingInformationType").S("ByPolygon"); lm.Kid("ReferenceInformationType").S("IndexToDirect"); lm.Kid("Materials").Arr('i', matIdx);
        FbxNode& L0 = g.Kid("Layer"); L0.I(0); L0.Kid("Version").I(100);
        for (auto t : { "LayerElementNormal", "LayerElementUV", "LayerElementMaterial" }) { FbxNode& e = L0.Kid("LayerElement"); e.Kid("Type").S(t); e.Kid("TypedIndex").I(0); }
    }
    {
        FbxNode& m = objects.Kid("Model"); m.L(modelId).S(name + std::string("\0\1", 2) + "Model").S("Mesh");
        m.Kid("Version").I(232);
        FbxNode p = FbxP70(); P70(p, "Lcl Scaling", "Lcl Scaling", "", "A", { 1, 1, 1 }); P70i(p, "DefaultAttributeIndex", "int", 0); m.Kid(p);
        m.Kid("Shading").S("Y"); m.Kid("Culling").S("CullingOff");
    }
    conns.Kid("C").S("OO").L(geoId).L(modelId);
    conns.Kid("C").S("OO").L(modelId).L(0);
    int nMat = 0, nTex = 0;
    for (size_t k = 0; k < usedMats.size(); k++) {
        const Mat* m = usedMats[k] >= 0 && usedMats[k] < (int)s.mats.size() ? &s.mats[usedMats[k]] : nullptr;
        int64_t mid = id(); nMat++;
        std::string mname = m ? m->name : "Material";
        FbxNode& mt = objects.Kid("Material"); mt.L(mid).S(mname + std::string("\0\1", 2) + "Material").S("");
        mt.Kid("Version").I(102); mt.Kid("ShadingModel").S("phong"); mt.Kid("MultiLayer").I(0);
        FbxNode p = FbxP70();
        double f[3] = { 0.8, 0.8, 0.8 }; if (m) for (int c = 0; c < 3; c++) f[c] = m->factor[c];
        P70(p, "DiffuseColor", "Color", "", "A", { f[0], f[1], f[2] }); P70(p, "Diffuse", "Vector3D", "Vector", "", { f[0], f[1], f[2] });
        P70(p, "DiffuseFactor", "Number", "", "A", { 1 });
        if (m && m->opacity < 1) P70(p, "TransparencyFactor", "Number", "", "A", { 1 - m->opacity });
        mt.Kid(p);
        conns.Kid("C").S("OO").L(mid).L(modelId);
        // pictures: saved next to the .fbx and also packed inside it
        auto addTex = [&](const Image* im, const char* slot, const char* suffix) {
            std::string png = PngOf(im); if (png.empty()) return;
            std::wstring file = stem + L"_" + std::to_wstring(k) + U2W(suffix) + L".png";
            WriteBytes(dir + L"\\" + file, png);
            int64_t tid = id(), vid = id(); nTex++;
            std::string fn = W2U(file);
            FbxNode& v = objects.Kid("Video"); v.L(vid).S(fn + std::string("\0\1", 2) + "Video").S("Clip");
            v.Kid("Type").S("Clip"); v.Kid("UseMipMap").I(0); v.Kid("Filename").S(W2U(dir + L"\\" + file)); v.Kid("RelativeFilename").S(fn); v.Kid("Content").R(png);
            FbxNode& t = objects.Kid("Texture"); t.L(tid).S(fn + std::string("\0\1", 2) + "Texture").S("");
            t.Kid("Type").S("TextureVideoClip"); t.Kid("Version").I(202); t.Kid("TextureName").S(fn + std::string("\0\1", 2) + "Texture");
            t.Kid("Media").S(fn + std::string("\0\1", 2) + "Video"); t.Kid("FileName").S(W2U(dir + L"\\" + file)); t.Kid("RelativeFilename").S(fn);
            t.Kid("ModelUVTranslation").D(0).D(0); t.Kid("ModelUVScaling").D(1).D(1); t.Kid("Texture_Alpha_Source").S("None"); t.Kid("Cropping").I(0).I(0).I(0).I(0);
            conns.Kid("C").S("OO").L(vid).L(tid);
            conns.Kid("C").S("OP").L(tid).L(mid).S(slot);
        };
        if (m) { addTex(m->tex, "DiffuseColor", ""); addTex(m->nrm, "NormalMap", "_normal"); }
    }
    // header, settings and the object counts
    FbxNode hdr("FBXHeaderExtension");
    hdr.Kid("FBXHeaderVersion").I(1003); hdr.Kid("FBXVersion").I(7400); hdr.Kid("Creator").S("UE4SS Launcher");
    FbxNode gs("GlobalSettings"); gs.Kid("Version").I(1000);
    {
        FbxNode p = FbxP70();
        P70i(p, "UpAxis", "int", 1); P70i(p, "UpAxisSign", "int", 1); P70i(p, "FrontAxis", "int", 2); P70i(p, "FrontAxisSign", "int", 1);
        P70i(p, "CoordAxis", "int", 0); P70i(p, "CoordAxisSign", "int", 1); P70i(p, "OriginalUpAxis", "int", 1); P70i(p, "OriginalUpAxisSign", "int", 1);
        P70(p, "UnitScaleFactor", "double", "Number", "", { 1 }); P70(p, "OriginalUnitScaleFactor", "double", "Number", "", { 1 });
        gs.Kid(p);
    }
    FbxNode defs("Definitions"); defs.Kid("Version").I(100); defs.Kid("Count").I(3 + nMat + nTex * 2);
    auto ot = [&](const char* t, int n) { FbxNode& o = defs.Kid("ObjectType"); o.S(t); o.Kid("Count").I(n); };
    ot("GlobalSettings", 1); ot("Geometry", 1); ot("Model", 1); ot("Material", nMat); if (nTex) { ot("Texture", nTex); ot("Video", nTex); }
    std::string o = std::string("Kaydara FBX Binary  ") + '\0' + '\x1a' + '\0';
    uint32_t ver = 7400; o.append((const char*)&ver, 4);
    FbxWrite(o, hdr); FbxWrite(o, gs); FbxWrite(o, defs); FbxWrite(o, objects); FbxWrite(o, conns);
    o.append(13, '\0');
    static const unsigned char foot1[16] = { 0xfa,0xbc,0xab,0x09,0xd0,0xc8,0xd4,0x66,0xb1,0x76,0xfb,0x83,0x1c,0xf7,0x26,0x7e };
    o.append((const char*)foot1, 16); o.append(4, '\0');
    while (o.size() % 16) o += '\0';
    o.append((const char*)&ver, 4); o.append(120, '\0');
    static const unsigned char foot2[16] = { 0xf8,0x5a,0x8c,0x6a,0xde,0xf5,0xd9,0x7e,0xec,0xe9,0x0c,0xe3,0x75,0x8f,0x29,0x0b };
    o.append((const char*)foot2, 16);
    MakeDirs(dir);
    if (!WriteBytes(path, o)) { msg = "Couldn't save " + W2U(path) + "."; return false; }
    return true;
}

// ---- very detailed models: brought down to the triangle limit by collapsing the smallest edges first (meshoptimizer),
// one material / piece at a time so their edges stay where they are. Only existing vertices are kept, so textures,
// bone weights and animations still fit.
static std::atomic<int> g_maxTris{ 400000 };
static std::string g_lastSimplified;
void ModelSetMaxTriangles(int n) { g_maxTris = std::clamp(n, 50000, 3000000); }
int ModelMaxTriangles() { return g_maxTris; }
static void SimplifyTo(Src& s, size_t target) {
    size_t tris = s.idx.size() / 3;
    if (tris <= target || s.pos.empty()) return;
    std::vector<float> P(s.pos.size() * 3);
    for (size_t i = 0; i < s.pos.size(); i++) { P[i * 3] = (float)s.pos[i].x; P[i * 3 + 1] = (float)s.pos[i].y; P[i * 3 + 2] = (float)s.pos[i].z; }
    std::map<std::pair<int, int>, std::vector<uint32_t>> groups;
    for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
        uint32_t a = s.idx[t];
        auto& g = groups[{ a < s.vMat.size() ? s.vMat[a] : 0, a < s.vBone.size() ? s.vBone[a] : 0 }];
        g.insert(g.end(), s.idx.begin() + t, s.idx.begin() + t + 3);
    }
    double keep = (double)target / tris;
    // Each group is simplified on its own compacted vertex list (not the whole model's buffer, which made big
    // models with many materials take minutes), and the groups run in parallel.
    std::vector<std::vector<uint32_t>*> work; for (auto& kv : groups) work.push_back(&kv.second);
    std::vector<std::vector<uint32_t>> results(work.size());
    std::atomic<size_t> next{ 0 };
    auto run = [&]() {
        std::vector<uint32_t> remap, back, li, d; std::vector<float> lp;
        for (;;) {
            size_t wi = next++; if (wi >= work.size()) break;
            auto& g = *work[wi]; auto& res = results[wi];
            size_t want = std::max<size_t>(3, (size_t)(g.size() / 3 * keep) * 3);
            if (g.size() <= want || g.size() < 300) { res = g; continue; }
            back.clear(); li.resize(g.size()); lp.clear();
            remap.assign(0, 0);
            std::unordered_map<uint32_t, uint32_t> m; m.reserve(g.size());
            for (size_t i = 0; i < g.size(); i++) {
                auto it = m.find(g[i]);
                if (it == m.end()) { it = m.emplace(g[i], (uint32_t)back.size()).first; back.push_back(g[i]); lp.insert(lp.end(), { P[g[i] * 3], P[g[i] * 3 + 1], P[g[i] * 3 + 2] }); }
                li[i] = it->second;
            }
            d.resize(g.size());
            size_t n = g.size();
            for (float err : { 0.02f, 0.2f, 1.f }) {
                float got = 0;
                n = meshopt_simplify(d.data(), li.data(), li.size(), lp.data(), back.size(), 12, want, err, 0, &got);
                if (n <= want * 11 / 10) break;
            }
            if (n > want * 11 / 10) n = meshopt_simplifySloppy(d.data(), li.data(), li.size(), lp.data(), back.size(), 12, want, 1.f, nullptr);
            if (n < 3) { res.assign(g.begin(), g.begin() + std::min<size_t>(g.size(), want)); continue; }
            res.resize(n); for (size_t i = 0; i < n; i++) res[i] = back[d[i]];
        }
    };
    unsigned nt = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> th; for (unsigned i = 1; i < nt; i++) th.emplace_back(run);
    run(); for (auto& t : th) t.join();
    std::vector<uint32_t> outIdx; outIdx.reserve(target * 3 + 64);
    for (auto& r : results) outIdx.insert(outIdx.end(), r.begin(), r.end());
    s.idx.swap(outIdx);
}

static bool WriteModelImpl(const Src& s, const std::wstring& src, const std::wstring& out, const std::wstring& texOverride, std::string& msg);
static bool WriteModel(const Src& s0, const std::wstring& src, const std::wstring& out, const std::wstring& texOverride, std::string& msg) {
    size_t tris = s0.idx.size() / 3, lim = (size_t)g_maxTris;
    g_lastSimplified.clear();
    if (tris <= lim) return WriteModelImpl(s0, src, out, texOverride, msg);
    Src s = s0;
    SimplifyTo(s, lim * 98 / 100);
    size_t now = s.idx.size() / 3;
    if (now > lim) { msg = "That model is too big (" + std::to_string(tris) + " triangles) and couldn't be simplified below the limit of " + std::to_string(lim) + ". Raise the limit on the Models page, or export a lower detail level (LOD) from FModel."; return false; }
    g_lastSimplified = "simplified from " + std::to_string(tris) + " to " + std::to_string(now) + " triangles";
    return WriteModelImpl(s, src, out, texOverride, msg);
}
std::string ModelLastSimplified() { return g_lastSimplified; }
static bool WriteModelImpl(const Src& s, const std::wstring& src, const std::wstring& out, const std::wstring& texOverride, std::string& msg) {
    // normals for meshes that don't have them
    std::vector<V3> nrm = s.nrm;
    bool needN = true; for (auto& n : nrm) if (n.x != 0 || n.y != 0 || n.z != 0) { needN = false; break; }
    if (needN) {
        nrm.assign(s.pos.size(), V3{});
        for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
            V3 a = s.pos[s.idx[t]], b = s.pos[s.idx[t + 1]], c = s.pos[s.idx[t + 2]];
            V3 fn = cross(sub(b, a), sub(c, a));
            for (int k = 0; k < 3; k++) nrm[s.idx[t + k]] = add(nrm[s.idx[t + k]], fn);
        }
        // Unreal's triangle order is clockwise from the front, which gives the opposite cross product
        for (auto& n : nrm) n = mul(norm(n), -1);
    } else for (auto& n : nrm) n = norm(n);
    // tangents from the UVs (needed for normal maps): per-triangle UV derivatives, averaged per vertex
    std::vector<V3> tan(s.pos.size()), bit(s.pos.size());
    for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
        uint32_t i0 = s.idx[t], i1 = s.idx[t + 1], i2 = s.idx[t + 2];
        V3 e1 = sub(s.pos[i1], s.pos[i0]), e2 = sub(s.pos[i2], s.pos[i0]);
        double du1 = s.u[i1] - s.u[i0], dv1 = s.v[i1] - s.v[i0], du2 = s.u[i2] - s.u[i0], dv2 = s.v[i2] - s.v[i0];
        double det = du1 * dv2 - du2 * dv1; if (std::fabs(det) < 1e-12) continue;
        double rr = 1 / det;
        V3 T = mul(sub(mul(e1, dv2), mul(e2, dv1)), rr), Bt = mul(sub(mul(e2, du1), mul(e1, du2)), rr);
        for (uint32_t i : { i0, i1, i2 }) { tan[i] = add(tan[i], T); bit[i] = add(bit[i], Bt); }
    }
    std::vector<int> flipY(s.pos.size(), 0);
    for (size_t i = 0; i < s.pos.size(); i++) {
        V3 n = i < nrm.size() ? nrm[i] : V3{ 0, 0, 1 };
        V3 t = sub(tan[i], mul(n, n.x * tan[i].x + n.y * tan[i].y + n.z * tan[i].z));
        if (len(t) < 1e-9) { V3 a = std::fabs(n.z) < 0.9 ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 }; t = cross(a, n); }
        tan[i] = norm(t);
        V3 c = cross(n, tan[i]);
        flipY[i] = (c.x * bit[i].x + c.y * bit[i].y + c.z * bit[i].z) < 0 ? 1 : 0;
    }
    // split into one part per bone and material (each part is one piece in the game, with one material)
    struct Part { int bone; int mat; std::vector<uint32_t> verts; std::vector<uint32_t> tris; std::unordered_map<uint32_t, uint32_t> remap; };
    std::map<std::pair<int, int>, Part> parts;
    for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
        int bone = s.vBone[s.idx[t]], mat = s.vMat[s.idx[t]];
        auto& p = parts[{ bone, mat }]; p.bone = bone; p.mat = mat;
        for (int k = 0; k < 3; k++) {
            uint32_t v = s.idx[t + k];
            auto it = p.remap.find(v);
            if (it == p.remap.end()) { it = p.remap.insert({ v, (uint32_t)p.verts.size() }).first; p.verts.push_back(v); }
            p.tris.push_back(it->second);
        }
    }
    size_t totalV = 0, totalT = 0; for (auto& p : parts) { totalV += p.second.verts.size(); totalT += p.second.tris.size() / 3; }
    if (totalT > (size_t)g_maxTris + 1000) { msg = "That model is too big (" + std::to_string(totalT) + " triangles; the limit is " + std::to_string((int)g_maxTris) + "). Raise the limit on the Models page, or export a lower detail level (LOD) from FModel."; return false; }
    // real skinning data (game exports with animations): vertices that don't simply follow their part's bone get
    // their bone weights, and each frame gets those bones' poses, so the game can bend the mesh properly
    std::vector<int> skinBones; std::unordered_map<int, int> skinIdx;
    auto needsSkin = [&](uint32_t v, int partBone) {
        if (s.anims.empty() || v >= s.vw.size() || s.vw[v].empty()) return false;
        return s.vw[v].size() > 1 || s.vw[v][0].first != partBone;
    };
    for (auto& pp : parts) for (uint32_t v : pp.second.verts) if (needsSkin(v, pp.second.bone))
        for (auto& w : s.vw[v]) if (!skinIdx.count(w.first)) { skinIdx[w.first] = (int)skinBones.size(); skinBones.push_back(w.first); }
    std::string o;
    o.reserve(totalV * 70 + totalT * 20 + 4096);
    bool textured = false; for (auto& m : s.mats) if (m.tex) textured = true;
    o += "UEMODEL 1\nname\t" + Clean(W2U(StemOf(src))) + "\nsource\t" + Clean(W2U(src)) + "\n";
    if (!texOverride.empty()) o += "texture\t" + Clean(W2U(texOverride)) + "\n";
    for (auto& a : s.psaFiles) o += "psa\t" + Clean(W2U(a)) + "\n";
    o += "textured\t" + std::string(textured ? "1" : "0") + "\nstats\t" + std::to_string(parts.size()) + "\t" + std::to_string(totalV) + "\t" + std::to_string(totalT) + "\n";
    for (auto& a : s.anims) o += "animname\t" + Clean(a.name) + "\n";
    if (!skinBones.empty()) o += "skinbones\t" + std::to_string(skinBones.size()) + "\n";
    // each material's colour texture is saved as a .png next to the model, so the game can show the real texture
    std::wstring base = DirOf(out) + L"\\" + StemOf(out);
    // identical pictures (materials sharing one texture) are encoded once and share one file, and the encoding
    // runs on worker threads (a big model can have dozens of 2048 pictures, which took most of the import time)
    using PngRef = std::shared_ptr<std::string>;
    std::unordered_map<uint64_t, std::pair<std::wstring, PngRef>> encCache;
    MakeDirs(DirOf(out));
    struct EncJob { std::vector<unsigned char> px; int w, h; std::wstring fn; PngRef d; };
    std::deque<EncJob> encQ; std::mutex encM; std::condition_variable encCv; bool encDone = false;
    auto encWorker = [&]() {
        for (;;) {
            EncJob j;
            { std::unique_lock<std::mutex> lk(encM); encCv.wait(lk, [&] { return encDone || !encQ.empty(); }); if (encQ.empty()) return; j = std::move(encQ.front()); encQ.pop_front(); }
            encCv.notify_all();
            std::string d; stbi_write_png_to_func([](void* ctx, void* dd, int sz) { ((std::string*)ctx)->append((const char*)dd, sz); }, &d, j.w, j.h, 4, j.px.data(), j.w * 4);
            if (!d.empty()) WriteBytes(j.fn, d);
            *j.d = std::move(d);
        }
    };
    unsigned encN = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> encTh; for (unsigned i = 0; i < encN; i++) encTh.emplace_back(encWorker);
    auto EncSave = [&](std::vector<unsigned char>& px, int ow, int oh, const std::wstring& fn, PngRef& dataOut, bool share = true) -> std::wstring {
        uint64_t h = 1469598103934665603ull ^ ((uint64_t)ow << 32 | (uint32_t)oh);
        const uint64_t* q = (const uint64_t*)px.data(); size_t n8 = px.size() / 8;
        for (size_t i = 0; i < n8; i++) h = (h ^ q[i]) * 1099511628211ull;
        for (size_t i = n8 * 8; i < px.size(); i++) h = (h ^ px[i]) * 1099511628211ull;
        auto it = share ? encCache.find(h) : encCache.end();
        if (it != encCache.end()) { dataOut = it->second.second; return it->second.first; }
        dataOut = std::make_shared<std::string>();
        if (share) encCache[h] = { fn, dataOut };
        std::unique_lock<std::mutex> lk(encM);
        encCv.wait(lk, [&] { return encQ.size() < encN * 2; });     // (keeps memory bounded)
        encQ.push_back(EncJob{ std::move(px), ow, oh, fn, dataOut });
        lk.unlock(); encCv.notify_all();
        return fn;
    };
    struct LibSave { std::string name; PngRef colour, normal, alpha; std::map<std::string, PngRef> masks; std::string look, orig; };
    std::vector<LibSave> libSaves;
    for (size_t mi = 0; mi < s.mats.size(); mi++) {
        const Mat& m = s.mats[mi]; std::wstring png, apng; PngRef colourData, normalData, alphaData; double avgAlpha = 1; int blendOut = m.blend;
        if (m.tex && m.tex->w > 0) {
            const Image& im = *m.tex; int w = im.w, h = im.h, step = 1;
            while (std::max(w, h) / step > 2048) step *= 2;              // full detail, up to 2048
            double alphaSum = 0;
            int ow = std::max(1, w / step), oh = std::max(1, h / step);
            std::vector<unsigned char> px((size_t)ow * oh * 4);
            for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
                const unsigned char* sp = &im.px[((size_t)(y * step) * w + (size_t)x * step) * 4];
                unsigned char* dp = &px[((size_t)y * ow + x) * 4];
                for (int k = 0; k < 3; k++) dp[k] = (unsigned char)std::lround(std::min(255.0, sp[k] * m.factor[k]));
                int a = 255;
                if (m.blend > 0) {            // see-through materials keep their alpha (from a separate opacity texture if there is one)
                    if (m.opac && m.opac->w > 0) { int sx = std::min(m.opac->w - 1, x * m.opac->w / ow), sy = std::min(m.opac->h - 1, y * m.opac->h / oh); a = m.opac->px[((size_t)sy * m.opac->w + sx) * 4]; }
                    else a = sp[3];
                    a = (int)std::lround(a * m.opacity);
                }
                dp[3] = (unsigned char)a; alphaSum += a;
            }
            { std::vector<unsigned char> pc = m.blend > 0 ? px : std::move(px); png = EncSave(pc, ow, oh, base + L"_tex" + std::to_wstring(mi) + L".png", colourData); }
            avgAlpha = alphaSum / std::max(1.0, (double)ow * oh) / 255.0;
            if (blendOut == 1 && avgAlpha > 0.999) blendOut = 0;    // "masked" but nothing cut out (often just for a fade effect): draw solid
            if (blendOut >= 2 && avgAlpha > 0.999 && !m.opac && m.opacity >= 1) avgAlpha = -0.35;   // (negative: no alpha picture, use 0.35)   // see-through but its amount is built into the material: assume glass-like
            if (blendOut > 0 && avgAlpha >= 0 && !png.empty()) {   // the alpha on its own (grey), for materials that read opacity from a separate texture
                std::vector<unsigned char> ap((size_t)ow * oh * 4);
                for (size_t i = 0; i < (size_t)ow * oh; i++) { unsigned char v = LinToSrgbByte(px[i * 4 + 3]); ap[i * 4] = ap[i * 4 + 1] = ap[i * 4 + 2] = v; ap[i * 4 + 3] = 255; }
                apng = EncSave(ap, ow, oh, base + L"_alpha" + std::to_wstring(mi) + L".png", alphaData);
            }
        }
        // the normal map, stored so that the game's colour-space conversion gives back the original values
        std::wstring npng;
        if (m.nrm && m.nrm->w > 0) {
            const Image& im = *m.nrm; int w = im.w, h = im.h, step = 1;
            while (std::max(w, h) / step > 2048) step *= 2;
            int ow = std::max(1, w / step), oh = std::max(1, h / step);
            std::vector<unsigned char> px((size_t)ow * oh * 4);
            for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
                const unsigned char* sp = &im.px[((size_t)(y * step) * w + (size_t)x * step) * 4];
                unsigned char* dp = &px[((size_t)y * ow + x) * 4];
                for (int k = 0; k < 3; k++) dp[k] = LinToSrgbByte(sp[k]);
                dp[3] = 255;
            }
            npng = EncSave(px, ow, oh, base + L"_nrm" + std::to_wstring(mi) + L".png", normalData);
        }
        // the surface masks, as one ORM picture plus the other layouts a borrowed material might want
        std::wstring opng; std::map<std::string, PngRef> maskPngs;
        if (m.mk[0] || m.mk[1] || m.mk[2]) {
            int W = 0, H = 0; for (auto im : m.mk) if (im && im->w * im->h > W * H) { W = im->w; H = im->h; }
            int step = 1; while (std::max(W, H) / step > 1024) step *= 2;
            int ow = std::max(1, W / step), oh = std::max(1, H / step);
            std::vector<unsigned char> ao((size_t)ow * oh, 255), ro((size_t)ow * oh, 204), me((size_t)ow * oh, 0);
            unsigned char* dst[3] = { ao.data(), ro.data(), me.data() };
            for (int c = 0; c < 3; c++) if (const Image* im = m.mk[c]) for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
                int sx = std::min(im->w - 1, x * im->w / ow), sy = std::min(im->h - 1, y * im->h / oh);
                dst[c][(size_t)y * ow + x] = im->px[((size_t)sy * im->w + sx) * 4 + m.mkCh[c]];
            }
            auto save = [&](const char* kind, int r, int g, int b) {   // r,g,b: 0 ao 1 rough 2 metal
                std::vector<unsigned char> px((size_t)ow * oh * 4);
                for (size_t i = 0; i < (size_t)ow * oh; i++) { px[i * 4] = LinToSrgbByte(dst[r][i]); px[i * 4 + 1] = LinToSrgbByte(dst[g][i]); px[i * 4 + 2] = LinToSrgbByte(dst[b][i]); px[i * 4 + 3] = 255; }
                bool isOrm = !strcmp(kind, "orm");
                std::wstring want = base + L"_orm" + std::to_wstring(mi) + (isOrm ? L"" : L"_" + U2W(kind)) + L".png";
                PngRef d; std::wstring fn = EncSave(px, ow, oh, want, d, isOrm);   // (the other layouts are named after the orm file)
                if (!fn.empty()) { maskPngs[kind] = d; if (isOrm) opng = fn; }
                return fn == want;
            };
            // a shared orm picture already has its other layouts saved next to it
            if (save("orm", 0, 1, 2)) { save("mra", 2, 1, 0); save("rma", 1, 2, 0); save("rough", 1, 1, 1); save("metal", 2, 2, 2); save("ao", 0, 0, 0); }
        }
        // the glow picture (masked, downscaled to 1024 at most) and its average colour (for stand-ins with no glow-picture slot)
        std::wstring epng; double eavg[3] = { 1, 1, 1 };
        if (m.glow && m.emi && m.emi->w > 0) {
            const Image& im = *m.emi; int step = 1; while (std::max(im.w, im.h) / step > 1024) step *= 2;
            int ow = std::max(1, im.w / step), oh = std::max(1, im.h / step);
            std::vector<unsigned char> px((size_t)ow * oh * 4); double acc[3] = { 0, 0, 0 };
            for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
                const unsigned char* sp = &im.px[((size_t)(y * step) * im.w + (size_t)x * step) * 4]; unsigned char* dp = &px[((size_t)y * ow + x) * 4];
                double mk = 1;
                if (const Image* mi2 = m.emiMask) { int mx = std::min(mi2->w - 1, x * mi2->w / ow), my = std::min(mi2->h - 1, y * mi2->h / oh); mk = mi2->px[((size_t)my * mi2->w + mx) * 4 + m.emiMaskCh] / 255.0; }
                for (int k = 0; k < 3; k++) { double lin = SrgbToLin(sp[k] / 255.0) * mk; dp[k] = (unsigned char)std::lround(LinToSrgb(lin) * 255); acc[k] += lin; }
                dp[3] = 255;
            }
            for (int k = 0; k < 3; k++) eavg[k] = acc[k] / ((double)ow * oh);
            PngRef d; epng = EncSave(px, ow, oh, base + L"_emi" + std::to_wstring(mi) + L".png", d);
        }
        std::string glowFields;
        if (m.glow) {
            char gb[160]; snprintf(gb, sizeof gb, "\t%.3f,%.3f,%.3f\t%.3f,%.3f,%.3f", m.emiCol[0], m.emiCol[1], m.emiCol[2], eavg[0], eavg[1], eavg[2]);
            glowFields = "\t" + (epng.empty() ? std::string("-") : Clean(W2U(NameOf(epng)))) + gb;
        }
        unsigned char c[3]; Mat plain = m; plain.tex = nullptr; Sample(plain, 0, 0, c);
        o += "mat\t" + std::to_string(mi) + "\t" + (png.empty() ? "-" : Clean(W2U(NameOf(png)))) + "\t" + std::to_string(c[0]) + " " + std::to_string(c[1]) + " " + std::to_string(c[2]) +
             "\t" + (npng.empty() ? "-" : Clean(W2U(NameOf(npng)))) + "\t" + Clean(SlotName(m.name, (int)mi)) + "\t" + (opng.empty() ? "-" : Clean(W2U(NameOf(opng)))) + "\t" + LookStr(blendOut, m.twoSided, m.clip, avgAlpha) + "\t" + (apng.empty() ? "-" : Clean(W2U(NameOf(apng)))) + "\t" + (m.orig.empty() ? "-" : Clean(m.orig)) + glowFields + "\n";
        // and into the materials library, so it can be put on any other imported model
        if (colourData) libSaves.push_back(LibSave{ SlotName(m.name, (int)mi), colourData, normalData, alphaData, maskPngs, LookStr(blendOut, m.twoSided, m.clip, avgAlpha), m.orig });
    }
    { std::lock_guard<std::mutex> lk(encM); encDone = true; }
    encCv.notify_all(); for (auto& t : encTh) t.join();
    for (auto& ls : libSaves) {
        if (ls.colour->empty()) continue;
        std::map<std::string, std::string> mk; for (auto& kv : ls.masks) if (kv.second && !kv.second->empty()) mk[kv.first] = *kv.second;
        if (ls.alpha && !ls.alpha->empty()) mk["alpha"] = *ls.alpha;
        SaveLibraryMaterial(ls.name, W2U(StemOf(out)), *ls.colour, ls.normal ? *ls.normal : std::string(), mk, ls.look, ls.orig);
    }
    for (auto& pp : parts) {
        auto& p = pp.second;
        std::string bn = p.bone >= 0 && p.bone < (int)s.bones.size() ? s.bones[p.bone].name : "Part";
        o += "part\t" + Clean(bn) + "\t" + std::to_string(p.verts.size()) + "\t" + std::to_string(p.tris.size() / 3) + "\t" + std::to_string(p.mat) + "\n";
        for (auto v : p.verts) {
            unsigned char c[3]; int mi = s.vMat[v];
            if (mi >= 0 && mi < (int)s.mats.size()) Sample(s.mats[mi], s.u[v], s.v[v], c); else { c[0] = c[1] = c[2] = 140; }
            const V3& P = s.pos[v]; const V3& N = nrm[v];
            char uv[128]; snprintf(uv, sizeof uv, " %.4f %.4f %.3f %.3f %.3f %d", s.u[v], s.v[v], tan[v].x, tan[v].y, tan[v].z, flipY[v]);
            o += "v " + Fmt(P.x) + " " + Fmt(P.y) + " " + Fmt(P.z) + " " + Fmt(N.x) + " " + Fmt(N.y) + " " + Fmt(N.z) + " " +
                 std::to_string(c[0]) + " " + std::to_string(c[1]) + " " + std::to_string(c[2]) + uv + "\n";
            if (!skinBones.empty() && needsSkin(v, p.bone)) {
                float tot = 0; for (auto& w : s.vw[v]) tot += w.second;
                o += "w";
                for (auto& w : s.vw[v]) { char wb[48]; snprintf(wb, sizeof wb, " %d %.3f", skinIdx[w.first], tot > 0 ? w.second / tot : 0.f); o += wb; }
                o += "\n";
            }
        }
        for (size_t t = 0; t < p.tris.size(); t += 3) o += "t " + std::to_string(p.tris[t]) + " " + std::to_string(p.tris[t + 1]) + " " + std::to_string(p.tris[t + 2]) + "\n";
    }
    // Big still models: a light copy of the shape for collision (a few thousand triangles). Bumping into, standing
    // on and the game's own traces against a million-triangle collision mesh made the game lag near the model.
    if (s.anims.empty() && s.hulls.empty() && totalT > 40000) {
        std::vector<float> P(s.pos.size() * 3);
        for (size_t i = 0; i < s.pos.size(); i++) { P[i * 3] = (float)s.pos[i].x; P[i * 3 + 1] = (float)s.pos[i].y; P[i * 3 + 2] = (float)s.pos[i].z; }
        // weld by position (UV seams and material edges split vertices, which would stop the simplifier)
        std::vector<unsigned int> remap(s.pos.size()); std::vector<float> wp; wp.reserve(P.size());
        {
            struct KH { size_t operator()(const std::array<float, 3>& k) const { uint32_t a, b, c; memcpy(&a, &k[0], 4); memcpy(&b, &k[1], 4); memcpy(&c, &k[2], 4); return (size_t)a * 73856093u ^ (size_t)b * 19349663u ^ (size_t)c * 83492791u; } };
            std::unordered_map<std::array<float, 3>, unsigned, KH> at; at.reserve(s.pos.size());
            for (size_t i = 0; i < s.pos.size(); i++) {
                std::array<float, 3> k{ P[i * 3], P[i * 3 + 1], P[i * 3 + 2] };
                auto it = at.emplace(k, (unsigned)(wp.size() / 3));
                if (it.second) wp.insert(wp.end(), k.begin(), k.end());
                remap[i] = it.first->second;
            }
        }
        size_t nu = wp.size() / 3;
        std::vector<unsigned int> wi(s.idx.size()); for (size_t i = 0; i < s.idx.size(); i++) wi[i] = remap[s.idx[i]];
        size_t want = 6000 * 3;
        std::vector<unsigned int> d(wi.size());
        size_t n = meshopt_simplify(d.data(), wi.data(), wi.size(), wp.data(), nu, 12, want, 0.02f, 0, nullptr);
        if (n > want * 2) n = meshopt_simplifySloppy(d.data(), wi.data(), wi.size(), wp.data(), nu, 12, want, 1.f, nullptr);
        if (n >= 3) {
            std::vector<unsigned int> cvm(nu, ~0u); std::vector<unsigned int> cv;
            for (size_t i = 0; i < n; i++) { unsigned& m2 = cvm[d[i]]; if (m2 == ~0u) { m2 = (unsigned)cv.size(); cv.push_back(d[i]); } d[i] = m2; }
            o += "coll\t" + std::to_string(cv.size()) + "\t" + std::to_string(n / 3) + "\n";
            for (unsigned v : cv) o += "cv " + Fmt(wp[v * 3]) + " " + Fmt(wp[v * 3 + 1]) + " " + Fmt(wp[v * 3 + 2]) + "\n";
            for (size_t i = 0; i + 2 < n; i += 3) o += "ct " + std::to_string(d[i]) + " " + std::to_string(d[i + 1]) + " " + std::to_string(d[i + 2]) + "\n";
        }
    }
    for (auto& f : s.fx) { std::string l = f; for (auto& c : l) if (c == '\n' || c == '\r') c = ' '; o += "fx\t" + l + "\n"; }
    for (auto& f : s.real) { std::string l = f; for (auto& c : l) if (c == '\n' || c == '\r') c = ' '; o += "real\t" + l + "\n"; }
    for (auto& h : s.hulls) {                  // collision shapes: hull <count> x y z x y z ...
        o += "hull\t" + std::to_string(h.size());
        for (auto& p : h) o += "\t" + Fmt(p.x) + "," + Fmt(p.y) + "," + Fmt(p.z);
        o += "\n";
    }
    for (auto& a : s.anims) {
        o += "anim\t" + Clean(a.name) + "\t" + Fmt(a.fps) + "\t" + std::to_string(a.k.size()) + (a.path.empty() ? "" : "\t" + Clean(a.path)) + "\n";
        for (auto& fr : a.k) {
            for (auto& pp : parts) {
                Xf x = pp.second.bone >= 0 && pp.second.bone < (int)fr.size() ? fr[pp.second.bone] : Xf{};
                double pi, ya, ro; Rotator(x.r, pi, ya, ro);
                o += "k " + Fmt(x.t.x) + " " + Fmt(x.t.y) + " " + Fmt(x.t.z) + " " + Fmt(pi) + " " + Fmt(ya) + " " + Fmt(ro) + "\n";
            }
            for (int b : skinBones) {     // the skin bones' poses this frame (quaternion + move)
                Xf x = b >= 0 && b < (int)fr.size() ? fr[b] : Xf{};
                char bb[160]; snprintf(bb, sizeof bb, "b %.5f %.5f %.5f %.5f %.3f %.3f %.3f\n", x.r.x, x.r.y, x.r.z, x.r.w, x.t.x, x.t.y, x.t.z);
                o += bb;
            }
        }
    }
    MakeDirs(DirOf(out));
    WriteNeutralTextures(DirOf(out));
    if (!WriteBytes(out, o)) { msg = "Couldn't save the converted model."; return false; }
    char b[300];
    snprintf(b, sizeof b, "Imported %s: %d part%s, %d triangles%s%s.", W2U(StemOf(src)).c_str(), (int)parts.size(), parts.size() == 1 ? "" : "s", (int)totalT,
             s.anims.empty() ? "" : (", " + std::to_string(s.anims.size()) + " animation" + (s.anims.size() == 1 ? "" : "s")).c_str(),
             textured ? ", coloured from its texture" : ", no texture found (plain colour)");
    msg = b;
    return true;
}
static bool Convert(const std::wstring& src, const std::wstring& out, const std::vector<std::wstring>& extraPsa, const std::wstring& texOverride, std::string& msg) {
    Src s; std::string err;
    std::wstring e = ExtLower(src);
    bool ok = false;
    if (e == L".psk" || e == L".pskx") ok = LoadPsk(src, s, err, texOverride);
    else if (e == L".gltf" || e == L".glb") ok = LoadGltf(src, s, err, texOverride);
    else if (e == L".obj") ok = LoadObj(src, s, err, texOverride);
    else if (e == L".fbx") ok = LoadFbx(src, s, err, texOverride);
    else err = "That file type isn't supported. Use .fbx, .psk (with .psa animations), .gltf / .glb or .obj.";
    if (!ok) { msg = err; return false; }
    std::string animNote;
    if (s.skinned && (e == L".psk" || e == L".pskx")) {
        // animations: the ones you picked, plus any .psa near the model with the same skeleton
        std::vector<std::wstring> psa = extraPsa;
        if (extraPsa.empty()) {
            std::vector<std::string> mine; for (auto& b : s.bones) mine.push_back(LowerA(b.name));
            int budget = 4000;
            Walk(DirOf(DirOf(src)), 4, [&](const std::wstring& p) {
                if (ExtLower(p) != L".psa" || psa.size() >= 40) return;
                auto names = PsaBoneNames(p); int hit = 0;
                for (auto& n : names) if (std::find(mine.begin(), mine.end(), n) != mine.end()) hit++;
                if (!names.empty() && hit * 10 >= (int)std::min(names.size(), mine.size()) * 6) psa.push_back(p);
            }, budget);
        }
        for (auto& p : psa) if (LoadPsa(p, s, animNote)) s.psaFiles.push_back(p);
    }
    bool r = WriteModel(s, src, out, texOverride, msg);
    if (r && !animNote.empty()) msg += " (" + animNote.substr(0, animNote.size() - 2) + ")";
    return r;
}
static std::wstring UniqueOut(const std::wstring& src) {
    std::wstring base = ModelsDir() + L"\\" + StemOf(src);
    std::wstring p = base + L".uemodel";
    for (int i = 2; FileExists(p); i++) p = base + L" (" + std::to_wstring(i) + L").uemodel";
    return p;
}
bool ModelImport(const std::wstring& src, std::wstring& outFile, std::string& msg) {
    MakeDirs(ModelsDir());
    outFile = UniqueOut(src);
    return Convert(src, outFile, {}, L"", msg);
}
static bool HeaderOf(const std::wstring& model, std::wstring& src, std::wstring& tex, std::vector<std::wstring>& psa) {
    std::string t; if (!ReadBytes(model, t)) return false;
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p); std::string l = t.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? t.size() : e + 1;
        if (l.rfind("part\t", 0) == 0) break;
        if (l.rfind("source\t", 0) == 0) src = U2W(l.substr(7));
        else if (l.rfind("texture\t", 0) == 0) tex = U2W(l.substr(8));
        else if (l.rfind("psa\t", 0) == 0) psa.push_back(U2W(l.substr(4)));
    }
    return !src.empty();
}
bool ModelAddAnims(const std::wstring& model, const std::vector<std::wstring>& more, std::string& msg) {
    std::wstring src, tex; std::vector<std::wstring> psa;
    if (!HeaderOf(model, src, tex, psa) || !FileExists(src)) { msg = "The original model file isn't there any more, so animations can't be added."; return false; }
    for (auto& m : more) if (std::find(psa.begin(), psa.end(), m) == psa.end()) psa.push_back(m);
    if (psa.empty()) psa.push_back(L"?");   // (stops the automatic search)
    return Convert(src, model, psa, tex, msg);
}
bool ModelSetTexture(const std::wstring& model, const std::wstring& image, std::string& msg) {
    std::wstring src, tex; std::vector<std::wstring> psa;
    if (!HeaderOf(model, src, tex, psa) || !FileExists(src)) { msg = "The original model file isn't there any more."; return false; }
    if (psa.empty()) psa.push_back(L"?");
    return Convert(src, model, psa, image, msg);
}
bool ModelDelete(const std::wstring& model) { DeleteFileW(SlotsFile(model).c_str()); return DeleteFileW(model.c_str()) != 0; }
std::vector<ModelInfo> ModelList() {
    std::vector<ModelInfo> out;
    for (auto& f : ListFiles(ModelsDir(), L"*.uemodel")) {
        ModelInfo mi; mi.file = ModelsDir() + L"\\" + f;
        HANDLE h = CreateFileW(mi.file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::string t(16384, '\0'); DWORD n = 0; ReadFile(h, &t[0], (DWORD)t.size(), &n, nullptr); CloseHandle(h); t.resize(n);
        size_t p = 0;
        while (p < t.size()) {
            size_t e = t.find('\n', p); if (e == std::string::npos) break;
            std::string l = t.substr(p, e - p); p = e + 1;
            if (l.rfind("part\t", 0) == 0) break;
            if (l.rfind("name\t", 0) == 0) mi.name = l.substr(5);
            else if (l.rfind("source\t", 0) == 0) mi.source = l.substr(7);
            else if (l.rfind("textured\t", 0) == 0) mi.textured = l.substr(9) == "1";
            else if (l.rfind("animname\t", 0) == 0) mi.anims.push_back(l.substr(9));
            else if (l.rfind("stats\t", 0) == 0) sscanf(l.c_str() + 6, "%d\t%d\t%d", &mi.parts, &mi.verts, &mi.tris);
            else if (l.rfind("mat\t", 0) == 0) {
                std::vector<std::string> f; size_t a = 0;
                while (true) { size_t b = l.find('\t', a); f.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
                int i = atoi(f[1].c_str());
                std::string nm = f.size() > 5 && !f[5].empty() ? f[5] : (f.size() > 2 && f[2] != "-" ? "Material " + std::to_string(i + 1) : "Colour " + std::to_string(i + 1));
                if (i >= 0 && i < 256) { if ((int)mi.slots.size() <= i) mi.slots.resize(i + 1); mi.slots[i] = nm; }
            }
        }
        mi.slotUse.assign(mi.slots.size(), "");
        std::string st;
        if (ReadBytes(SlotsFile(mi.file), st)) {
            size_t q = 0;
            while (q < st.size()) {
                size_t e = st.find('\n', q); std::string l = st.substr(q, e == std::string::npos ? std::string::npos : e - q); q = e == std::string::npos ? st.size() : e + 1;
                int i = -1; char nm[512] = {};
                if (sscanf(l.c_str(), "slot\t%d\t%511[^\t]", &i, nm) == 2 && i >= 0 && i < (int)mi.slotUse.size()) mi.slotUse[i] = nm;
            }
        }
        if (mi.name.empty()) mi.name = W2U(StemOf(f));
        out.push_back(mi);
    }
    std::sort(out.begin(), out.end(), [](const ModelInfo& a, const ModelInfo& b) { return LowerA(a.name) < LowerA(b.name); });
    return out;
}


static std::wstring FModelDir() { return ExeDir() + L"\\Tools\\FModel"; }
// ================================================================ exporting straight from a game's .pak files (Unreal 4)
// Reads the selected object's static mesh, its materials and their colour textures out of the game's packed files
// and turns them into a .uemodel. Handles pak versions 3-11 (zlib, Oodle via an Oodle library already on the PC,
// uncompressed), Unreal 4 packages with tagged properties, and DXT1/3/5, BC7 and plain RGBA colour textures.
extern "C" int puff(unsigned char* dest, unsigned long* destlen, const unsigned char* source, unsigned long* sourcelen);
#ifdef _WIN32
#include <bcrypt.h>
#endif
static std::string g_aesKey;   // 32 raw bytes, or empty
bool ModelSetAesKey(const std::string& text) {
    std::string h; for (char c : text) if (isxdigit((unsigned char)c)) h += c;
    if (h.size() >= 2 && (text.find("0x") == 0 || text.find("0X") == 0)) h = h.substr(1), h = h.substr(h[0] == 'x' || h[0] == 'X' ? 1 : 0);
    g_aesKey.clear();
    if (h.size() != 64) return text.empty();
    for (int i = 0; i < 32; i++) g_aesKey += (char)std::stoi(h.substr(i * 2, 2), nullptr, 16);
    return true;
}
// AES-256 decryption in plain code: used to try many possible keys quickly (and by the test build)
struct SoftAes {
    static uint8_t sbox[256], inv[256], m9[256], m11[256], m13[256], m14[256]; static bool ready;
    uint8_t rk[240];
    static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
    static void Init() {
        if (ready) return;
        uint8_t p = 1, q = 1;
        do {
            p = p ^ (uint8_t)(p << 1) ^ (p & 0x80 ? 0x1B : 0);
            q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;
            uint8_t x = q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^ (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4));
            sbox[p] = x ^ 0x63;
        } while (p != 1);
        sbox[0] = 0x63; for (int i = 0; i < 256; i++) inv[sbox[i]] = (uint8_t)i;
        auto mul = [](uint8_t x, uint8_t y) { uint8_t r = 0; while (y) { if (y & 1) r ^= x; x = xt(x); y >>= 1; } return r; };
        for (int i = 0; i < 256; i++) { m9[i] = mul((uint8_t)i, 9); m11[i] = mul((uint8_t)i, 11); m13[i] = mul((uint8_t)i, 13); m14[i] = mul((uint8_t)i, 14); }
        ready = true;
    }
    explicit SoftAes(const uint8_t key[32]) {
        Init();
        memcpy(rk, key, 32); uint8_t rc = 1;
        for (int i = 32; i < 240; i += 4) {
            uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
            if (i % 32 == 0) { uint8_t u = t[0]; t[0] = sbox[t[1]] ^ rc; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u]; rc = xt(rc); }
            else if (i % 32 == 16) for (auto& v : t) v = sbox[v];
            for (int k = 0; k < 4; k++) rk[i + k] = rk[i - 32 + k] ^ t[k];
        }
    }
    void Decrypt(uint8_t* st) const {
        for (int k = 0; k < 16; k++) st[k] ^= rk[224 + k];
        for (int round = 13; round >= 0; round--) {
            uint8_t t[16]; for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[((c + r) % 4) * 4 + r] = st[c * 4 + r];
            for (int k = 0; k < 16; k++) st[k] = inv[t[k]] ^ rk[round * 16 + k];
            if (round) for (int c = 0; c < 4; c++) {
                uint8_t a0 = st[c * 4], a1 = st[c * 4 + 1], a2 = st[c * 4 + 2], a3 = st[c * 4 + 3];
                st[c * 4] = m14[a0] ^ m11[a1] ^ m13[a2] ^ m9[a3]; st[c * 4 + 1] = m9[a0] ^ m14[a1] ^ m11[a2] ^ m13[a3];
                st[c * 4 + 2] = m13[a0] ^ m9[a1] ^ m14[a2] ^ m11[a3]; st[c * 4 + 3] = m11[a0] ^ m13[a1] ^ m9[a2] ^ m14[a3];
            }
        }
    }
};
uint8_t SoftAes::sbox[256], SoftAes::inv[256], SoftAes::m9[256], SoftAes::m11[256], SoftAes::m13[256], SoftAes::m14[256]; bool SoftAes::ready = false;
static bool AesDecrypt(std::string& d) {          // AES-256, ECB (how Unreal encrypts .pak files)
    if (g_aesKey.size() != 32 || d.size() % 16) return false;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0) == 0 &&
            BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, (PUCHAR)g_aesKey.data(), 32, 0) == 0) {
            ULONG got = 0;
            ok = BCryptDecrypt(key, (PUCHAR)&d[0], (ULONG)d.size(), nullptr, nullptr, 0, (PUCHAR)&d[0], (ULONG)d.size(), &got, 0) == 0;
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
#else
    SoftAes aes((const uint8_t*)g_aesKey.data());
    for (size_t i = 0; i < d.size(); i += 16) aes.Decrypt((uint8_t*)&d[i]);
    return true;
#endif
}
static size_t Align16(size_t n) { return (n + 15) & ~(size_t)15; }
namespace uex {
struct Rd {
    const std::string& b; size_t o; bool bad = false;
    Rd(const std::string& s, size_t at = 0) : b(s), o(at) {}
    template <class T> T get() { T v{}; if (o + sizeof(T) > b.size()) { bad = true; return v; } memcpy(&v, b.data() + o, sizeof(T)); o += sizeof(T); return v; }
    void skip(size_t n) { if (o + n > b.size()) bad = true; else o += n; }
    std::string fstr() {
        int n = get<int>(); if (bad || n == 0) return "";
        if (n > 0) { if (n > 65536 || o + n > b.size()) { bad = true; return ""; } std::string r(b.data() + o, n - 1); o += n; return r; }
        n = -n; if (n > 65536 || o + 2ull * n > b.size()) { bad = true; return ""; }
        std::string r; for (int i = 0; i < n - 1; i++) { uint16_t c; memcpy(&c, b.data() + o + 2 * i, 2); r += c < 128 ? (char)c : '?'; } o += 2ull * n; return r;
    }
};
struct Entry { int pak = 0; long long off = 0, size = 0, usize = 0, hdr = 0; int comp = 0; uint32_t bsz = 0; bool enc = false; std::vector<std::pair<long long, long long>> blocks; };
// ---- IoStore containers (.utoc + .ucas, Unreal 5 and some late Unreal 4 games)
struct IoBlock { long long off; uint32_t csize, usize; uint8_t method; };
struct IoContainer {
    std::wstring utoc, ucas; int version = 0; uint32_t flags = 0, blockSize = 0;
    std::vector<std::array<uint8_t, 12>> chunkIds; std::vector<std::pair<long long, long long>> offLen;   // uncompressed offset, length
    std::vector<IoBlock> blocks; std::vector<std::string> methods;
};
struct IoRef { int cont = -1; int idx = -1; };
struct Package;
struct PakSet {
    std::wstring dir; std::vector<std::wstring> paks; std::vector<std::vector<std::string>> methods; std::vector<int> versions;
    std::unordered_map<std::string, Entry> files;    // "/game/x/y.uasset" (lower case) -> entry
    std::vector<IoContainer> io; std::unordered_map<std::string, IoRef> ioFiles;   // same keys, files in IoStore containers
    // Unreal 4.26/4.27 IoStore packages: the engine's script object names (from global.utoc), and each package's
    // exports by their global import index (to work out what a package imports)
    bool scriptLoaded = false; std::unordered_map<uint64_t, std::pair<std::string, uint64_t>> scriptObjects;   // index -> name, outer
    std::unordered_map<std::string, std::vector<std::pair<uint64_t, std::string>>> zenExports;           // package key -> (global index, name)
    std::string err; bool anyEncrypted = false, badKey = false;
};
static std::string PackagePathOf(std::string full) {   // "Hk_project/Content/Map/X.umap" -> "/game/map/x.umap"
    while (full.rfind("../", 0) == 0) full = full.substr(3);
    if (!full.empty() && full[0] == '/') full = full.substr(1);
    std::string lf = LowerA(full);
    if (lf.rfind("engine/content/", 0) == 0) return "/engine/" + lf.substr(15);
    size_t pl = lf.find("/plugins/");
    if (pl != std::string::npos) { size_t c = lf.find("/content/", pl); if (c == std::string::npos) return ""; size_t ns = lf.rfind('/', c - 1); return "/" + lf.substr(ns + 1, c - ns - 1) + "/" + lf.substr(c + 9); }
    size_t c = lf.find("/content/");
    if (c == std::string::npos) return "";
    return "/game/" + lf.substr(c + 9);
}
static bool ReadAt(HANDLE h, long long off, void* buf, size_t n) {
    LARGE_INTEGER li; li.QuadPart = off;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD rd = 0; return ReadFile(h, buf, (DWORD)n, &rd, nullptr) && rd == n;
}
static Entry ReadEntry(Rd& r, int ver) {
    Entry e; e.off = r.get<long long>(); e.size = r.get<long long>(); e.usize = r.get<long long>();
    uint32_t c = r.get<uint32_t>(); e.comp = (int)c;
    if (ver == 1) r.skip(8);
    r.skip(20);
    if (ver >= 3) {
        if (c) { int n = r.get<int>(); if (n < 0 || n > 4000000) { r.bad = true; return e; } for (int i = 0; i < n; i++) { long long a = r.get<long long>(), b = r.get<long long>(); e.blocks.push_back({ a, b }); } }
        e.enc = (r.get<uint8_t>() & 1) != 0; e.bsz = r.get<uint32_t>();
    }
    e.hdr = 8 + 8 + 8 + 4 + 20 + (ver >= 3 ? 1 + 4 + (c ? 4 + 16 * (long long)e.blocks.size() : 0) : 0) + (ver == 1 ? 8 : 0);
    if (ver < 5) for (auto& b : e.blocks) { b.first -= e.off; b.second -= e.off; }   // older paks: absolute offsets
    return e;
}
static bool OpenPak(PakSet& ps, const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    std::string tail((size_t)std::min<long long>(sz.QuadPart, 1024), '\0');
    long long tailOff = sz.QuadPart - (long long)tail.size();
    bool ok = ReadAt(h, tailOff, &tail[0], tail.size());
    long long m = -1; const uint32_t magic = 0x5A6F12E1;
    for (long long i = (long long)tail.size() - 44; ok && i >= 0; i--) if (memcmp(tail.data() + i, &magic, 4) == 0) {
        uint32_t v; long long io, is; memcpy(&v, tail.data() + i + 4, 4); memcpy(&io, tail.data() + i + 8, 8); memcpy(&is, tail.data() + i + 16, 8);
        if (v >= 1 && v <= 12 && io > 0 && is > 0 && io + is <= sz.QuadPart) { m = i; break; }
    }
    if (m < 0) { CloseHandle(h); return false; }
    uint32_t ver; long long idxOff, idxSize;
    memcpy(&ver, tail.data() + m + 4, 4); memcpy(&idxOff, tail.data() + m + 8, 8); memcpy(&idxSize, tail.data() + m + 16, 8);
    std::vector<std::string> methods;
    if (ver >= 8) {
        size_t at = (size_t)m + 44 + (ver == 9 ? 1 : 0);
        for (int k = 0; k < 5 && at + 32 <= tail.size(); k++, at += 32) methods.push_back(std::string(tail.data() + at, strnlen(tail.data() + at, 32)));
    } else methods = { "Zlib", "Gzip", "Oodle" };     // (old flag bits 1, 2, 4)
    bool encIndex = ver >= 4 && m >= 1 && tail[(size_t)m - 1] != 0;
    if (encIndex) ps.anyEncrypted = true;
    if (encIndex && g_aesKey.empty()) { CloseHandle(h); return false; }
    if (idxSize > 512ll * 1024 * 1024) { CloseHandle(h); return false; }
    std::string idx(encIndex ? Align16((size_t)idxSize) : (size_t)idxSize, '\0');
    if (!ReadAt(h, idxOff, &idx[0], idx.size())) { CloseHandle(h); return false; }
    if (encIndex && !AesDecrypt(idx)) { ps.badKey = true; CloseHandle(h); return false; }
    int pakNo = (int)ps.paks.size();
    ps.paks.push_back(path); ps.methods.push_back(methods); ps.versions.push_back((int)ver);
    Rd r(idx);
    std::string mount = r.fstr(); int num = r.get<int>();
    if (r.bad || num < 0 || mount.find("../") == std::string::npos && mount.find('/') == std::string::npos) { if (encIndex) ps.badKey = true; CloseHandle(h); return false; }
    auto add = [&](const std::string& full, Entry e) {
        std::string k = PackagePathOf(full); if (k.empty()) return;
        e.pak = pakNo;
        if (ver < 8 && e.comp) e.comp = (e.comp & 1) ? 1 : (e.comp & 4) ? 3 : 2;   // old flags -> method number
        if (e.enc) ps.anyEncrypted = true;
        ps.files[k] = e;                                 // later paks (patches) win
    };
    if (ver >= 10) {
        r.get<unsigned long long>();
        if (r.get<uint32_t>()) r.skip(8 + 8 + 20);
        if (!r.get<uint32_t>()) { CloseHandle(h); return false; }
        long long fdOff = r.get<long long>(), fdSize = r.get<long long>(); r.skip(20);
        int encSize = r.get<int>();
        if (r.bad || encSize < 0 || r.o + (size_t)encSize > idx.size()) { CloseHandle(h); return false; }
        std::string enc = idx.substr(r.o, encSize); r.skip(encSize);
        int nUnenc = r.get<int>();
        std::vector<Entry> unenc;
        for (int i = 0; i < nUnenc && !r.bad; i++) unenc.push_back(ReadEntry(r, (int)ver));
        if (fdSize <= 0 || fdSize > 512ll * 1024 * 1024) { CloseHandle(h); return false; }
        std::string fd(encIndex ? Align16((size_t)fdSize) : (size_t)fdSize, '\0');
        if (!ReadAt(h, fdOff, &fd[0], fd.size())) { CloseHandle(h); return false; }
        if (encIndex && !AesDecrypt(fd)) { ps.badKey = true; CloseHandle(h); return false; }
        Rd d(fd);
        int dirs = d.get<int>();
        for (int i = 0; i < dirs && !d.bad; i++) {
            std::string dir = d.fstr(); int n = d.get<int>();
            for (int j = 0; j < n && !d.bad; j++) {
                std::string fn = d.fstr(); int e = d.get<int>();
                Entry en;
                if (e >= 0 && (size_t)e + 4 <= enc.size()) {
                    Rd q(enc, (size_t)e); uint32_t v = q.get<uint32_t>();
                    en.bsz = (v & 0x3f) == 0x3f ? q.get<uint32_t>() : (v & 0x3f) << 11;
                    en.comp = (v >> 23) & 0x3f; en.enc = ((v >> 22) & 1) != 0; int nb = (v >> 6) & 0xffff;
                    en.off = (v & (1u << 31)) ? q.get<uint32_t>() : q.get<long long>();
                    en.usize = (v & (1u << 30)) ? q.get<uint32_t>() : q.get<long long>();
                    en.size = en.comp ? ((v & (1u << 29)) ? q.get<uint32_t>() : q.get<long long>()) : en.usize;
                    en.hdr = 8 + 8 + 8 + 4 + 20 + 1 + 4 + (en.comp ? 4 + 16ll * nb : 0);
                    if (nb == 1 && !en.enc) en.blocks.push_back({ en.hdr, en.hdr + en.size });
                    else { long long o = en.hdr; for (int k = 0; k < nb; k++) { uint32_t bs = q.get<uint32_t>(); en.blocks.push_back({ o, o + bs }); o += bs; if (en.enc) o = (o + 15) & ~15ll; } }
                } else if (e < 0 && -e - 1 < (int)unenc.size()) en = unenc[-e - 1];
                else continue;
                add(mount + dir + fn, en);
            }
        }
    } else {
        for (int i = 0; i < num && !r.bad; i++) { std::string fn = r.fstr(); Entry en = ReadEntry(r, (int)ver); if (!r.bad) add(mount + fn, en); }
    }
    CloseHandle(h);
    return true;
}
typedef intptr_t (*OodleFn)(const void*, intptr_t, void*, intptr_t, int, int, int, void*, intptr_t, void*, void*, void*, intptr_t, int);
static OodleFn g_oodle = nullptr; static bool g_oodleTried = false;
// No Oodle library on the PC (games like Stray build it into their exe): fetch the one FModel uses - Epic's Oodle
// for Unreal, built by github.com/WorkingRobot/OodleUE - and keep just its decompression DLL next to the launcher.
static bool DownloadOodle(std::wstring& dllOut) {
    std::wstring dir = ExeDir() + L"\\oodle";
    std::string zip, err;
    if (!NetGet(L"https://github.com/WorkingRobot/OodleUE/releases/latest/download/msvc-x64-release.zip", zip, err) || zip.size() < 1000) return false;
    wchar_t tmpBase[MAX_PATH]; GetTempPathW(MAX_PATH, tmpBase);
    std::wstring tmp = std::wstring(tmpBase) + L"UE4SS-Launcher-oodle";
    DeleteFolderTree(tmp);
    if (!ZipExtract(zip, tmp)) { DeleteFolderTree(tmp); return false; }
    std::string dll; bool got = ReadBytes(tmp + L"\\bin\\oodle-data-shared.dll", dll);
    DeleteFolderTree(tmp);
    if (!got || dll.size() < 10000) return false;
    MakeDirs(dir);
    dllOut = dir + L"\\oodle-data-shared.dll";
    if (!WriteBytes(dllOut, dll)) {      // (the launcher's folder isn't writable: keep it in the temp folder)
        dir = std::wstring(tmpBase) + L"UE4SS-Launcher-oodle-dll"; MakeDirs(dir);
        dllOut = dir + L"\\oodle-data-shared.dll";
        if (!WriteBytes(dllOut, dll)) return false;
    }
    return true;
}
static std::mutex g_oodleMx;
static bool LoadOodle(const std::wstring& paksDir) {
    std::lock_guard<std::mutex> lk(g_oodleMx);
    static ULONGLONG s_failAt = 0;
    if (g_oodle) return true;
    if (g_oodleTried && GetTickCount64() - s_failAt < 60000) return false;   // (a failed search/download is retried after a minute)
    g_oodleTried = true;
    std::vector<std::wstring> dirs;
    std::wstring root = paksDir;   // <Game>\Content\Paks -> <Game> -> its Binaries, and the folder above (Engine)
    for (int i = 0; i < 3; i++) root = DirOf(root);
    dirs.push_back(DirOf(paksDir) + L"\\..\\Binaries\\Win64");
    dirs.push_back(root + L"\\Engine\\Binaries\\ThirdParty\\Oodle\\Win64");
    std::wstring fm = FModelExports();
    dirs.push_back(DirOf(fm) + L"\\.data"); dirs.push_back(FModelDir()); dirs.push_back(FModelDir() + L"\\Output\\.data"); dirs.push_back(ExeDir());
    dirs.push_back(ExeDir() + L"\\oodle");
    { wchar_t tb[MAX_PATH]; GetTempPathW(MAX_PATH, tb); dirs.push_back(std::wstring(tb) + L"UE4SS-Launcher-oodle-dll"); }
    for (auto& d : dirs) for (auto pat : { L"oo2core*win64*.dll", L"oodle-data-shared*.dll", L"oo2core*.dll" }) for (auto& f : ListFiles(d, pat)) {
        HMODULE m = LoadLibraryW((d + L"\\" + f).c_str());
        if (!m) continue;
        g_oodle = (OodleFn)GetProcAddress(m, "OodleLZ_Decompress");
        if (g_oodle) return true;
    }
    std::wstring dl;
    if (DownloadOodle(dl)) {
        if (HMODULE m = LoadLibraryW(dl.c_str())) g_oodle = (OodleFn)GetProcAddress(m, "OodleLZ_Decompress");
        if (g_oodle) return true;
    }
    s_failAt = GetTickCount64();
    return false;
}
static uint64_t BE40(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 5; i++) v = (v << 8) | p[i]; return v; }
static bool OpenIoStore(PakSet& ps, const std::wstring& utoc) {
    std::string t; if (!ReadBytes(utoc, t) || t.size() < 144 || t.compare(0, 16, "-==--==--==--==-") != 0) return false;
    IoContainer c; c.utoc = utoc; c.ucas = utoc.substr(0, utoc.size() - 5) + L".ucas";
    Rd r(t, 16);
    c.version = r.get<uint8_t>(); r.skip(3);
    uint32_t hdrSize = r.get<uint32_t>(), entries = r.get<uint32_t>(), nBlocks = r.get<uint32_t>(), blockEntrySize = r.get<uint32_t>();
    uint32_t nMethods = r.get<uint32_t>(), methodLen = r.get<uint32_t>(); c.blockSize = r.get<uint32_t>();
    uint32_t dirSize = r.get<uint32_t>(); r.get<uint32_t>(); /*partitions*/ r.get<uint64_t>(); /*container id*/ r.skip(16); /*key guid*/
    c.flags = r.get<uint8_t>(); r.skip(3);
    uint32_t seeds = c.version >= 4 ? r.get<uint32_t>() : 0; if (c.version >= 3) r.get<uint64_t>();
    uint32_t noHash = c.version >= 5 ? r.get<uint32_t>() : 0;
    if (r.bad || blockEntrySize != 12 || entries > 20000000 || nBlocks > 100000000) return false;
    size_t o = hdrSize;
    auto need = [&](size_t n) { return o + n <= t.size(); };
    if (!need((size_t)entries * 22)) return false;
    c.chunkIds.resize(entries); for (uint32_t i = 0; i < entries; i++) memcpy(c.chunkIds[i].data(), t.data() + o + 12ull * i, 12);
    o += 12ull * entries;
    c.offLen.resize(entries);
    for (uint32_t i = 0; i < entries; i++) { const uint8_t* p = (const uint8_t*)t.data() + o + 10ull * i; c.offLen[i] = { (long long)BE40(p), (long long)BE40(p + 5) }; }
    o += 10ull * entries;
    o += 4ull * seeds + 4ull * noHash;
    if (!need(12ull * nBlocks)) return false;
    c.blocks.resize(nBlocks);
    for (uint32_t i = 0; i < nBlocks; i++) {
        const uint8_t* p = (const uint8_t*)t.data() + o + 12ull * i; IoBlock b;
        b.off = (long long)(p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32);
        b.csize = p[5] | p[6] << 8 | p[7] << 16; b.usize = p[8] | p[9] << 8 | p[10] << 16; b.method = p[11];
        c.blocks[i] = b;
    }
    o += 12ull * nBlocks;
    for (uint32_t i = 0; i < nMethods && need(methodLen); i++) { std::string m(t.data() + o, methodLen); m = m.c_str(); c.methods.push_back(m); o += methodLen; }
    if (c.flags & 4) {   // signed
        if (!need(4)) return false; uint32_t hs; memcpy(&hs, t.data() + o, 4); o += 4 + 2ull * hs + 20ull * nBlocks;
    }
    if (c.flags & 2) ps.anyEncrypted = true;
    if (!(c.flags & 8) || dirSize == 0 || !need(dirSize)) { ps.io.push_back(std::move(c)); return false; }   // no file names (global.utoc)
    std::string dir = t.substr(o, dirSize);
    if (getenv("UEX_DEBUG")) fprintf(stderr, "utoc v%d entries %u blocks %u methods %u bs %u dir %u flags %u seeds %u nohash %u dirAt %zu size %zu\n", c.version, entries, nBlocks, nMethods, c.blockSize, dirSize, c.flags, seeds, noHash, o, t.size());
    if (c.flags & 2) {
        if (g_aesKey.empty()) return false;
        if (dir.size() % 16 || !AesDecrypt(dir)) { ps.badKey = true; return false; }
    }
    if (getenv("UEX_DEBUG")) { fprintf(stderr, "dir: "); for (int i = 0; i < 48 && i < (int)dir.size(); i++) fprintf(stderr, "%02x", (uint8_t)dir[i]); fprintf(stderr, "\n"); }
    Rd d(dir);
    std::string mount = d.fstr();
    int nd = d.get<int>(); if (d.bad || nd < 0 || nd > 10000000) { if (c.flags & 2) ps.badKey = true; return false; }
    struct DE { uint32_t name, child, sib, file; }; std::vector<DE> des(nd);
    for (int i = 0; i < nd; i++) { des[i].name = d.get<uint32_t>(); des[i].child = d.get<uint32_t>(); des[i].sib = d.get<uint32_t>(); des[i].file = d.get<uint32_t>(); }
    int nf = d.get<int>(); if (d.bad || nf < 0 || nf > 20000000) return false;
    struct FE { uint32_t name, next, data; }; std::vector<FE> fes(nf);
    for (int i = 0; i < nf; i++) { fes[i].name = d.get<uint32_t>(); fes[i].next = d.get<uint32_t>(); fes[i].data = d.get<uint32_t>(); }
    int ns = d.get<int>(); if (d.bad || ns < 0 || ns > 20000000) return false;
    std::vector<std::string> strs(ns); for (int i = 0; i < ns && !d.bad; i++) strs[i] = d.fstr();
    if (d.bad) return false;
    int ci = (int)ps.io.size();
    // walk the tree: directory names joined with '/', files under each
    std::vector<std::pair<uint32_t, std::string>> stack; if (nd) stack.push_back({ 0, "" });
    int guard = 0;
    while (!stack.empty() && guard++ < 20000000) {
        auto [di, path] = stack.back(); stack.pop_back();
        if (di >= (uint32_t)nd) continue;
        const DE& e = des[di];
        std::string here = path;
        if (e.name != 0xFFFFFFFF && e.name < (uint32_t)ns) here += strs[e.name] + "/";
        for (uint32_t f = e.file; f != 0xFFFFFFFF && f < (uint32_t)nf; f = fes[f].next) {
            if (fes[f].name >= (uint32_t)ns || fes[f].data >= entries) continue;
            std::string key = PackagePathOf(mount + here + strs[fes[f].name]);
            if (!key.empty()) ps.ioFiles[key] = { ci, (int)fes[f].data };
        }
        if (e.sib != 0xFFFFFFFF) stack.push_back({ e.sib, path });
        if (e.child != 0xFFFFFFFF) stack.push_back({ e.child, here });
    }
    ps.io.push_back(std::move(c));
    return true;
}
static bool Unpack(const std::string& method, const std::string& in, char* out, size_t us, const std::wstring& gameDir, std::string& err);
// While scanning a whole game, each thread keeps the game's files open instead of opening them for every read
struct HandleCache { std::map<std::wstring, HANDLE> h; ~HandleCache() { for (auto& kv : h) if (kv.second != INVALID_HANDLE_VALUE) CloseHandle(kv.second); } };
static thread_local HandleCache* t_handles = nullptr;
static HANDLE OpenRead(const std::wstring& f) {
    if (t_handles) { auto it = t_handles->h.find(f); if (it != t_handles->h.end()) return it->second; }
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (t_handles && h != INVALID_HANDLE_VALUE) t_handles->h[f] = h;
    return h;
}
static void DoneRead(HANDLE h) { if (!t_handles) CloseHandle(h); }
static bool ReadIoChunk(PakSet& ps, const IoRef& ref, std::string& out, std::string& err, long long want = -1) {
    const IoContainer& c = ps.io[ref.cont];
    long long off = c.offLen[ref.idx].first, len = c.offLen[ref.idx].second;
    if (want >= 0 && want < len) len = want;
    HANDLE h = OpenRead(c.ucas);
    if (h == INVALID_HANDLE_VALUE) { err = "couldn't open the game's .ucas file"; return false; }
    out.clear(); out.reserve((size_t)len);
    long long first = off / c.blockSize, last = (off + len - 1) / c.blockSize;
    bool ok = true;
    for (long long bi = first; bi <= last && ok; bi++) {
        if (bi >= (long long)c.blocks.size()) { ok = false; err = "file outside the container"; break; }
        const IoBlock& b = c.blocks[(size_t)bi];
        std::string cb((c.flags & 2) ? Align16(b.csize) : b.csize, '\0');
        if (!ReadAt(h, b.off, &cb[0], cb.size())) { ok = false; err = "couldn't read the game's .ucas file"; break; }
        if (c.flags & 2) { if (!AesDecrypt(cb)) { ok = false; err = "couldn't decrypt (wrong AES key?)"; break; } }
        std::string ub;
        if (b.method == 0) ub = cb.substr(0, b.usize);
        else {
            ub.resize(b.usize);
            std::string m = b.method - 1 < (int)c.methods.size() ? c.methods[b.method - 1] : "";
            cb.resize(b.csize);
            if (!Unpack(m, cb, &ub[0], b.usize, ps.dir, err)) { ok = false; break; }
        }
        long long blockStart = bi * (long long)c.blockSize;
        long long a = std::max(off, blockStart) - blockStart, z = std::min(off + len, blockStart + (long long)b.usize) - blockStart;
        if (z > a) out.append(ub.data() + a, (size_t)(z - a));
    }
    DoneRead(h);
    return ok;
}
static bool Unpack(const std::string& method, const std::string& cb, char* out, size_t us, const std::wstring& gameDir, std::string& err) {
    std::string m = LowerA(method);
    if (m == "zlib") {
        unsigned long dl = (unsigned long)us, sl = (unsigned long)cb.size() - 2;
        if (cb.size() < 3 || puff((unsigned char*)out, &dl, (const unsigned char*)cb.data() + 2, &sl) != 0 || dl != us) { err = "couldn't unpack (zlib)"; return false; }
        return true;
    }
    if (m == "oodle") {
        if (!LoadOodle(gameDir)) { err = "this game uses Oodle compression and no Oodle library was found and it couldn't be downloaded (check the internet connection, or put oo2core_9_win64.dll or oodle-data-shared.dll next to the launcher)"; return false; }
        intptr_t r = g_oodle(cb.data(), (intptr_t)cb.size(), out, (intptr_t)us, 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
        if (r != (intptr_t)us) { err = "couldn't unpack (Oodle)"; return false; }
        return true;
    }
    err = "unsupported compression (" + method + ")"; return false;
}
static bool ReadFileFromPak(PakSet& ps, const std::string& key, std::string& out, std::string& err, long long want = -1) {
    auto it = ps.files.find(key);
    if (it == ps.files.end()) { auto io = ps.ioFiles.find(key); if (io == ps.ioFiles.end()) return false; return ReadIoChunk(ps, io->second, out, err, want); }
    const Entry& e = it->second;
    if (e.enc && g_aesKey.empty()) { err = "the game's files are encrypted - enter the game's AES key on the launcher's Models page"; return false; }
    HANDLE h = OpenRead(ps.paks[e.pak]);
    if (h == INVALID_HANDLE_VALUE) { err = "couldn't open the game's .pak file"; return false; }
    out.clear(); out.reserve((size_t)e.usize);
    bool ok = true;
    if (!e.comp) {
        out.resize(e.enc ? Align16((size_t)e.usize) : (size_t)e.usize); ok = ReadAt(h, e.off + e.hdr, &out[0], out.size());
        if (ok && e.enc) { ok = AesDecrypt(out); out.resize((size_t)e.usize); if (!ok) err = "couldn't decrypt (wrong AES key?)"; }
    }
    else {
        std::string method = e.comp - 1 < (int)ps.methods[e.pak].size() ? ps.methods[e.pak][e.comp - 1] : "";
        long long rem = e.usize;
        for (auto& b : e.blocks) {
            size_t clen = (size_t)(b.second - b.first);
            std::string cb(e.enc ? Align16(clen) : clen, '\0');
            if (!ReadAt(h, e.off + b.first, &cb[0], cb.size())) { ok = false; break; }
            if (e.enc) { if (!AesDecrypt(cb)) { ok = false; err = "couldn't decrypt (wrong AES key?)"; break; } cb.resize(clen); }
            size_t us = (size_t)std::min<long long>(e.bsz ? (long long)e.bsz : rem, rem);
            size_t at = out.size(); out.resize(at + us);
            if (LowerA(method) == "zlib") {
                unsigned long dl = (unsigned long)us, sl = (unsigned long)cb.size() - 2;
                if (cb.size() < 3 || puff((unsigned char*)&out[at], &dl, (const unsigned char*)cb.data() + 2, &sl) != 0 || dl != us) { ok = false; err = "couldn't unpack (zlib)"; break; }
            } else if (LowerA(method) == "oodle") {
                if (!LoadOodle(ps.dir)) { ok = false; err = "this game uses Oodle compression and no Oodle library was found and it couldn't be downloaded (check the internet connection, or put oo2core_9_win64.dll or oodle-data-shared.dll next to the launcher)"; break; }
                intptr_t r = g_oodle(cb.data(), (intptr_t)cb.size(), &out[at], (intptr_t)us, 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
                if (r != (intptr_t)us) { ok = false; err = "couldn't unpack (Oodle)"; break; }
            } else { ok = false; err = "unsupported compression (" + method + ")"; break; }
            rem -= us;
        }
    }
    DoneRead(h);
    return ok;
}
static bool OpenPakSetAt(PakSet& cache, const std::wstring& paksDir, std::string& err) {
    cache = PakSet(); cache.dir = paksDir;
    std::vector<std::wstring> paks; int budget = 4000;
    Walk(paksDir, 2, [&](const std::wstring& p) { if (ExtLower(p) == L".pak") paks.push_back(p); }, budget);
    std::sort(paks.begin(), paks.end(), [](const std::wstring& a, const std::wstring& b) {   // patches (_P) last so they win
        bool pa = Lower(a).find(L"_p.pak") != std::wstring::npos, pb = Lower(b).find(L"_p.pak") != std::wstring::npos;
        return pa != pb ? pb : Lower(a) < Lower(b); });
    for (auto& p : paks) OpenPak(cache, p);
    std::vector<std::wstring> tocs; int b3 = 4000;
    Walk(paksDir, 2, [&](const std::wstring& p) { if (ExtLower(p) == L".utoc") tocs.push_back(p); }, b3);
    std::sort(tocs.begin(), tocs.end(), [](const std::wstring& a, const std::wstring& b) {
        bool pa = Lower(a).find(L"_p.utoc") != std::wstring::npos, pb = Lower(b).find(L"_p.utoc") != std::wstring::npos;
        return pa != pb ? pb : Lower(a) < Lower(b); });
    for (auto& p : tocs) OpenIoStore(cache, p);
    if (cache.files.empty() && cache.ioFiles.empty()) {
        bool ioStore = false; int b2 = 200; Walk(paksDir, 2, [&](const std::wstring& p) { if (ExtLower(p) == L".utoc") ioStore = true; }, b2);
        err = cache.badKey ? "the AES key doesn't open this game's files (check the key on the Models page)"
            : cache.anyEncrypted ? "the game's files are encrypted - enter the game's AES key on the launcher's Models page (FModel and modding sites list keys for most games)"
            : ioStore ? "this game keeps its files in the newer IoStore format (.utoc/.ucas, used by most Unreal 5 games), which the built-in exporter can't read yet - use FModel for this game"
            : "couldn't read the game's .pak files";
        return false;
    }
    return true;
}
static PakSet* OpenGame(const std::wstring& paksDir, std::string& err) {
    static PakSet cache; static ULONGLONG when = 0; static std::string keyUsed;
    if (Lower(cache.dir) == Lower(paksDir) && (!cache.files.empty() || !cache.ioFiles.empty()) && keyUsed == g_aesKey && GetTickCount64() - when < 600000) return &cache;
    keyUsed = g_aesKey;
    if (!OpenPakSetAt(cache, paksDir, err)) return nullptr;
    when = GetTickCount64();
    return &cache;
}

// ---- finding a game's AES key in the running game
// The key is built into the game's program. While the game runs, its program is in memory (also for Xbox app games,
// whose files on disk are scrambled), so the launcher reads it, collects everything that could be a key and tries each
// on a block of the game's encrypted files: the right key turns it into the start of a folder path ("../../../").
struct KeySample { int kind = 0; std::string data; size_t usize = 0, clen = 0; std::string method; std::wstring dir; };   // 0 folder path, 1 package start, 2 compressed block
static void KeySamples(const std::wstring& paksDir, std::vector<KeySample>& out) {
    int budget = 4000;
    Walk(paksDir, 2, [&](const std::wstring& p) {
        if (out.size() >= 4) return;
        std::wstring ext = ExtLower(p);
        if (ext == L".pak") {
            HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) return;
            LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
            std::string tail((size_t)std::min<long long>(sz.QuadPart, 1024), '\0');
            if (ReadAt(h, sz.QuadPart - (long long)tail.size(), &tail[0], tail.size())) {
                const uint32_t magic = 0x5A6F12E1;
                for (long long i = (long long)tail.size() - 44; i >= 1; i--) if (memcmp(tail.data() + i, &magic, 4) == 0) {
                    long long io; memcpy(&io, tail.data() + i + 8, 8);
                    KeySample ks; ks.data.resize(16);
                    if (tail[i - 1] == 1 && io > 0 && ReadAt(h, io, &ks.data[0], 16)) out.push_back(ks);
                    else if (tail[i - 1] == 0) {
                        // the index is readable: take an encrypted file instead
                        PakSet tmp; tmp.dir = paksDir;
                        CloseHandle(h); h = INVALID_HANDLE_VALUE;
                        if (OpenPak(tmp, p)) {
                            int got = 0;
                            for (int pass = 0; pass < 2 && got < 2; pass++) for (auto& f : tmp.files) {
                                const Entry& e = f.second; if (!e.enc || got >= 2) continue;
                                HANDLE h2 = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                                if (h2 == INVALID_HANDLE_VALUE) break;
                                KeySample k2;
                                bool pkg = f.first.size() > 7 && (f.first.compare(f.first.size() - 7, 7, ".uasset") == 0 || f.first.compare(f.first.size() - 5, 5, ".umap") == 0);
                                if (pass == 0 && !e.comp && pkg) { k2.kind = 1; k2.data.resize(16); if (ReadAt(h2, e.off + e.hdr, &k2.data[0], 16)) { out.push_back(k2); got++; } }
                                else if (pass == 1 && e.comp && !e.blocks.empty()) {
                                    size_t clen = (size_t)(e.blocks[0].second - e.blocks[0].first);
                                    if (clen > 0 && clen < (1 << 20)) {
                                        k2.kind = 2; k2.data.resize(Align16(clen)); k2.usize = (size_t)std::min<long long>(e.bsz ? (long long)e.bsz : e.usize, e.usize);
                                        k2.method = e.comp - 1 < (int)tmp.methods[0].size() ? tmp.methods[0][e.comp - 1] : "";
                                        k2.clen = clen; k2.dir = paksDir;
                                        if (ReadAt(h2, e.off + e.blocks[0].first, &k2.data[0], k2.data.size())) { out.push_back(k2); got++; }
                                    }
                                }
                                CloseHandle(h2);
                            }
                        }
                    }
                    break;
                }
            }
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        } else if (ext == L".utoc") {
            std::string t; if (!ReadBytes(p, t) || t.size() < 144 || t.compare(0, 16, "-==--==--==--==-") != 0) return;
            Rd r(t, 16); int ver = r.get<uint8_t>(); r.skip(3);
            uint32_t hs = r.get<uint32_t>(), n = r.get<uint32_t>(), nb = r.get<uint32_t>(); r.get<uint32_t>();
            uint32_t nm = r.get<uint32_t>(), ml = r.get<uint32_t>(); r.get<uint32_t>(); uint32_t ds = r.get<uint32_t>();
            r.get<uint32_t>(); r.get<uint64_t>(); r.skip(16); uint8_t fl = r.get<uint8_t>(); r.skip(3);
            uint32_t seeds = ver >= 4 ? r.get<uint32_t>() : 0; if (ver >= 3) r.get<uint64_t>(); uint32_t nh = ver >= 5 ? r.get<uint32_t>() : 0;
            if (r.bad || !(fl & 2) || !(fl & 8) || ds < 16) return;
            size_t o = hs + 22ull * n + 4ull * seeds + 4ull * nh + 12ull * nb + (size_t)nm * ml;
            if (fl & 4) { if (o + 4 > t.size()) return; uint32_t hsz; memcpy(&hsz, t.data() + o, 4); o += 4 + 2ull * hsz + 20ull * nb; }
            if (o + 16 > t.size()) return;
            KeySample ks; ks.data = t.substr(o, 16); out.push_back(ks);
        }
    }, budget);
}
static bool LooksLikeMount(const uint8_t* p) {          // FString: length, then text such as "../../../"
    int32_t n; memcpy(&n, p, 4);
    if (n >= 2 && n <= 1024) { for (int i = 4; i < 16 && i < 4 + n - 1; i++) if (p[i] < 0x20 || p[i] > 0x7E) return false; return p[4] == '.' || p[4] == '/' || isalnum(p[4]); }
    if (n <= -2 && n >= -1024) { for (int i = 4; i + 1 < 16 && i < 4 + 2 * (-n - 1); i += 2) if (p[i] < 0x20 || p[i] > 0x7E || p[i + 1]) return false; return true; }
    return false;
}
static bool KeyOpens(const uint8_t* key, const std::vector<KeySample>& samples) {
    SoftAes a(key);
    // quick checks on the first 16 bytes of every sample, then the slow check (unpacking) for compressed blocks
    for (auto& sm : samples) {
        uint8_t b[16]; memcpy(b, sm.data.data(), 16); a.Decrypt(b);
        if (sm.kind == 0 && !LooksLikeMount(b)) return false;
        if (sm.kind == 1 && !(b[0] == 0xC1 && b[1] == 0x83 && b[2] == 0x2A && b[3] == 0x9E)) return false;
        if (sm.kind == 2) {
            std::string m = LowerA(sm.method);
            if (m == "zlib" && !(b[0] == 0x78 && (b[1] == 0x01 || b[1] == 0x5E || b[1] == 0x9C || b[1] == 0xDA))) return false;
            if (m == "oodle" && !((b[0] & 0x0F) == 0x0C)) return false;
        }
    }
    for (auto& sm : samples) if (sm.kind == 2) {
        std::string d = sm.data; for (size_t i = 0; i + 16 <= d.size(); i += 16) a.Decrypt((uint8_t*)&d[i]);
        std::string out(sm.usize, '\0'), err;
        d.resize(sm.clen);
        if (!Unpack(sm.method, d, &out[0], sm.usize, sm.dir, err)) return false;
    }
    return !samples.empty();
}
static std::string KeyHex(const uint8_t* k) { std::string h = "0x"; char b[3]; for (int i = 0; i < 32; i++) { snprintf(b, 3, "%02X", k[i]); h += b; } return h; }
static bool ScanImageForKey(const std::string& img, const std::vector<KeySample>& samples, std::string& keyHex, std::string& msg);
static bool FindAesKey(unsigned long pid, const std::wstring& paksDir, std::string& keyHex, std::string& msg) {
    keyHex.clear();
    std::vector<KeySample> samples; KeySamples(paksDir, samples);
    if (samples.empty()) { msg = "This game's files aren't encrypted: no key needed."; return true; }
    if (g_aesKey.size() == 32 && KeyOpens((const uint8_t*)g_aesKey.data(), samples)) { keyHex = KeyHex((const uint8_t*)g_aesKey.data()); msg = "The saved key is right."; return true; }
#ifndef _WIN32
    (void)pid; msg = "not available here"; return false;
#else
    // the game's program in memory
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) { msg = "Couldn't look inside the running game to find its key (Windows refused; anti-cheat can do this)."; return false; }
    uintptr_t base = 0; size_t size = 0;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        MODULEENTRY32W me{}; me.dwSize = sizeof me;
        if (snap != INVALID_HANDLE_VALUE) { if (Module32FirstW(snap, &me)) { base = (uintptr_t)me.modBaseAddr; size = me.modBaseSize; } CloseHandle(snap); }
    }
    if (!base || size < 4096 || size > 2000000000) { CloseHandle(h); msg = "Couldn't find the game's program in memory."; return false; }
    std::string img(size, '\0');
    for (size_t off = 0; off < size;) {
        MEMORY_BASIC_INFORMATION mb{};
        if (!VirtualQueryEx(h, (LPCVOID)(base + off), &mb, sizeof mb)) break;
        size_t regEnd = std::min(size, (size_t)((uintptr_t)mb.BaseAddress + mb.RegionSize - base));
        if (mb.State == MEM_COMMIT && !(mb.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (size_t o = off; o < regEnd; o += 1 << 20) { SIZE_T got = 0; ReadProcessMemory(h, (LPCVOID)(base + o), &img[o], std::min<size_t>(1 << 20, regEnd - o), &got); }
        }
        if (regEnd <= off) break;
        off = regEnd;
    }
    CloseHandle(h);
    return ScanImageForKey(img, samples, keyHex, msg);
#endif
}
// searches a program image (laid out as in memory) for the key that opens the samples
static bool ScanImageForKey(const std::string& img, const std::vector<KeySample>& samples, std::string& keyHex, std::string& msg) {
    size_t size = img.size();
    // sections: code is searched for instructions that build a key, data for a key stored as-is
    struct Sec { size_t a, z; bool code; }; std::vector<Sec> secs;
    if (img.size() > 0x400 && img[0] == 'M' && img[1] == 'Z') {
        int32_t pe; memcpy(&pe, img.data() + 0x3C, 4);
        if (pe > 0 && (size_t)pe + 24 < img.size() && memcmp(img.data() + pe, "PE\0\0", 4) == 0) {
            uint16_t ns, opt; memcpy(&ns, img.data() + pe + 6, 2); memcpy(&opt, img.data() + pe + 20, 2);
            size_t sh = (size_t)pe + 24 + opt;
            for (int i = 0; i < ns && sh + 40ull * (i + 1) <= img.size(); i++) {
                const char* e = img.data() + sh + 40ull * i; uint32_t vs, va, ch; memcpy(&vs, e + 8, 4); memcpy(&va, e + 12, 4); memcpy(&ch, e + 36, 4);
                if (va < size) secs.push_back({ va, std::min(size, (size_t)va + vs), (ch & 0x20000020) != 0 });
            }
        }
    }
    if (secs.empty()) secs.push_back({ 0, size, true });
    const uint8_t* b = (const uint8_t*)img.data();
    std::vector<std::array<uint8_t, 32>> cands;
    auto add = [&](const uint8_t* k) { std::array<uint8_t, 32> a; memcpy(a.data(), k, 32); cands.push_back(a); };
    for (auto& sc : secs) if (sc.code) {
        // mov dword [reg+disp], imm32  x8 (the usual way a key is put together)
        struct St { size_t pos; uint32_t reg; int32_t disp; uint32_t imm; }; std::vector<St> st;
        for (size_t i = sc.a + 1; i + 11 < sc.z; i++) {
            if (b[i] != 0xC7 || ((b[i + 1] >> 3) & 7)) continue;
            uint8_t m = b[i + 1], mod = m >> 6, rm = m & 7; uint32_t reg = ((b[i - 1] & 0xF0) == 0x40 ? b[i - 1] : 0) << 16 | rm; int32_t disp = 0; size_t im;
            if (mod == 0 && rm != 4 && rm != 5) im = i + 2;
            else if (mod == 1 && rm != 4) { disp = (int8_t)b[i + 2]; im = i + 3; }
            else if (mod == 1 && rm == 4) { reg |= (uint32_t)b[i + 2] << 8; disp = (int8_t)b[i + 3]; im = i + 4; }
            else if (mod == 2 && rm != 4) { memcpy(&disp, b + i + 2, 4); im = i + 6; }
            else if (mod == 2 && rm == 4) { reg |= (uint32_t)b[i + 2] << 8; memcpy(&disp, b + i + 3, 4); im = i + 7; }
            else continue;
            St s2; s2.pos = i; s2.reg = reg; s2.disp = disp; memcpy(&s2.imm, b + im, 4); st.push_back(s2);
        }
        for (size_t i = 0; i < st.size(); i++) {
            uint32_t k[8]; int got = 1; k[0] = st[i].imm;
            for (int want = 1; want < 8; want++) {
                bool f = false;
                for (size_t j = i + 1; j < st.size() && st[j].pos < st[i].pos + 400; j++)
                    if (st[j].reg == st[i].reg && st[j].disp == st[i].disp + 4 * want) { k[want] = st[j].imm; f = true; break; }
                if (!f) break; got++;
            }
            if (got == 8) add((const uint8_t*)k);
        }
        // movaps / movups / movdqa / movdqu xmm, [rip+x]: a key loaded as two 16-byte halves
        std::vector<std::pair<size_t, size_t>> ld;
        for (size_t i = sc.a; i + 7 < sc.z; i++)
            if (b[i] == 0x0F && (b[i + 1] == 0x28 || b[i + 1] == 0x10 || b[i + 1] == 0x6F) && (b[i + 2] & 0xC7) == 5) {
                int32_t rel; memcpy(&rel, b + i + 3, 4); long long t = (long long)i + 7 + rel;
                if (t >= 0 && (size_t)t + 32 <= size) ld.push_back({ i, (size_t)t });
            }
        for (size_t i = 0; i < ld.size(); i++) {
            add(b + ld[i].second);
            for (size_t j = i + 1; j < ld.size() && ld[j].first < ld[i].first + 80; j++) { uint8_t k[32]; memcpy(k, b + ld[i].second, 16); memcpy(k + 16, b + ld[j].second, 16); add(k); }
        }
    }
    for (auto& c : cands) if (KeyOpens(c.data(), samples)) { keyHex = KeyHex(c.data()); msg = "Found the game's AES key."; return true; }
    // a key stored as-is anywhere in the program's data (slower: every position that looks random enough)
    std::vector<Sec> data; for (auto& sc : secs) if (!sc.code) data.push_back(sc);
    if (data.empty()) data = secs;
    std::atomic<bool> found{ false }; std::mutex mx; std::string foundKey;
    unsigned nt = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    for (auto& sc : data) {
        if (found) break;
        size_t len = sc.z > sc.a + 32 ? sc.z - sc.a - 32 : 0;
        std::vector<std::thread> th;
        for (unsigned t = 0; t < nt; t++) th.emplace_back([&, t] {
            size_t from = sc.a + (len / nt) * t, to = t + 1 == nt ? sc.a + len : sc.a + (len / nt) * (t + 1);
            for (size_t i = from & ~(size_t)3; i < to && !found; i += 4) {
                const uint8_t* k = b + i; int zeros = 0; uint32_t seen[8] = {}; int distinct = 0;
                for (int q = 0; q < 32; q++) { if (!k[q]) zeros++; uint32_t bit = 1u << (k[q] & 31); if (!(seen[k[q] >> 5] & bit)) { seen[k[q] >> 5] |= bit; distinct++; } }
                if (zeros > 3 || distinct < 22) continue;
                if (KeyOpens(k, samples)) { std::lock_guard<std::mutex> lk(mx); if (!found) { found = true; foundKey = KeyHex(k); } }
            }
        });
        for (auto& t : th) t.join();
    }
    if (found) { keyHex = foundKey; msg = "Found the game's AES key."; return true; }
    msg = "Couldn't find the game's AES key in the running game (it may be stored scrambled). You can still type it in on the Models page.";
    return false;
}
// ---- packages
struct Imp { std::string cls, name; int outer; };
struct Exp { int cls, outer; std::string name; long long size, off; };
struct Package {
    std::string asset, exp, bulk; int ver = 0; uint32_t flags = 0; int total = 0;
    std::vector<std::string> names; std::vector<Imp> imports; std::vector<Exp> exports;
    std::string err;
    const std::string& Data() const { return exp.empty() ? asset : exp; }
    long long DataOff(const Exp& e) const { return exp.empty() ? e.off : e.off - total; }
    std::string N(Rd& r) const { int i = r.get<int>(), n = r.get<int>(); if (i < 0 || i >= (int)names.size()) { r.bad = true; return ""; } return n ? names[i] + "_" + std::to_string(n - 1) : names[i]; }
    std::string ObjName(int idx) const { if (idx < 0 && -idx - 1 < (int)imports.size()) return imports[-idx - 1].name; if (idx > 0 && idx - 1 < (int)exports.size()) return exports[idx - 1].name; return ""; }
    std::string ClassOf(const Exp& e) const { return ObjName(e.cls); }
    std::string ImportPackage(int idx) const {   // package path of an import ("/Game/...")
        int o = idx; int guard = 0;
        while (o < 0 && -o - 1 < (int)imports.size() && imports[-o - 1].outer != 0 && guard++ < 16) o = imports[-o - 1].outer;
        return o < 0 && -o - 1 < (int)imports.size() ? imports[-o - 1].name : "";
    }
};
// ---- Unreal 4.26 / 4.27 IoStore packages ("zen" packages with tagged properties)
static std::vector<std::string> NameBatch(const std::string& b, size_t at, size_t end, size_t count) {
    std::vector<std::string> out; size_t o = at;
    for (size_t i = 0; i < count && o + 2 <= end; i++) {
        uint8_t b0 = (uint8_t)b[o], b1 = (uint8_t)b[o + 1]; o += 2;
        bool wide = (b0 & 0x80) != 0; size_t len = ((size_t)(b0 & 0x7F) << 8) | b1;
        std::string n;
        if (wide) { if (o & 1) o++; for (size_t k = 0; k < len && o + 2 <= end; k++, o += 2) { uint16_t c; memcpy(&c, b.data() + o, 2); n += c < 128 ? (char)c : '?'; } }
        else { if (o + len > end) break; n.assign(b.data() + o, len); o += len; }
        out.push_back(n);
    }
    return out;
}
static const IoContainer* FindIoChunk(PakSet& ps, uint8_t type, IoRef& ref) {
    for (int c = 0; c < (int)ps.io.size(); c++) for (int i = 0; i < (int)ps.io[c].chunkIds.size(); i++)
        if (ps.io[c].chunkIds[i][11] == type) { ref = { c, i }; return &ps.io[c]; }
    return nullptr;
}
static void LoadScriptObjects(PakSet& ps) {
    if (ps.scriptLoaded) return; ps.scriptLoaded = true;
    // Unreal 4.27 chunk types: 7 initial load meta (script objects), 8 global names, 9 global name hashes
    IoRef rm, rn, rh; std::string meta, names, hashes, err;
    if (!FindIoChunk(ps, 7, rm) || !FindIoChunk(ps, 8, rn) || !FindIoChunk(ps, 9, rh)) return;
    if (!ReadIoChunk(ps, rm, meta, err) || !ReadIoChunk(ps, rn, names, err) || !ReadIoChunk(ps, rh, hashes, err) || hashes.size() < 8) return;
    std::vector<std::string> gn = NameBatch(names, 0, names.size(), (hashes.size() - 8) / 8);
    Rd r(meta); int n = r.get<int>();
    for (int i = 0; i < n && !r.bad; i++) {
        uint32_t ni = r.get<uint32_t>(), num = r.get<uint32_t>(); uint64_t gi = r.get<uint64_t>(), outer = r.get<uint64_t>(); r.get<uint64_t>();
        ni &= 0x3FFFFFFF;
        std::string nm = ni < gn.size() ? gn[ni] : std::string();
        if (num) nm += "_" + std::to_string(num - 1);
        ps.scriptObjects[gi] = { nm, outer };
    }
}
static bool IsZen427(const std::string& b) { uint32_t v; return b.size() > 0x40 && (memcpy(&v, b.data() + 0x18, 4), v == 0x40); }
struct ZenHdr { std::vector<std::string> names; std::vector<uint64_t> imports; struct E { uint64_t off, size, outer, cls, global; std::string name; }; std::vector<E> exports; std::vector<uint32_t> order; size_t dataStart = 0; };
static bool ParseZen427(const std::string& b, ZenHdr& z) {
    Rd r(b, 0x10); r.get<uint32_t>(); /*flags*/ uint32_t cookedHeader = r.get<uint32_t>(); (void)cookedHeader;
    uint32_t nOff = r.get<uint32_t>(), nSize = r.get<uint32_t>(), hOff = r.get<uint32_t>(), hSize = r.get<uint32_t>();
    uint32_t impOff = r.get<uint32_t>(), expOff = r.get<uint32_t>(), bunOff = r.get<uint32_t>(), gOff = r.get<uint32_t>(), gSize = r.get<uint32_t>();
    (void)hOff;
    if (r.bad || hSize < 8 || expOff < impOff || bunOff < expOff || gOff < bunOff || (size_t)gOff + gSize > b.size()) return false;
    z.names = NameBatch(b, nOff, (size_t)nOff + nSize, (hSize - 8) / 8);
    for (uint32_t o = impOff; o + 8 <= expOff; o += 8) { uint64_t v; memcpy(&v, b.data() + o, 8); z.imports.push_back(v); }
    auto nameOf = [&](uint32_t idx, uint32_t num) { idx &= 0x3FFFFFFF; std::string n = idx < z.names.size() ? z.names[idx] : "?"; if (num) n += "_" + std::to_string(num - 1); return n; };
    for (uint32_t o = expOff; o + 72 <= bunOff; o += 72) {
        Rd e(b, o); ZenHdr::E x; x.off = e.get<uint64_t>(); x.size = e.get<uint64_t>();
        uint32_t ni = e.get<uint32_t>(), nn = e.get<uint32_t>(); x.name = nameOf(ni, nn);
        x.outer = e.get<uint64_t>(); x.cls = e.get<uint64_t>(); e.get<uint64_t>(); e.get<uint64_t>(); x.global = e.get<uint64_t>();
        z.exports.push_back(x);
    }
    // export bundles: headers (first entry, count) then entries (export index, command 0 create / 1 serialize)
    size_t nExp = z.exports.size(), bunBytes = gOff - bunOff;
    if (bunBytes < 16 * nExp) return false;
    size_t nBundles = (bunBytes - 16 * nExp) / 8;
    size_t entries = bunOff + 8 * nBundles;
    for (size_t bI = 0; bI < nBundles; bI++) {
        uint32_t first, count; memcpy(&first, b.data() + bunOff + 8 * bI, 4); memcpy(&count, b.data() + bunOff + 8 * bI + 4, 4);
        for (uint32_t k = 0; k < count && entries + 8ull * (first + k) + 8 <= gOff; k++) {
            uint32_t ei, cmd; memcpy(&ei, b.data() + entries + 8ull * (first + k), 4); memcpy(&cmd, b.data() + entries + 8ull * (first + k) + 4, 4);
            if (cmd == 1 && ei < nExp) z.order.push_back(ei);
        }
    }
    z.dataStart = (size_t)gOff + gSize;
    return true;
}
static const std::vector<std::pair<uint64_t, std::string>>* ZenExportsOf(PakSet& ps, const std::string& key) {
    auto it = ps.zenExports.find(key); if (it != ps.zenExports.end()) return &it->second;
    auto& v = ps.zenExports[key];
    std::string b, err;
    if (ReadFileFromPak(ps, key, b, err) && IsZen427(b)) { ZenHdr z; if (ParseZen427(b, z)) for (auto& e : z.exports) v.push_back({ e.global, e.name }); }
    return &v;
}
static bool LoadZenPackage(PakSet& ps, const std::string& key, Package& P);
static bool ParseLegacyHeader(Package& P);
static bool LoadPackage(PakSet& ps, std::string pkgPath, Package& P) {
    size_t dot = pkgPath.find('.', pkgPath.find_last_of('/')); if (dot != std::string::npos) pkgPath = pkgPath.substr(0, dot);
    std::string k = LowerA(pkgPath);
    std::string ext = ".uasset";
    if (!ReadFileFromPak(ps, k + ext, P.asset, P.err)) {
        std::string e3; ext = ".umap";   // a level
        if (!ReadFileFromPak(ps, k + ext, P.asset, e3)) { if (P.err.empty()) P.err = "the game's files don't have " + pkgPath; return false; }
        P.err.clear();
    }
    std::string e2; ReadFileFromPak(ps, k + ".uexp", P.exp, e2); ReadFileFromPak(ps, k + ".ubulk", P.bulk, e2);
    {
        Rd r(P.asset);
        if (r.get<uint32_t>() != 0x9E2A83C1) {
            if (ps.ioFiles.count(k + ext) && IsZen427(P.asset)) return LoadZenPackage(ps, k, P);   // Unreal 4.26/4.27 IoStore package
            P.err = ps.ioFiles.count(k + ext) ? "the game's files opened, but this is an Unreal 5 package, which the built-in exporter can't read yet" : "not an Unreal package";
            return false;
        }
    }
    return ParseLegacyHeader(P);
}
static bool ParseLegacyHeader(Package& P) {
    Rd r(P.asset);
    if (r.get<uint32_t>() != 0x9E2A83C1) { P.err = "not an Unreal package"; return false; }
    int legacy = r.get<int>();
    if (legacy < -7) { P.err = "this is an Unreal 5 package, which the built-in exporter can't read yet (use FModel)"; return false; }
    if (legacy != -4) r.get<int>();
    P.ver = r.get<int>(); r.get<int>();
    if (P.ver == 0) P.ver = 522;                         // cooked "unversioned": newest Unreal 4
    if (legacy <= -2) { int n = r.get<int>(); if (n < 0 || n > 1000) { P.err = "unexpected package header"; return false; } r.skip((size_t)n * (legacy <= -5 ? 20 : legacy == -2 ? 8 : 20)); }
    P.total = r.get<int>(); r.fstr(); P.flags = r.get<uint32_t>();
    if (P.flags & 0x2000) { P.err = "this game saves its files without property names (unversioned), which the built-in exporter can't read yet - use FModel"; return false; }
    int nc = r.get<int>(), no = r.get<int>();
    if (P.ver >= 516 && !(P.flags & 0x80000000)) r.fstr();
    if (P.ver >= 459) { r.get<int>(); r.get<int>(); }
    int ec = r.get<int>(), eo = r.get<int>(), ic = r.get<int>(), io = r.get<int>();
    if (r.bad || nc < 0 || nc > 1000000 || ec < 0 || ec > 100000 || ic < 0 || ic > 100000) { P.err = "unexpected package header"; return false; }
    Rd rn(P.asset, (size_t)no);
    for (int i = 0; i < nc && !rn.bad; i++) { P.names.push_back(rn.fstr()); if (P.ver >= 504) rn.skip(4); }
    Rd ri(P.asset, (size_t)io);
    for (int i = 0; i < ic && !ri.bad; i++) { Imp im; P.N(ri); im.cls = P.N(ri); im.outer = ri.get<int>(); im.name = P.N(ri); P.imports.push_back(im); }
    Rd re(P.asset, (size_t)eo);
    for (int i = 0; i < ec && !re.bad; i++) {
        Exp e; e.cls = re.get<int>(); re.get<int>(); if (P.ver >= 508) re.get<int>(); e.outer = re.get<int>(); e.name = P.N(re); re.get<uint32_t>();
        if (P.ver >= 511) { e.size = re.get<long long>(); e.off = re.get<long long>(); } else { e.size = re.get<int>(); e.off = re.get<int>(); }
        re.skip(4 * 3 + 16 + 4); if (P.ver >= 365) re.skip(4); if (P.ver >= 485) re.skip(4); if (P.ver >= 507) re.skip(20);
        P.exports.push_back(e);
    }
    if (rn.bad || ri.bad || re.bad) { P.err = "unexpected package layout"; return false; }
    return true;
}
static bool LoadZenPackage(PakSet& ps, const std::string& k, Package& P) {
    ZenHdr z;
    if (!ParseZen427(P.asset, z)) { P.err = "unexpected package layout (IoStore)"; return false; }
    LoadScriptObjects(ps);
    P.ver = 522; P.flags = 0; P.names = z.names; P.exp.clear(); P.total = 0;
    // imports: engine classes by name; other packages' objects by finding which package exports them
    std::vector<std::string> cands;
    for (auto& n : z.names) if (n.size() > 1 && n[0] == '/' && n.rfind("/Script/", 0) != 0 && n.find('.') == std::string::npos) cands.push_back(n);
    std::unordered_map<std::string, int> pkgImp;
    std::vector<Imp> imps(z.imports.size()), extra;
    auto resolve = [&](uint64_t v, Imp& im) {
        int type = (int)(v >> 62); im.outer = 0;
        if (type == 1) { auto it = ps.scriptObjects.find(v); im.name = it != ps.scriptObjects.end() ? it->second.first : "?"; im.cls = "Class"; return; }
        if (type != 2) { im.name = ""; return; }
        for (auto& c : cands) {
            auto* ex = ZenExportsOf(ps, LowerA(c) + ".uasset");
            for (auto& e : *ex) if (e.first == v) {
                im.name = e.second; im.cls = "";
                auto f = pkgImp.find(c);
                if (f == pkgImp.end()) { extra.push_back(Imp{ "Package", c, 0 }); f = pkgImp.insert({ c, -(int)(z.imports.size() + extra.size()) }).first; }
                im.outer = f->second;
                return;
            }
        }
        im.name = "?";
    };
    for (size_t i = 0; i < z.imports.size(); i++) resolve(z.imports[i], imps[i]);
    // an object index (export, import or engine class) in the package's own numbering: +n export, -n import
    auto idxOf = [&](uint64_t v) -> int {
        int type = (int)(v >> 62);
        if (type == 3) return 0;
        if (type == 0) return (int)(v & 0x3FFFFFFFFFFFFFFFULL) + 1;
        for (size_t j = 0; j < z.imports.size(); j++) if (z.imports[j] == v) return -(int)(j + 1);
        Imp im; resolve(v, im); extra.push_back(im); return -(int)(z.imports.size() + extra.size());
    };
    std::vector<Exp> exps(z.exports.size());
    for (size_t i = 0; i < z.exports.size(); i++) { exps[i].name = z.exports[i].name; exps[i].size = 0; exps[i].off = 0; }
    for (size_t i = 0; i < z.exports.size(); i++) { exps[i].cls = idxOf(z.exports[i].cls); exps[i].outer = idxOf(z.exports[i].outer); }
    size_t cur = z.dataStart;
    for (uint32_t ei : z.order) { exps[ei].off = (long long)cur; exps[ei].size = (long long)z.exports[ei].size; cur += (size_t)z.exports[ei].size; }
    if (cur > P.asset.size()) { P.err = "unexpected package layout (IoStore data)"; return false; }
    // extra imports go after the package's own (indices past the import map), outers pointing at them stay valid
    imps.insert(imps.end(), extra.begin(), extra.end());
    P.imports = imps; P.exports = exps;
    return true;
}
struct Tag { std::string type, structName, inner; int size = 0; size_t at = 0; uint8_t boolVal = 0; };
typedef std::multimap<std::string, Tag> Props;
static Props ReadProps(const Package& P, Rd& r, size_t end) {
    Props out;
    while (!r.bad && r.o < end) {
        std::string name = P.N(r);
        if (r.bad || name == "None") break;
        Tag t; t.type = P.N(r); t.size = r.get<int>(); r.get<int>();
        if (t.type == "StructProperty") { t.structName = P.N(r); r.skip(16); }
        else if (t.type == "BoolProperty") t.boolVal = r.get<uint8_t>();
        else if (t.type == "ByteProperty" || t.type == "EnumProperty") P.N(r);
        else if (t.type == "ArrayProperty" || t.type == "SetProperty") t.inner = P.N(r);
        else if (t.type == "MapProperty") { t.inner = P.N(r); P.N(r); }
        if (P.ver >= 503 && r.get<uint8_t>()) r.skip(16);
        t.at = r.o;
        if (t.size < 0 || r.o + (size_t)t.size > end) { r.bad = true; break; }
        out.insert({ name, t });
        r.o += t.size;
    }
    return out;
}
static const Tag* Find(const Props& p, const char* n) { auto it = p.find(n); return it == p.end() ? nullptr : &it->second; }
static std::vector<Props> StructArray(const Package& P, const Tag& t) {
    std::vector<Props> out; if (t.inner != "StructProperty") return out;
    Rd r(P.Data(), t.at); size_t end = t.at + t.size;
    int n = r.get<int>(); if (n < 0 || n > 100000) return out;
    P.N(r); P.N(r); r.get<int>(); r.get<int>(); P.N(r); r.skip(16); if (P.ver >= 503 && r.get<uint8_t>()) r.skip(16);
    for (int i = 0; i < n && !r.bad; i++) out.push_back(ReadProps(P, r, end));
    return out;
}
static int ObjAt(const Package& P, const Tag* t) { if (!t) return 0; Rd r(P.Data(), t->at); return r.get<int>(); }
static std::string NameAt(const Package& P, const Tag* t) { if (!t) return ""; Rd r(P.Data(), t->at); return P.N(r); }

// ---- textures
static double Half(uint16_t h) {
    int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    double v = e == 0 ? std::ldexp(m, -24) : e == 31 ? 65504 : std::ldexp(m + 1024, e - 25);
    return s ? -v : v;
}
static void C565(uint16_t c, int o[3]) { o[0] = ((c >> 11) & 31) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; }
static void DecodeBC1(const uint8_t* b, uint8_t out[16][4]) {
    uint16_t c0, c1; uint32_t bits; memcpy(&c0, b, 2); memcpy(&c1, b + 2, 2); memcpy(&bits, b + 4, 4);
    int p[4][3]; C565(c0, p[0]); C565(c1, p[1]);
    for (int k = 0; k < 3; k++) { if (c0 > c1) { p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3; } else { p[2][k] = (p[0][k] + p[1][k]) / 2; p[3][k] = 0; } }
    for (int i = 0; i < 16; i++) { int s = (bits >> (2 * i)) & 3; for (int k = 0; k < 3; k++) out[i][k] = (uint8_t)p[s][k]; out[i][3] = 255; }
}
// BC7 (all modes, colour only)
struct BitR { const uint8_t* d; int pos = 0; uint32_t get(int n) { uint32_t v = 0; for (int i = 0; i < n; i++, pos++) v |= ((d[pos >> 3] >> (pos & 7)) & 1u) << i; return v; } };
static const uint16_t kP2[64] = { 0xCCCC,0x8888,0xEEEE,0xECC8,0xC880,0xFEEC,0xFEC8,0xEC80,0xC800,0xFFEC,0xFE80,0xE800,0xFFE8,0xFF00,0xFFF0,0xF000,0xF710,0x008E,0x7100,0x08CE,0x008C,0x7310,0x3100,0x8CCE,0x088C,0x3110,0x6666,0x366C,0x17E8,0x0FF0,0x718E,0x399C,
    0xAAAA,0xF0F0,0x5A5A,0x33CC,0x3C3C,0x55AA,0x9696,0xA55A,0x73CE,0x13C8,0x324C,0x3BDC,0x6996,0xC33C,0x9966,0x0660,0x0272,0x04E4,0x4E40,0x2720,0xC936,0x936C,0x39C6,0x639C,0x9336,0x9CC6,0x817E,0xE718,0xCCF0,0x0FCC,0x7744,0xEE22 };
static const uint8_t kP3[64][16] = {
{0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},{0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},{0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},{0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},{0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},{0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},{0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},{0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},{0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},{0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},{0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},{0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},{0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
{0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2},{0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},{0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},{0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},{0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},{0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},{0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},{0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
{0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},{0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},{0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},{0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},{0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},{0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},{0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},{0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
{0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},{0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},{0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},{0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},{0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},{0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},{0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},{0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
{0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},{0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},{0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},{0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},{0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},{0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},{0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
{0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},{0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},{0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},{0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},{0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},{0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},{0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
{0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},{0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},{0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},{0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},{0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},{0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},{0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},{0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0} };
static const uint8_t kA2[64] = { 15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,2,8,2,2,8,8,15,2,8,2,2,8,8,2,2,15,15,6,8,2,8,15,15,2,8,2,2,2,15,15,6,6,2,6,8,15,15,2,2,15,15,15,15,15,2,2,15 };
static const uint8_t kA3a[64] = { 3,3,15,15,8,3,15,15,8,8,6,6,6,5,3,3,3,3,8,15,3,3,6,10,5,8,8,6,8,5,15,15,8,15,3,5,6,10,8,15,15,3,15,5,15,15,15,15,3,15,5,5,5,8,5,10,5,10,8,13,15,12,3,3 };
static const uint8_t kA3b[64] = { 15,8,8,3,15,15,3,8,15,15,15,15,15,15,15,8,15,8,15,3,15,8,15,8,3,15,6,10,15,15,10,8,15,3,15,10,10,8,9,10,6,15,8,15,3,6,6,8,15,3,15,15,15,15,15,15,15,15,15,15,3,15,15,8 };
static const uint8_t kW2[4] = { 0,21,43,64 }, kW3[8] = { 0,9,18,27,37,46,55,64 }, kW4[16] = { 0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64 };
static void DecodeBC7(const uint8_t* blk, uint8_t out[16][4]) {
    int mode = 0; while (mode < 8 && !((blk[0] >> mode) & 1)) mode++;
    if (mode == 8) { for (int i = 0; i < 16; i++) { out[i][0] = out[i][1] = out[i][2] = 0; out[i][3] = 255; } return; }
    static const int NS[8] = { 3,2,3,2,1,1,1,2 }, PB[8] = { 4,6,6,6,0,0,0,6 }, RB[8] = { 0,0,0,0,2,2,0,0 }, ISB[8] = { 0,0,0,0,1,0,0,0 },
        CB[8] = { 4,6,5,7,5,7,7,5 }, AB[8] = { 0,0,0,0,6,8,7,5 }, EPB[8] = { 1,0,0,1,0,0,1,1 }, SPB[8] = { 0,1,0,0,0,0,0,0 }, IB[8] = { 3,3,2,2,2,2,4,2 }, IB2[8] = { 0,0,0,0,3,2,0,0 };
    BitR br{ blk }; br.pos = mode + 1;
    int part = br.get(PB[mode]), rot = br.get(RB[mode]), isel = br.get(ISB[mode]);
    int ns = NS[mode]; int ep[6][4] = {};
    for (int c = 0; c < 3; c++) for (int e = 0; e < ns * 2; e++) ep[e][c] = br.get(CB[mode]);
    for (int e = 0; e < ns * 2; e++) ep[e][3] = AB[mode] ? br.get(AB[mode]) : 255;
    int cb = CB[mode], ab = AB[mode];
    if (EPB[mode]) { for (int e = 0; e < ns * 2; e++) { int p = br.get(1); for (int c = 0; c < 3; c++) ep[e][c] = (ep[e][c] << 1) | p; if (ab) ep[e][3] = (ep[e][3] << 1) | p; } cb++; if (ab) ab++; }
    if (SPB[mode]) { for (int s = 0; s < ns; s++) { int p = br.get(1); for (int e = s * 2; e < s * 2 + 2; e++) { for (int c = 0; c < 3; c++) ep[e][c] = (ep[e][c] << 1) | p; } } cb++; }
    for (int e = 0; e < ns * 2; e++) {
        for (int c = 0; c < 3; c++) ep[e][c] = (ep[e][c] << (8 - cb)) | (ep[e][c] >> (2 * cb - 8 > 0 ? 2 * cb - 8 : 0)), ep[e][c] &= 255;
        if (ab) { ep[e][3] = (ep[e][3] << (8 - ab)) | (ep[e][3] >> (2 * ab - 8 > 0 ? 2 * ab - 8 : 0)); ep[e][3] &= 255; }
    }
    auto subset = [&](int i) { return ns == 1 ? 0 : ns == 2 ? ((kP2[part] >> i) & 1) : kP3[part][i]; };
    auto anchor = [&](int s) { return s == 0 ? 0 : ns == 2 ? kA2[part] : (s == 1 ? kA3a[part] : kA3b[part]); };
    int idx1[16], idx2[16] = {};
    int ib = IB[mode], ib2 = IB2[mode];
    for (int i = 0; i < 16; i++) { bool a = false; for (int s = 0; s < ns; s++) if (anchor(s) == i && (ns > 1 || i == 0)) a = true; if (ns == 1 && i == 0) a = true; idx1[i] = br.get(a ? ib - 1 : ib); }
    if (ib2) for (int i = 0; i < 16; i++) idx2[i] = br.get(i == 0 ? ib2 - 1 : ib2);
    auto w = [](int bits, int i) { return bits == 2 ? kW2[i] : bits == 3 ? kW3[i] : kW4[i]; };
    for (int i = 0; i < 16; i++) {
        int s = subset(i); int* a = ep[s * 2]; int* b = ep[s * 2 + 1];
        int ci = idx1[i], ai = idx1[i], cbits = ib, abits = ib;
        if (ib2) { if (isel) { ci = idx2[i]; cbits = ib2; } else { ai = idx2[i]; abits = ib2; } }
        int wc = w(cbits, ci), wa = w(abits, ai);
        int px[4]; for (int c = 0; c < 3; c++) px[c] = ((64 - wc) * a[c] + wc * b[c] + 32) >> 6; px[3] = ((64 - wa) * a[3] + wa * b[3] + 32) >> 6;
        if (rot) std::swap(px[3], px[rot - 1]);
        for (int c = 0; c < 4; c++) out[i][c] = (uint8_t)px[c];
    }
}
static void DecodeBC4(const uint8_t* b, uint8_t out[16]) {
    int a0 = b[0], a1 = b[1]; int pal[8]; pal[0] = a0; pal[1] = a1;
    if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
    uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)pal[(bits >> (3 * i)) & 7];
}
static bool DecodeTexture(const std::string& fmt, int w, int h, const std::string& d, Image& im) {
    im.w = w; im.h = h; im.px.assign((size_t)w * h * 4, 255);
    auto put = [&](int x, int y, const uint8_t* p) { if (x < w && y < h) memcpy(&im.px[((size_t)y * w + x) * 4], p, 4); };
    bool bc1 = fmt == "PF_DXT1", bc3 = fmt == "PF_DXT5" || fmt == "PF_DXT3", bc7 = fmt == "PF_BC7";
    if (bc1 || bc3 || bc7) {
        size_t bs = bc1 ? 8 : 16, k = 0; int bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (d.size() < bs * bw * bh) return false;
        uint8_t px[16][4];
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++, k += bs) {
            const uint8_t* b = (const uint8_t*)d.data() + k;
            if (bc7) DecodeBC7(b, px); else DecodeBC1(bc1 ? b : b + 8, px);
            for (int i = 0; i < 16; i++) put(bx * 4 + (i & 3), by * 4 + (i >> 2), px[i]);
        }
        return true;
    }
    if (fmt == "PF_BC5" || fmt == "PF_BC4" || fmt == "PF_ATI2") {   // two-channel normal maps (BC5) / single-channel masks (BC4)
        bool two = fmt != "PF_BC4"; size_t bs = two ? 16 : 8, k = 0; int bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (d.size() < bs * bw * bh) return false;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++, k += bs) {
            uint8_t r[16], g[16]; DecodeBC4((const uint8_t*)d.data() + k, r); if (two) DecodeBC4((const uint8_t*)d.data() + k + 8, g);
            for (int i = 0; i < 16; i++) {
                uint8_t px[4];
                if (two) { double x = r[i] / 127.5 - 1, y = g[i] / 127.5 - 1, z = std::sqrt(std::max(0.0, 1 - x * x - y * y)); px[0] = r[i]; px[1] = g[i]; px[2] = (uint8_t)std::lround((z * 0.5 + 0.5) * 255); }
                else px[0] = px[1] = px[2] = r[i];
                px[3] = 255; put(bx * 4 + (i & 3), by * 4 + (i >> 2), px);
            }
        }
        return true;
    }
    if (fmt == "PF_B8G8R8A8" || fmt == "PF_R8G8B8A8") {
        if (d.size() < (size_t)w * h * 4) return false;
        for (size_t i = 0; i < (size_t)w * h; i++) { const uint8_t* s = (const uint8_t*)d.data() + i * 4; uint8_t p[4] = { s[fmt[3] == 'B' ? 2 : 0], s[1], s[fmt[3] == 'B' ? 0 : 2], 255 }; memcpy(&im.px[i * 4], p, 4); }
        return true;
    }
    if (fmt == "PF_G8") { if (d.size() < (size_t)w * h) return false; for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t g = (uint8_t)d[i]; uint8_t p[4] = { g, g, g, 255 }; memcpy(&im.px[i * 4], p, 4); } return true; }
    return false;
}
// reads one texture from the game's files (no caching): the most detailed mip up to maxDim
static bool ReadGameTexture(PakSet& ps, const std::string& pkgPath, std::string& note, int maxDim, Image& im) {
    Package P;
    auto fail = [&](const std::string& why) -> bool { note += why + "; "; return false; };
    if (!LoadPackage(ps, pkgPath, P)) return fail(P.err);
    const Exp* e = nullptr; for (auto& x : P.exports) if (P.ClassOf(x) == "Texture2D") { e = &x; break; }
    if (!e) return fail("no texture in " + pkgPath);
    const std::string& D = P.Data(); size_t start = (size_t)P.DataOff(*e);
    Rd r(D, start); ReadProps(P, r, start + (size_t)e->size);
    if (r.get<int>()) r.skip(16);
    r.skip(4); int cooked = r.get<int>();
    if (!cooked || r.bad) return fail("texture isn't cooked");
    std::string pf = P.N(r); if (P.ver >= 504) r.get<long long>(); else r.get<int>();
    int sx = r.get<int>(), sy = r.get<int>(); uint32_t packed = r.get<uint32_t>();
    if (packed & (1u << 31)) return fail("virtual texture");
    std::string fmt = r.fstr(); r.get<int>(); int nm = r.get<int>();
    struct Mip { int w, h; std::string data; };
    std::vector<Mip> mips;
    for (int m = 0; m < nm && !r.bad && m < 20; m++) {
        r.get<int>(); uint32_t fl = r.get<uint32_t>();
        long long cnt, disk; if (fl & 0x2000) { cnt = r.get<long long>(); disk = r.get<long long>(); } else { cnt = r.get<int>(); disk = r.get<int>(); }
        long long off = r.get<long long>(); (void)cnt;
        Mip mp; mp.w = mp.h = 0;
        if (disk > 0 && disk < (1ll << 31)) {
            if (fl & 0x100) { if (!(fl & 0x800) && off >= 0 && off + disk <= (long long)P.bulk.size()) mp.data = P.bulk.substr((size_t)off, (size_t)disk); }
            else if ((fl & 0x40) || !(fl & 0x1)) { if (r.o + disk <= D.size()) mp.data = D.substr(r.o, (size_t)disk); r.skip((size_t)disk); }
            else { long long at = off - (P.exp.empty() ? 0 : P.total); if (at >= 0 && at + disk <= (long long)D.size()) mp.data = D.substr((size_t)at, (size_t)disk); }
        }
        mp.w = r.get<int>(); mp.h = r.get<int>(); r.get<int>();
        mips.push_back(mp);
    }
    (void)sx; (void)sy; (void)fmt;
    // the most detailed mip that's in the game's files, up to maxDim
    const Mip* pick = nullptr;
    for (auto& m : mips) if (!m.data.empty() && std::max(m.w, m.h) <= maxDim) { pick = &m; break; }
    if (!pick) for (auto& m : mips) if (!m.data.empty()) pick = &m;
    if (!pick) return fail("texture data not found");
    if (!DecodeTexture(pf, pick->w, pick->h, pick->data, im)) return fail("texture format " + pf + " isn't supported");
    return true;
}
static const Image* LoadGameTexture(PakSet& ps, const std::string& pkgPath, std::string& note, int maxDim = 2048) {
    std::wstring key = U2W("game:" + LowerA(pkgPath) + ":" + std::to_string(maxDim));
    auto it = g_imgCache.find(key); if (it != g_imgCache.end()) return it->second.w ? &it->second : nullptr;
    Image im;
    if (!ReadGameTexture(ps, pkgPath, note, maxDim, im)) { g_imgCache[key] = Image(); return nullptr; }
    auto& slot = g_imgCache[key] = std::move(im);
    return &slot;
}
static int ColourScore(const std::string& raw) {
    std::string n = LowerA(raw);
    for (auto bad : { "normal", "rough", "metal", "mask", "spec", "height", "emiss", "opacity", "ao", "orm", "mra", "detail", "noise", "lp" }) if (n.find(bad) != std::string::npos) return -10;
    if (n == "nm" || n == "n" || n == "_n") return -10;
    for (auto good : { "basecolor", "base color", "base_color", "diffuse", "albedo", "colour", "color" }) if (n.find(good) != std::string::npos) return 10;
    for (auto good : { "co", "bc", "d", "c", "diff", "alb", "col", "tex", "texture", "main" }) if (n == good) return 8;
    for (auto suf : { "_co", "_bc", "_d", "_diff", "_alb", "_col", "_c", "_basecolor", "_albedo", "_diffuse", "_color" }) if (n.size() > strlen(suf) && n.compare(n.size() - strlen(suf), strlen(suf), suf) == 0) return 6;
    return 0;
}
// a colour setting that tints the colour picture (Stray's "CO mul", "Tint", "BaseColor Multiplier"...): baked into the
// exported picture so the model keeps its colour in other games
static int TintScore(const std::string& raw) {
    std::string n = LowerA(raw);
    for (auto bad : { "emiss", "emission", "glow", "fresnel", "rim", "fog", "subsurface", "sss", "sheen", "fuzz", "edge", "scatter", "spec",
                      "dirt", "wet", "snow", "dust", "grime", "blood", "moss", "light", "shadow", "sky", "water", "outline", "normal", "rough", "metal" })
        if (n.find(bad) != std::string::npos) return 0;
    if (n.rfind("si ", 0) == 0 || n.rfind("si_", 0) == 0 || n.rfind("si/", 0) == 0 || n == "si") return 0;    // self-illumination
    if (ColourScore(raw) >= 8) return 10;
    bool tintWord = n.find("tint") != std::string::npos || n.find("multipl") != std::string::npos || n.find("mul") != std::string::npos;
    bool colWord = n.find("col") != std::string::npos || n.rfind("co", 0) == 0 || n.rfind("bc", 0) == 0 || n.find("base") != std::string::npos ||
                   n.find("albedo") != std::string::npos || n.find("diff") != std::string::npos || n.find("main") != std::string::npos;
    if (tintWord && (colWord || n == "tint")) return 9;
    return 0;
}
static int NormalScore(const std::string& raw) {
    std::string n = LowerA(raw);
    if (n.find("detail") != std::string::npos || n.find("macro") != std::string::npos) return 0;
    if (n.find("normal") != std::string::npos) return 10;
    for (auto good : { "nm", "n", "nrm", "nor", "norm" }) if (n == good) return 9;
    for (auto suf : { "_n", "_nm", "_nrm", "_nor", "_norm", "_normal", "_nmap" }) if (n.size() > strlen(suf) && n.compare(n.size() - strlen(suf), strlen(suf), suf) == 0) return 7;
    return 0;
}
// the normal map of a material (instance), following parent instances
static std::string MaterialNormalTexture(PakSet& ps, const std::string& matPkg, int depth = 0) {
    Package P; if (depth > 4 || !LoadPackage(ps, matPkg, P)) return "";
    const Exp* e = nullptr; for (auto& x : P.exports) { std::string c = P.ClassOf(x); if (c.find("MaterialInstance") != std::string::npos || c == "Material") { e = &x; break; } }
    if (!e) return "";
    Rd r(P.Data(), (size_t)P.DataOff(*e)); Props pr = ReadProps(P, r, (size_t)P.DataOff(*e) + (size_t)e->size);
    int best = 0; std::string bestPkg;
    if (auto t = Find(pr, "TextureParameterValues")) for (auto& el : StructArray(P, *t)) {
        std::string pname;
        if (auto pi = Find(el, "ParameterInfo")) { Rd ri(P.Data(), pi->at); Props ip = ReadProps(P, ri, pi->at + pi->size); pname = NameAt(P, Find(ip, "Name")); }
        else pname = NameAt(P, Find(el, "ParameterName"));
        int obj = ObjAt(P, Find(el, "ParameterValue"));
        if (obj >= 0) continue;
        int sc = std::max(NormalScore(pname), NormalScore(P.ObjName(obj)) - 1);
        if (sc > best) { best = sc; bestPkg = P.ImportPackage(obj); }
    }
    if (!bestPkg.empty()) return bestPkg;
    if (auto par = Find(pr, "Parent")) { int o = ObjAt(P, par); if (o < 0) { std::string got = MaterialNormalTexture(ps, P.ImportPackage(o), depth + 1); if (!got.empty()) return got; } }
    return "";
}
// roughness / metal / ambient occlusion textures, only when the name says clearly how they're laid out
// (returns 1 ORM, 2 MRA, 3 RMA, 4 roughness, 5 metal, 6 occlusion, 7 packed with only roughness known (green), 0 unknown)
static int MaskKind(const std::string& raw) {
    std::string n = LowerA(raw);
    if (n.find("detail") != std::string::npos || n.find("normal") != std::string::npos) return 0;
    std::vector<std::string> tok; std::string cur;
    for (char c : n) { if (c == '_' || c == ' ' || c == '-' || c == '.') { if (!cur.empty()) tok.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) tok.push_back(cur);
    for (auto& t : tok) {
        if (t == "orm" || t == "arm" || t == "aorm" || t == "occlusionroughnessmetallic") return 1;
        if (t == "mra" || t == "mrao") return 2;
        if (t == "rma" || t == "rmao") return 3;
    }
    if (!tok.empty() && tok.back() == "lp") return 7;     // e.g. Stray: green is roughness, the other channels are game-specific
    if (n.find("roughness") != std::string::npos) return 4;
    if (n.find("metallic") != std::string::npos || n.find("metalness") != std::string::npos) return 5;
    if (n.find("ambientocclusion") != std::string::npos || n.find("ambient occlusion") != std::string::npos || n.find("occlusion") != std::string::npos) return 6;
    if (!tok.empty()) { const std::string& l = tok.back(); if (l == "rough" || l == "r") return 4; if (l == "metal") return 5; if (l == "ao") return 6; }
    return 0;
}
static void MaterialMasks(PakSet& ps, const std::string& matPkg, std::string found[8], int depth = 0) {
    Package P; if (depth > 4 || !LoadPackage(ps, matPkg, P)) return;
    const Exp* e = nullptr; for (auto& x : P.exports) { std::string c = P.ClassOf(x); if (c.find("MaterialInstance") != std::string::npos || c == "Material") { e = &x; break; } }
    if (!e) return;
    Rd r(P.Data(), (size_t)P.DataOff(*e)); Props pr = ReadProps(P, r, (size_t)P.DataOff(*e) + (size_t)e->size);
    if (auto t = Find(pr, "TextureParameterValues")) for (auto& el : StructArray(P, *t)) {
        std::string pname;
        if (auto pi = Find(el, "ParameterInfo")) { Rd ri(P.Data(), pi->at); Props ip = ReadProps(P, ri, pi->at + pi->size); pname = NameAt(P, Find(ip, "Name")); }
        else pname = NameAt(P, Find(el, "ParameterName"));
        int obj = ObjAt(P, Find(el, "ParameterValue"));
        if (obj >= 0) continue;
        int k = MaskKind(pname); if (!k) k = MaskKind(P.ObjName(obj));
        if (k && found[k].empty()) found[k] = P.ImportPackage(obj);         // the instance's own value wins over its parent's
    }
    if (auto par = Find(pr, "Parent")) { int o = ObjAt(P, par); if (o < 0) MaterialMasks(ps, P.ImportPackage(o), found, depth + 1); }
}
static void AttachMasks(PakSet& ps, const std::string& matPkg, Mat& m, std::string& note) {
    std::string f[8]; MaterialMasks(ps, matPkg, f);
    // channel of AO, roughness, metal in each packed layout
    static const int lay[4][3] = { { 0, 0, 0 }, { 0, 1, 2 }, { 2, 1, 0 }, { 2, 0, 1 } };
    for (int k = 1; k <= 3; k++) if (!f[k].empty()) {
        if (const Image* im = LoadGameTexture(ps, f[k], note, 1024)) for (int c = 0; c < 3; c++) { m.mk[c] = im; m.mkCh[c] = lay[k][c]; }
        break;
    }
    // "LP" (Stray's packed surface map): red is metal, green roughness (blue is its self-illumination mask)
    if (!m.mk[1] && !f[7].empty()) if (const Image* im = LoadGameTexture(ps, f[7], note, 1024)) { m.mk[1] = im; m.mkCh[1] = 1; if (!m.mk[2]) { m.mk[2] = im; m.mkCh[2] = 0; } }
    for (int k = 4; k <= 6; k++) if (!f[k].empty()) {
        int role = k == 4 ? 1 : k == 5 ? 2 : 0;
        if (const Image* im = LoadGameTexture(ps, f[k], note, 1024)) { m.mk[role] = im; m.mkCh[role] = 0; }
    }
}

// ---- glow (self-illumination): a material's glow picture, its colour and strength, from the instance and its parents.
// Also Stray-style materials: "ColorMap_for_SI" on = the colour picture glows (masked by the LP map's blue).
static bool GlowName(const std::string& raw) {
    std::string n = LowerA(raw);
    for (auto w : { "emiss", "emission", "glow", "selfillum", "self illum", "self_illum", "illumination" }) if (n.find(w) != std::string::npos) return true;
    return n == "si" || n.rfind("si_", 0) == 0 || n.rfind("si ", 0) == 0 || n.rfind("si/", 0) == 0;
}
static bool GlowyName(const std::string& raw) {           // a material named for glowing
    std::string n = LowerA(raw);
    for (auto w : { "neon", "emissive", "emission", "glow", "selfillum", "self_illum", "_lights", "_light", "lamp", "screen_on", "bulb" }) if (n.find(w) != std::string::npos) return true;
    return false;
}
struct GlowFind { std::string tex, lpTex; double col[3] = { 1, 1, 1 }; bool haveCol = false; double scale = 1; bool colourGlows = false, offSaid = false, glowyChain = false; std::set<std::string> seen; };
static void MaterialGlowRec(PakSet& ps, const std::string& matPkg, GlowFind& g, int depth) {
    Package P; if (depth > 6 || !LoadPackage(ps, matPkg, P)) return;
    if (GlowyName(matPkg.substr(matPkg.find_last_of('/') + 1))) g.glowyChain = true;
    const Exp* e = nullptr; std::string cls;
    for (auto& x : P.exports) { std::string c = P.ClassOf(x); if (c.find("MaterialInstance") != std::string::npos || c == "Material") { e = &x; cls = c; break; } }
    if (!e || cls == "Material") return;      // the base material: its own defaults aren't readable here
    Rd r(P.Data(), (size_t)P.DataOff(*e)); Props pr = ReadProps(P, r, (size_t)P.DataOff(*e) + (size_t)e->size);
    auto pname = [&](const Props& el) {
        if (auto pi = Find(el, "ParameterInfo")) { Rd ri(P.Data(), pi->at); Props ip = ReadProps(P, ri, pi->at + pi->size); return NameAt(P, Find(ip, "Name")); }
        return NameAt(P, Find(el, "ParameterName"));
    };
    auto skip = [](const std::string& n) { std::string l = LowerA(n); for (auto w : { "speed", "pan", "tiling", "offset", "uv", "scale", "fresnel", "flick", "freq" }) if (l.find(w) != std::string::npos) return true; return false; };
    if (auto t = Find(pr, "TextureParameterValues")) for (auto& el : StructArray(P, *t)) {
        std::string n = pname(el); int obj = ObjAt(P, Find(el, "ParameterValue"));
        if (obj >= 0 || !g.seen.insert("t|" + n).second) continue;
        std::string on = LowerA(P.ObjName(obj));
        bool nameSays = GlowName(n) || (on.size() > 2 && (on.compare(on.size() - 2, 2, "_e") == 0 || on.find("_emi") != std::string::npos || on.find("emissive") != std::string::npos));
        if (g.tex.empty() && nameSays) g.tex = P.ImportPackage(obj);
        std::string ln = LowerA(n);
        if (g.lpTex.empty() && (ln == "lp" || (on.size() > 3 && on.compare(on.size() - 3, 3, "_lp") == 0))) g.lpTex = P.ImportPackage(obj);
    }
    if (auto t = Find(pr, "VectorParameterValues")) for (auto& el : StructArray(P, *t)) {
        std::string n = pname(el); auto v = Find(el, "ParameterValue");
        if (!v || v->size < 16 || !GlowName(n) || skip(n) || !g.seen.insert("v|" + n).second) continue;
        Rd rv(P.Data(), v->at); double c[3]; for (auto& x : c) x = std::max(0.0, (double)rv.get<float>());
        if (!g.haveCol) { g.col[0] = g.col[1] = g.col[2] = 1; g.haveCol = true; }
        for (int k = 0; k < 3; k++) g.col[k] *= c[k];
    }
    if (auto t = Find(pr, "ScalarParameterValues")) for (auto& el : StructArray(P, *t)) {
        std::string n = pname(el), l = LowerA(n); auto v = Find(el, "ParameterValue");
        if (!v || v->size != 4 || !g.seen.insert("s|" + n).second) continue;
        Rd rv(P.Data(), v->at); float val = rv.get<float>();
        // "ColorMap_for_SI", "UseColorAsEmissive": the colour picture is the glow
        if (l.find("for_si") != std::string::npos || l.find("colorasemissive") != std::string::npos || l.find("color_as_emissive") != std::string::npos || l.find("colour_as_emissive") != std::string::npos) {
            if (val > 0.5f) g.colourGlows = true; else g.offSaid = true;
            continue;
        }
        if (!GlowName(n) || skip(n)) continue;
        bool strength = false; for (auto w : { "intens", "strength", "power", "bright", "mul", "amount" }) if (l.find(w) != std::string::npos) strength = true;
        if (l == "emissive" || l == "glow" || l == "si") strength = true;
        if (strength) g.scale *= std::max(0.0, (double)val);
    }
    if (auto par = Find(pr, "Parent")) { int o = ObjAt(P, par); if (o < 0) MaterialGlowRec(ps, P.ImportPackage(o), g, depth + 1); }
}
static void MaterialGlow(PakSet& ps, const std::string& matPkg, Mat& m, std::string& note) {
    GlowFind g;
    MaterialGlowRec(ps, matPkg, g, 0);
    bool fromColour = false;
    if (g.tex.empty()) {
        // the colour picture glows: said by a setting, or the material is named for glowing (a neon tube)
        if (g.colourGlows || (g.glowyChain && !g.offSaid && !g.haveCol)) fromColour = m.tex != nullptr;
        if (!fromColour && !(g.haveCol && g.glowyChain)) return;   // (a glow colour alone, in a material not named for it, is usually masked inside it)
    }
    if (!g.haveCol) { g.col[0] = g.col[1] = g.col[2] = 4; }       // strength set inside the base material: a bright glow
    for (int k = 0; k < 3; k++) m.emiCol[k] = std::min(200.0, g.col[k] * g.scale);
    if (m.emiCol[0] + m.emiCol[1] + m.emiCol[2] <= 0.001) return;  // glow turned off
    if (!g.tex.empty()) m.emi = LoadGameTexture(ps, g.tex, note, 1024);
    else if (fromColour) { m.emi = m.tex; if (!g.lpTex.empty()) { m.emiMask = LoadGameTexture(ps, g.lpTex, note, 1024); m.emiMaskCh = 2; } }
    m.glow = true;
    if (!m.tex && m.emi) m.tex = m.emi;                   // nothing else colours it: the glow picture is its colour
}
// how a material draws (blend mode, two-sided, cut-out threshold, opacity), from the instance's overrides or its parent material
static int BlendOf(const Package& P, const Tag* t) {
    if (!t) return -1;
    if (t->size == 1) { Rd r(P.Data(), t->at); return r.get<uint8_t>(); }
    std::string n = LowerA(NameAt(P, t));
    if (n.find("masked") != std::string::npos) return 1;
    if (n.find("translucent") != std::string::npos || n.find("alphacomposite") != std::string::npos || n.find("alphaholdout") != std::string::npos) return 2;
    if (n.find("additive") != std::string::npos) return 3;
    if (n.find("modulate") != std::string::npos) return 4;
    return 0;
}
static void MaterialLookRec(PakSet& ps, const std::string& matPkg, Mat& m, std::string& opacTex, int depth) {
    Package P; if (depth > 5 || !LoadPackage(ps, matPkg, P)) return;
    const Exp* e = nullptr; std::string cls;
    for (auto& x : P.exports) { std::string c = P.ClassOf(x); if (c.find("MaterialInstance") != std::string::npos || c == "Material") { e = &x; cls = c; break; } }
    if (!e) return;
    Rd r(P.Data(), (size_t)P.DataOff(*e)); Props pr = ReadProps(P, r, (size_t)P.DataOff(*e) + (size_t)e->size);
    auto fl = [&](const Tag* t, float& out) { if (t && t->size == 4) { Rd rf(P.Data(), t->at); out = rf.get<float>(); return true; } return false; };
    if (cls == "Material") {     // the base material: whatever isn't set is the engine default (opaque, one-sided, 0.3333)
        if (m.blend < 0) m.blend = std::max(0, BlendOf(P, Find(pr, "BlendMode")));
        if (m.twoSided < 0) { auto t = Find(pr, "TwoSided"); m.twoSided = t ? t->boolVal : 0; }
        float c; if (fl(Find(pr, "OpacityMaskClipValue"), c) && m.clip == 0.3333f) m.clip = c;
        // see-through with no opacity parameter: an opacity / alpha / mask texture used inside the material itself
        if (m.blend > 0 && opacTex.empty()) {
            int best = 0;
            for (int i = 0; i < (int)P.imports.size(); i++) if (P.imports[i].cls == "Texture2D") {
                std::string n = LowerA(P.imports[i].name); int sc = 0;
                if (n.find("opacity") != std::string::npos) sc = 3; else if (n.find("alpha") != std::string::npos) sc = 2;
                else if (n.size() > 5 && (n.compare(n.size() - 5, 5, "_mask") == 0 || n.compare(n.size() - 2, 2, "_a") == 0 || n.compare(n.size() - 3, 3, "_op") == 0)) sc = 1;
                if (n.find("noise") != std::string::npos || n.find("dither") != std::string::npos || n.find("blue") != std::string::npos) sc = 0;
                if (sc > best) { best = sc; opacTex = P.ImportPackage(-i - 1); }
            }
        }
        return;
    }
    if (auto bo = Find(pr, "BasePropertyOverrides")) {
        Rd rb(P.Data(), bo->at); Props b = ReadProps(P, rb, bo->at + bo->size);
        auto on = [&](const char* n) { auto t = Find(b, n); return t && t->boolVal; };
        if (m.blend < 0 && on("bOverride_BlendMode")) m.blend = std::max(0, BlendOf(P, Find(b, "BlendMode")));
        if (m.twoSided < 0 && on("bOverride_TwoSided")) { auto t = Find(b, "TwoSided"); m.twoSided = t ? t->boolVal : 0; }
        float c; if (on("bOverride_OpacityMaskClipValue") && fl(Find(b, "OpacityMaskClipValue"), c)) m.clip = c;
    }
    auto pname = [&](const Props& el) {
        if (auto pi = Find(el, "ParameterInfo")) { Rd ri(P.Data(), pi->at); Props ip = ReadProps(P, ri, pi->at + pi->size); return NameAt(P, Find(ip, "Name")); }
        return NameAt(P, Find(el, "ParameterName"));
    };
    if (auto t = Find(pr, "ScalarParameterValues")) for (auto& el : StructArray(P, *t)) {
        std::string n = LowerA(pname(el)); float v;
        if ((n == "opacity" || n == "opacity amount" || n == "opacityamount" || n == "alpha") && fl(Find(el, "ParameterValue"), v) && m.opacity == 1) m.opacity = std::min(1.f, std::max(0.f, v));
    }
    if (opacTex.empty()) if (auto t = Find(pr, "TextureParameterValues")) for (auto& el : StructArray(P, *t)) {
        std::string n = LowerA(pname(el)); int obj = ObjAt(P, Find(el, "ParameterValue"));
        if (obj < 0 && (n.find("opacity") != std::string::npos || n == "alpha" || n == "alphamask" || n == "alpha mask")) { opacTex = P.ImportPackage(obj); break; }
    }
    if (auto par = Find(pr, "Parent")) { int o = ObjAt(P, par); if (o < 0) MaterialLookRec(ps, P.ImportPackage(o), m, opacTex, depth + 1); }
}
static void MaterialLook(PakSet& ps, const std::string& matPkg, Mat& m, std::string& note) {
    std::string opacTex; MaterialLookRec(ps, matPkg, m, opacTex, 0);
    if (m.blend < 0) m.blend = 0;
    if (m.twoSided < 0) m.twoSided = 0;
    if (m.blend != 0 && !opacTex.empty()) m.opac = LoadGameTexture(ps, opacTex, note, 2048);
}
// the colour texture of a material (instance), following parent instances
static std::string MaterialColourTexture(PakSet& ps, const std::string& matPkg, double factor[3], int depth = 0) {
    Package P; if (depth > 4 || !LoadPackage(ps, matPkg, P)) return "";
    const Exp* e = nullptr; for (auto& x : P.exports) { std::string c = P.ClassOf(x); if (c.find("MaterialInstance") != std::string::npos || c == "Material") { e = &x; break; } }
    if (!e) return "";
    Rd r(P.Data(), (size_t)P.DataOff(*e)); Props pr = ReadProps(P, r, (size_t)P.DataOff(*e) + (size_t)e->size);
    int best = 0; std::string bestPkg;
    if (auto t = Find(pr, "TextureParameterValues")) for (auto& el : StructArray(P, *t)) {
        std::string pname;
        if (auto pi = Find(el, "ParameterInfo")) { Rd ri(P.Data(), pi->at); Props ip = ReadProps(P, ri, pi->at + pi->size); pname = NameAt(P, Find(ip, "Name")); }
        else pname = NameAt(P, Find(el, "ParameterName"));
        int obj = ObjAt(P, Find(el, "ParameterValue"));
        if (obj >= 0) continue;
        int sc = std::max(ColourScore(pname), ColourScore(P.ObjName(obj)) - 1);
        if (sc > best) { best = sc; bestPkg = P.ImportPackage(obj); }
    }
    if (auto t = Find(pr, "VectorParameterValues")) for (auto& el : StructArray(P, *t)) {   // a plain colour (used if there's no texture)
        std::string pname;
        if (auto pi = Find(el, "ParameterInfo")) { Rd ri(P.Data(), pi->at); Props ip = ReadProps(P, ri, pi->at + pi->size); pname = NameAt(P, Find(ip, "Name")); }
        auto v = Find(el, "ParameterValue");
        if (v && v->size >= 16 && TintScore(pname) > 0) {     // tints multiply together
            Rd rv(P.Data(), v->at); for (int k = 0; k < 3; k++) factor[k] *= std::min(1.0, std::max(0.0, (double)rv.get<float>()));
        }
    }
    if (!bestPkg.empty()) return bestPkg;
    if (auto par = Find(pr, "Parent")) { int o = ObjAt(P, par); if (o < 0) { std::string pp = P.ImportPackage(o); std::string got = MaterialColourTexture(ps, pp, factor, depth + 1); if (!got.empty()) return got; } }
    // last resort: any colour-looking texture this material uses
    for (int i = 0; i < (int)P.imports.size(); i++) if (P.imports[i].cls == "Texture2D" && ColourScore(P.imports[i].name) > best) { best = ColourScore(P.imports[i].name); bestPkg = P.ImportPackage(-i - 1); }
    return best >= 6 ? bestPkg : "";
}
// ---------------------------------------------------------------- animations straight from the game's files
// An AnimSequence holds, per bone track, keys compressed either with Unreal's own codecs (constant / variable key
// lerp, per-track compression) or with ACL (Animation Compression Library, used by many newer games). Both are decoded
// here into each bone's local transform per frame, keyed by bone name so every piece using that skeleton can follow.
struct DecodedAnim { std::string name; double fps = 30; int frames = 0; std::unordered_map<std::string, std::vector<Xf>> bones; };
struct SkelRef { std::vector<std::string> names; std::vector<Xf> pose; };
static bool LoadSkeletonRef(PakSet& ps, const std::string& skelPkg, SkelRef& out) {
    static std::map<std::string, SkelRef> cache;
    auto it = cache.find(LowerA(skelPkg)); if (it != cache.end()) { out = it->second; return !out.names.empty(); }
    SkelRef& sr = cache[LowerA(skelPkg)];
    Package P; if (!LoadPackage(ps, skelPkg, P)) return false;
    for (auto& x : P.exports) if (P.ClassOf(x) == "Skeleton") {
        const std::string& D = P.Data(); size_t st = (size_t)P.DataOff(x), en = st + (size_t)x.size;
        if (en > D.size()) return false;
        Rd r(D, st); ReadProps(P, r, en); if (r.get<int>()) r.skip(16);
        int nb = r.get<int>(); if (r.bad || nb <= 0 || nb > 4000) return false;
        for (int i = 0; i < nb && !r.bad; i++) { sr.names.push_back(LowerA(P.N(r))); r.get<int>(); }
        int np = r.get<int>(); if (np != nb) { sr.names.clear(); return false; }
        for (int i = 0; i < np && !r.bad; i++) { float f[10]; for (auto& v : f) v = r.get<float>(); sr.pose.push_back({ qnorm({ f[0], f[1], f[2], f[3] }), { f[4], f[5], f[6] } }); }
        if (r.bad) { sr.names.clear(); sr.pose.clear(); return false; }
        break;
    }
    out = sr; return !out.names.empty();
}
static Q Slerp(Q a, Q b, double t) {
    double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b = { -b.x, -b.y, -b.z, -b.w }; d = -d; }
    if (d > 0.9995) return qnorm({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
    double th = std::acos(d), s0 = std::sin((1 - t) * th) / std::sin(th), s1 = std::sin(t * th) / std::sin(th);
    return qnorm({ a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1, a.w * s0 + b.w * s1 });
}
// keys of one track: values plus (optionally) the frame each key is at
struct KeysV { std::vector<V3> v; std::vector<double> t; };
struct KeysQ { std::vector<Q> v; std::vector<double> t; };
static double Dec16(uint16_t x) { return (x - 32767) / 32767.0; }
static double Dec11(uint32_t x) { return ((int)x - 1023) / 1023.0; }
static double Dec10(uint32_t x) { return ((int)x - 511) / 511.0; }
static Q QW(double x, double y, double z) { double w2 = 1 - x * x - y * y - z * z; return { x, y, z, w2 > 0 ? std::sqrt(w2) : 0 }; }
static float F32Bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static Q ReadQuatKey(Rd& r, int fmt, int mask, const double mn[3], const double rg[3]) {
    switch (fmt) {
    case 0: { float f[4]; for (auto& v : f) v = r.get<float>(); return { f[0], f[1], f[2], f[3] }; }   // None (only in key lerp)
    case 1: { float x = r.get<float>(), y = r.get<float>(), z = r.get<float>(); return QW(x, y, z); }
    case 2: { double x = (mask & 1) ? Dec16(r.get<uint16_t>()) : 0, y = (mask & 2) ? Dec16(r.get<uint16_t>()) : 0, z = (mask & 4) ? Dec16(r.get<uint16_t>()) : 0; return QW(x, y, z); }
    case 3: { uint32_t p = r.get<uint32_t>(); return QW(Dec11(p >> 21) * rg[0] + mn[0], Dec11((p & 0x1ffc00) >> 10) * rg[1] + mn[1], Dec10(p & 0x3ff) * rg[2] + mn[2]); }
    case 4: { uint32_t p = r.get<uint32_t>(); return QW(Dec11(p >> 21), Dec11((p & 0x1ffc00) >> 10), Dec10(p & 0x3ff)); }
    case 5: { uint32_t p = r.get<uint32_t>(); uint32_t ux = p >> 21, uy = (p & 0x1ffc00) >> 10, uz = p & 0x3ff;
              double x = F32Bits(((((ux >> 7) & 7) + 123) << 23) | ((ux & 0x7F | 32 * (ux & 0xFFFFFC00)) << 16));
              double y = F32Bits(((((uy >> 7) & 7) + 123) << 23) | ((uy & 0x7F | 32 * (uy & 0xFFFFFC00)) << 16));
              double z = F32Bits(((((uz >> 6) & 7) + 123) << 23) | ((uz & 0x3F | 32 * (uz & 0xFFFFFE00)) << 17));
              return QW(x, y, z); }
    default: return {};
    }
}
static void ReadTimes(Rd& r, int n, int frames, std::vector<double>& t) {
    t.resize(n); if (n <= 1) return;
    for (int i = 0; i < n; i++) t[i] = frames < 256 ? (double)r.get<uint8_t>() : (double)r.get<uint16_t>();
    r.o = (r.o + 3) & ~(size_t)3;
}
static void PerTrackV(Rd& r, int frames, KeysV& k, bool scale) {
    uint32_t info = r.get<uint32_t>(); int fmt = info >> 28, mask = (info >> 24) & 0xF, n = info & 0xFFFFFF; bool times = mask & 8;
    double mn[3] = {}, rg[3] = {};
    if (fmt == 3) for (int c = 0; c < 3; c++) if (mask & (1 << c)) { mn[c] = r.get<float>(); rg[c] = r.get<float>(); }
    if (n > 100000) { r.bad = true; return; }
    for (int i = 0; i < n && !r.bad; i++) {
        V3 v;
        if (fmt == 0 || fmt == 1) { if (mask & 7) { v.x = (mask & 1) ? r.get<float>() : 0; v.y = (mask & 2) ? r.get<float>() : 0; v.z = (mask & 4) ? r.get<float>() : 0; } else { v.x = r.get<float>(); v.y = r.get<float>(); v.z = r.get<float>(); } }
        else if (fmt == 3) { uint32_t p = r.get<uint32_t>(); v = { Dec10(p & 0x3ff) * rg[0] + mn[0], Dec11((p & 0x1ffc00) >> 10) * rg[1] + mn[1], Dec11(p >> 21) * rg[2] + mn[2] }; }
        else if (fmt == 2) { auto d = [&](uint16_t x) { return (x - 255) / 1.0; }; v.x = (mask & 1) ? d(r.get<uint16_t>()) : 0; v.y = (mask & 2) ? d(r.get<uint16_t>()) : 0; v.z = (mask & 4) ? d(r.get<uint16_t>()) : 0; }
        else if (fmt == 6) v = scale ? V3{ 1, 1, 1 } : V3{};
        k.v.push_back(v);
    }
    r.o = (r.o + 3) & ~(size_t)3;
    if (times) ReadTimes(r, n, frames, k.t);
}
static void PerTrackQ(Rd& r, int frames, KeysQ& k) {
    uint32_t info = r.get<uint32_t>(); int fmt = info >> 28, mask = (info >> 24) & 0xF, n = info & 0xFFFFFF; bool times = mask & 8;
    double mn[3] = {}, rg[3] = {};
    if (fmt == 3) for (int c = 0; c < 3; c++) if (mask & (1 << c)) { mn[c] = r.get<float>(); rg[c] = r.get<float>(); }
    if (n > 100000) { r.bad = true; return; }
    for (int i = 0; i < n && !r.bad; i++) k.v.push_back(fmt == 6 ? Q{} : fmt == 0 ? ReadQuatKey(r, 1, 7, mn, rg) : ReadQuatKey(r, fmt, fmt == 2 ? mask : 7, mn, rg));
    r.o = (r.o + 3) & ~(size_t)3;
    if (times) ReadTimes(r, n, frames, k.t);
}
template <class T, class L> static T SampleKeys(const std::vector<T>& v, const std::vector<double>& t, int frame, int frames, L lerp) {
    if (v.size() == 1) return v[0];
    double pos;
    if (!t.empty() && t.size() == v.size()) {           // keys at given frames
        size_t i = 0; while (i + 1 < t.size() && t[i + 1] <= frame) i++;
        if (i + 1 >= t.size()) return v.back();
        double span = t[i + 1] - t[i]; double a = span > 0 ? (frame - t[i]) / span : 0;
        return lerp(v[i], v[i + 1], std::min(1.0, std::max(0.0, a)));
    }
    pos = frames > 1 ? (double)frame * (v.size() - 1) / (frames - 1) : 0;   // keys spread evenly
    size_t i = (size_t)pos; if (i + 1 >= v.size()) return v.back();
    return lerp(v[i], v[i + 1], pos - (double)i);
}
struct AclWriter final : public acl::track_writer {
    const std::vector<Xf>* ref; const std::vector<int>* map; std::vector<std::vector<Xf>>* out; uint32_t frame = 0;
    static constexpr acl::default_sub_track_mode get_default_rotation_mode() { return acl::default_sub_track_mode::variable; }
    static constexpr acl::default_sub_track_mode get_default_translation_mode() { return acl::default_sub_track_mode::variable; }
    static constexpr acl::default_sub_track_mode get_default_scale_mode() { return acl::default_sub_track_mode::variable; }
    Xf RefOf(uint32_t t) const { int b = t < map->size() ? (*map)[t] : -1; return b >= 0 && b < (int)ref->size() ? (*ref)[b] : Xf{}; }
    rtm::quatf RTM_SIMD_CALL get_variable_default_rotation(uint32_t t) const { Q q = RefOf(t).r; return rtm::quat_set((float)q.x, (float)q.y, (float)q.z, (float)q.w); }
    rtm::vector4f RTM_SIMD_CALL get_variable_default_translation(uint32_t t) const { V3 v = RefOf(t).t; return rtm::vector_set((float)v.x, (float)v.y, (float)v.z); }
    rtm::vector4f RTM_SIMD_CALL get_variable_default_scale(uint32_t) const { return rtm::vector_set(1.0f); }
    void RTM_SIMD_CALL write_rotation(uint32_t t, rtm::quatf_arg0 q) { if (t < out->size()) (*out)[t][frame].r = qnorm({ rtm::quat_get_x(q), rtm::quat_get_y(q), rtm::quat_get_z(q), rtm::quat_get_w(q) }); }
    void RTM_SIMD_CALL write_translation(uint32_t t, rtm::vector4f_arg0 v) { if (t < out->size()) (*out)[t][frame].t = { rtm::vector_get_x(v), rtm::vector_get_y(v), rtm::vector_get_z(v) }; }
    void RTM_SIMD_CALL write_scale(uint32_t, rtm::vector4f_arg0) {}
};
static bool DecodeAnim(PakSet& ps, const std::string& animPkg, DecodedAnim& out, std::string& err, int maxFrames = 300) {
    Package P; if (!LoadPackage(ps, animPkg, P)) { err = P.err; return false; }
    if (getenv("UEX_DEBUG")) fprintf(stderr, "  loaded package %zu bytes\n", P.Data().size());
    const Exp* e = nullptr; for (auto& x : P.exports) if (P.ClassOf(x) == "AnimSequence") { e = &x; break; }
    if (!e) { err = "not an animation"; return false; }
    const std::string& D = P.Data(); size_t st = (size_t)P.DataOff(*e), en = st + (size_t)e->size;
    if (en > D.size()) { err = "animation data cut short"; return false; }
    Rd r(D, st); Props pr = ReadProps(P, r, en);
    if (auto t = Find(pr, "AdditiveAnimType")) { std::string v = t->size == 8 ? NameAt(P, t) : ""; if (!v.empty() && v.find("None") == std::string::npos) { err = "additive (layered) animation"; return false; } }
    int numFrames = 0; float length = 0;
    if (auto t = Find(pr, "NumFrames")) memcpy(&numFrames, D.data() + t->at, 4);
    if (auto t = Find(pr, "SequenceLength")) memcpy(&length, D.data() + t->at, 4);
    std::string skel; { int so = ObjAt(P, Find(pr, "Skeleton")); if (so < 0) skel = P.ImportPackage(so); }
    if (getenv("UEX_DEBUG")) fprintf(stderr, "  props ok, frames %d length %.2f skeleton %s\n", numFrames, length, skel.c_str());
    SkelRef sk; if (skel.empty() || !LoadSkeletonRef(ps, skel, sk)) { err = "couldn't read its skeleton"; return false; }
    if (getenv("UEX_DEBUG")) fprintf(stderr, "  skeleton %zu bones\n", sk.names.size());
    // after the properties: object GUID flag, (a GUID), strip flags, "has compressed data", then the compressed data
    if (r.get<int>()) r.skip(16);
    { uint16_t sf; memcpy(&sf, D.data() + r.o, 2); int b; memcpy(&b, D.data() + r.o + 2, 4); if (!((sf & 0xFF) <= 3 && (b == 0 || b == 1))) r.skip(16); }
    r.skip(2);
    if (!r.get<int>()) { err = "no animation data"; return false; }
    r.get<int>();                                                        // raw size
    int nmap = r.get<int>(); if (r.bad || nmap < 0 || nmap > 10000) { err = "unexpected animation layout"; return false; }
    std::vector<int> track(nmap); for (auto& v : track) v = r.get<int>();
    int ncur = r.get<int>(); if (ncur < 0 || ncur > 100000) { err = "unexpected animation layout (curves)"; return false; } r.skip(8ull * ncur);
    int nbytes = r.get<int>(); int useBulk = r.get<int>();
    std::string stream;
    if (nbytes < 0) { err = "unexpected animation layout (data)"; return false; }
    if (!useBulk) { if (r.o + nbytes > D.size()) { err = "animation data cut short"; return false; } stream = D.substr(r.o, nbytes); r.skip(nbytes); }
    else {
        uint32_t fl = r.get<uint32_t>(); long long cnt, disk; if (fl & 0x2000) { cnt = r.get<long long>(); disk = r.get<long long>(); } else { cnt = r.get<int>(); disk = r.get<int>(); }
        long long off = r.get<long long>(); (void)cnt;
        if (fl & 0x40) { if (r.o + disk > D.size()) { err = "animation data cut short"; return false; } stream = D.substr(r.o, (size_t)disk); r.skip((size_t)disk); }
        else if ((fl & 0x100) && off >= 0 && off + disk <= (long long)P.bulk.size()) stream = P.bulk.substr((size_t)off, (size_t)disk);
        else if (off >= 0 && off + disk <= (long long)D.size() && !(fl & 0x100)) stream = D.substr((size_t)off, (size_t)disk);
        else { err = "couldn't find the animation's data"; return false; }
        if ((long long)stream.size() > nbytes) stream.resize(nbytes);
    }
    std::string codec = r.fstr(); r.fstr();
    int ncb = r.get<int>(); if (ncb < 0) { err = "unexpected animation layout"; return false; } r.skip(ncb);
    if (r.bad) { err = "unexpected animation layout"; return false; }
    std::string lc = LowerA(codec);
    if (getenv("UEX_DEBUG")) fprintf(stderr, "  codec [%s] tracks %d bytes %zu bulk %d\n", codec.c_str(), nmap, stream.size(), useBulk);
    std::vector<std::vector<Xf>> keys;                  // [track][frame] local transforms
    int frames = 0;
    if (lc.find("acl") != std::string::npos) {
        r.get<int>();                                      // frames (ACL keeps its own count)
        std::vector<uint8_t> buf(stream.size() + 32);
        uint8_t* al = (uint8_t*)(((uintptr_t)buf.data() + 15) & ~(uintptr_t)15); memcpy(al, stream.data(), stream.size());
        const acl::compressed_tracks* ct = acl::make_compressed_tracks(al);
        if (!ct || ct->is_valid(false).any()) { err = "couldn't read its ACL data"; return false; }
        uint32_t ns = ct->get_num_samples_per_track(), nt = ct->get_num_tracks();
        float rate = ct->get_sample_rate(), dur = ct->get_finite_duration();
        int stride = rate > 31 ? (int)std::lround(rate / 30) : 1;      // at most ~30 frames a second
        frames = (int)std::min<uint32_t>((ns + stride - 1) / stride, (uint32_t)maxFrames);
        std::vector<int> map(nt, -1); for (uint32_t t = 0; t < nt && t < track.size(); t++) map[t] = track[t];
        keys.assign(nt, std::vector<Xf>(frames));
        acl::decompression_context<acl::decompression_settings> ctx; ctx.initialize(*ct);
        AclWriter w; w.ref = &sk.pose; w.map = &map; w.out = &keys;
        for (int f = 0; f < frames; f++) { w.frame = f; ctx.seek(std::min((float)(f * stride) / rate, dur), acl::sample_rounding_policy::nearest); ctx.decompress_tracks(w); }
        out.fps = rate > 0 ? rate / stride : 30;
    } else {
        // Unreal's own codecs
        int nf = r.get<int>();
        uint8_t keyFmt = r.get<uint8_t>(), tf = r.get<uint8_t>(), rf = r.get<uint8_t>(), sf = r.get<uint8_t>();
        int nByteStream = r.get<int>(), nOff = r.get<int>(), nScale = r.get<int>(), strip = r.get<int>();
        if (getenv("UEX_DEBUG")) fprintf(stderr, "  ue codec frames %d key %d fmts %d %d %d bytes %d offs %d scale %d strip %d\n", nf, keyFmt, tf, rf, sf, nByteStream, nOff, nScale, strip);
        if (r.bad || nOff < 0 || nScale < 0 || nByteStream < 0 || (size_t)4 * (nOff + nScale) + nByteStream > stream.size()) { err = "unexpected animation layout (" + codec + ")"; return false; }
        std::vector<int> offs(nOff), soffs(nScale);
        memcpy(offs.data(), stream.data(), 4ull * nOff); memcpy(soffs.data(), stream.data() + 4ull * nOff, 4ull * nScale);
        std::string bs = stream.substr(4ull * (nOff + nScale), (size_t)nByteStream);
        int total = nf > 0 ? nf : numFrames;
        double srcFps = (total > 1 && length > 0) ? (total - 1) / length : 30;
        int stride = srcFps > 31 ? (int)std::lround(srcFps / 30) : 1;    // at most ~30 frames a second
        frames = std::min(std::max(1, (total + stride - 1) / stride), maxFrames);
        bool scaleOk = strip > 0 && nScale > 0;
        keys.assign(nmap, std::vector<Xf>(frames));
        for (int ti = 0; ti < nmap; ti++) {
            int bone = track[ti];
            Xf refx = bone >= 0 && bone < (int)sk.pose.size() ? sk.pose[bone] : Xf{};
            KeysV kt; KeysQ kr;
            if (keyFmt == 2) {                             // per-track compression
                if (2 * ti + 1 >= nOff) break;
                int to = offs[2 * ti], ro = offs[2 * ti + 1];
                if (to >= 0) { Rd q(bs, (size_t)to); PerTrackV(q, total, kt, false); }
                if (ro >= 0) { Rd q(bs, (size_t)ro); PerTrackQ(q, total, kr); }
                (void)scaleOk;
            } else {                                       // constant / variable key lerp
                if (4 * ti + 3 >= nOff) break;
                int to = offs[4 * ti], tn = offs[4 * ti + 1], ro = offs[4 * ti + 2], rn = offs[4 * ti + 3];
                bool timed = keyFmt == 1;
                if (tn > 0 && tn < 100000 && to >= 0) {
                    Rd q(bs, (size_t)to); int fmt = tn == 1 ? 0 : tf; double mn[3] = {}, rg[3] = {};
                    if (fmt == 3) { for (int c = 0; c < 3; c++) mn[c] = q.get<float>(); for (int c = 0; c < 3; c++) rg[c] = q.get<float>(); }
                    for (int k = 0; k < tn && !q.bad; k++) {
                        V3 v;
                        if (fmt == 0 || fmt == 1) { v.x = q.get<float>(); v.y = q.get<float>(); v.z = q.get<float>(); }
                        else if (fmt == 3) { uint32_t p = q.get<uint32_t>(); v = { Dec10(p & 0x3ff) * rg[0] + mn[0], Dec11((p & 0x1ffc00) >> 10) * rg[1] + mn[1], Dec11(p >> 21) * rg[2] + mn[2] }; }
                        else if (fmt == 2) { v.x = Dec16(q.get<uint16_t>()) * 128; v.y = Dec16(q.get<uint16_t>()) * 128; v.z = Dec16(q.get<uint16_t>()) * 128; }
                        kt.v.push_back(v);
                    }
                    q.o = (q.o + 3) & ~(size_t)3; if (timed) ReadTimes(q, tn, total, kt.t);
                }
                if (rn > 0 && rn < 100000 && ro >= 0) {
                    Rd q(bs, (size_t)ro); int fmt = rn == 1 ? 1 : rf; double mn[3] = {}, rg[3] = {};
                    if (fmt == 3) { for (int c = 0; c < 3; c++) mn[c] = q.get<float>(); for (int c = 0; c < 3; c++) rg[c] = q.get<float>(); }
                    for (int k = 0; k < rn && !q.bad; k++) kr.v.push_back(fmt == 6 ? Q{} : ReadQuatKey(q, fmt, 7, mn, rg));
                    q.o = (q.o + 3) & ~(size_t)3; if (timed) ReadTimes(q, rn, total, kr.t);
                }
            }
            for (int f = 0; f < frames; f++) {
                Xf x = refx; int fi = std::min(f * stride, std::max(0, total - 1));
                if (!kr.v.empty()) x.r = qnorm(SampleKeys(kr.v, kr.t, fi, total, [](Q a, Q b, double t) { return Slerp(a, b, t); }));
                if (!kt.v.empty()) x.t = SampleKeys(kt.v, kt.t, fi, total, [](V3 a, V3 b, double t) { return V3{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t }; });
                keys[ti][f] = x;
            }
        }
        out.fps = srcFps / stride;
    }
    if (frames <= 0) { err = "empty animation"; return false; }
    out.frames = frames;
    out.name = e->name;
    if (getenv("UEX_DEBUG")) {
        double sum = 0, mx = 0; int n = 0; double tsum = 0;
        for (size_t ti = 0; ti < keys.size() && ti < track.size(); ti++) {
            int b = track[ti]; if (b < 0 || b >= (int)sk.pose.size() || keys[ti].empty()) continue;
            Q a = keys[ti][0].r, c = sk.pose[b].r; double d = std::fabs(a.x * c.x + a.y * c.y + a.z * c.z + a.w * c.w); d = std::min(1.0, d);
            double ang = 2 * std::acos(d) * 57.2958; sum += ang; mx = std::max(mx, ang); n++;
            V3 dt = sub(keys[ti][0].t, sk.pose[b].t); tsum += std::sqrt(dt.x * dt.x + dt.y * dt.y + dt.z * dt.z);
        }
        fprintf(stderr, "  frame 0 vs reference pose: average turn %.1f deg (max %.1f), average move %.2f over %d bones\n", n ? sum / n : 0, mx, n ? tsum / n : 0, n);
    }
    for (size_t ti = 0; ti < keys.size() && ti < track.size(); ti++) {
        int b = track[ti]; if (b < 0 || b >= (int)sk.names.size()) continue;
        out.bones[sk.names[b]] = std::move(keys[ti]);
    }
    return !out.bones.empty();
}
// the game's animations for a skeleton: animation packages that name it (checked on the start of each file only)
static std::vector<std::string> FindAnims(PakSet& ps, const std::string& skelPkg, const std::string& meshPkg, int maxCount, int maxChecks = 2500) {
    std::string skel = LowerA(skelPkg); if (skel.empty()) return {};
    std::set<std::string> tokens;                         // folder / name words shared with the mesh rank higher
    for (auto src : { meshPkg, skelPkg }) { std::string w; for (char c : LowerA(src) + "/") { if (c == '/' || c == '_') { if (w.size() > 2 && w != "game" && w != "mesh" && w != "meshes" && w != "character" && w != "characters") tokens.insert(w); w.clear(); } else w += c; } }
    struct C { std::string pkg; int score; };
    std::vector<C> cands;
    auto consider = [&](const std::string& key) {
        if (key.size() < 8 || key.compare(key.size() - 7, 7, ".uasset") != 0) return;
        std::string pkg = key.substr(0, key.size() - 7);
        std::string file = pkg.substr(pkg.find_last_of('/') + 1);
        bool animish = pkg.find("anim") != std::string::npos || file.rfind("a_", 0) == 0 || file.rfind("anim_", 0) == 0 || file.find("_anm") != std::string::npos || file.rfind("as_", 0) == 0;
        if (!animish) return;
        for (auto bad : { "lipsync", "/face", "facial", "montage", "_bs", "bs_", "blendspace", "aimoffset", "_ao_", "additive", "_add", "pose", "/cine", "cinematic", "_mtg", "_abp", "abp_", "_bp", "bp_", "curve", "notify" })
            if (pkg.find(bad) != std::string::npos) return;
        int sc = 0;
        for (auto& t : tokens) if (pkg.find(t) != std::string::npos) sc += 3;
        const char* good[] = { "idle", "walk", "run", "jog", "sprint", "jump", "attack", "talk", "sit", "turn", "dance", "wave" };
        for (int i = 0; i < 12; i++) if (file.find(good[i]) != std::string::npos) { sc += 12 - i; break; }
        cands.push_back({ pkg, sc });
    };
    for (auto& f : ps.files) consider(f.first);
    for (auto& f : ps.ioFiles) consider(f.first);
    std::sort(cands.begin(), cands.end(), [](const C& a, const C& b) { return a.score != b.score ? a.score > b.score : a.pkg < b.pkg; });
    std::vector<std::string> out; std::set<std::string> kinds;
    int checks = 0;
    for (auto& c : cands) {
        if ((int)out.size() >= maxCount || checks++ >= maxChecks) break;
        std::string head, err;
        auto io = ps.ioFiles.find(c.pkg + ".uasset");
        bool ok = io != ps.ioFiles.end() ? ReadIoChunk(ps, io->second, head, err, 65536) : ReadFileFromPak(ps, c.pkg + ".uasset", head, err);
        if (!ok || LowerA(head).find(skel) == std::string::npos) continue;
        // one of each kind first (idle, walk, run...), so the list isn't six idles
        std::string file = c.pkg.substr(c.pkg.find_last_of('/') + 1), kind = "other";
        const char* good[] = { "idle", "walk", "run", "jog", "sprint", "jump", "attack", "talk", "sit", "turn", "dance", "wave" };
        for (auto g : good) if (file.find(g) != std::string::npos) { kind = g; break; }
        if (kinds.count(kind) && (int)cands.size() > maxCount * 3 && kind != "other") continue;
        kinds.insert(kind);
        out.push_back(c.pkg);
    }
    return out;
}
// static mesh -> Src
static bool LoadStaticMesh(PakSet& ps, const std::string& objPath, Src& s, std::string& err, std::string& note) {
    Package P; if (!LoadPackage(ps, objPath, P)) { err = P.err; return false; }
    const Exp* e = nullptr; for (auto& x : P.exports) if (P.ClassOf(x) == "StaticMesh") { e = &x; break; }
    if (!e) { err = "that isn't a static mesh"; return false; }
    const std::string& D = P.Data(); size_t start = (size_t)P.DataOff(*e), end = start + (size_t)e->size;
    if (end > D.size()) { err = "the mesh data is cut short"; return false; }
    Rd r(D, start); Props pr = ReadProps(P, r, end);
    if (r.get<int>()) r.skip(16);
    uint8_t gstrip = r.get<uint8_t>(); r.get<uint8_t>();
    int cooked = r.get<int>(); r.get<int>(); r.get<int>();
    if (!(gstrip & 1) || !cooked) { err = "the mesh isn't in cooked game form"; return false; }
    r.skip(16); int nsock = r.get<int>(); if (nsock < 0 || nsock > 10000) { err = "unexpected mesh layout"; return false; } r.skip(4ull * nsock);
    // render data: LODs
    int nlod = r.get<int>(); if (r.bad || nlod <= 0 || nlod > 16) { err = "unexpected mesh layout (LODs)"; return false; }
    r.skip(2);
    int nsec = r.get<int>(); if (nsec <= 0 || nsec > 1000) { err = "unexpected mesh layout (sections)"; return false; }
    struct Sec { int mat, first, tris, minv, maxv; };
    std::vector<Sec> secs;
    size_t secStart = r.o; int secSize = 0;
    for (int cand : { 36, 32, 28, 40, 44 }) {   // the section record grew over engine versions; find the size that fits
        bool okc = true; long long expect = 0;
        for (int i = 0; i < nsec; i++) {
            if (secStart + (size_t)(i + 1) * cand > D.size()) { okc = false; break; }
            int v[5]; memcpy(v, D.data() + secStart + (size_t)i * cand, 20);
            if (v[0] < 0 || v[0] > 255 || v[1] != expect || v[2] < 0 || v[3] > v[4] + 1) { okc = false; break; }
            expect = v[1] + 3ll * v[2];
        }
        if (okc) {
            size_t after = secStart + (size_t)nsec * cand + 4;   // MaxDeviation, then cooked-out / inlined flags (0 or 1)
            int a = 2, b = 2; if (after + 8 <= D.size()) { memcpy(&a, D.data() + after, 4); memcpy(&b, D.data() + after + 4, 4); }
            if ((a == 0 || a == 1) && (b == 0 || b == 1)) { secSize = cand; break; }
        }
    }
    if (!secSize) { err = "this game's mesh format isn't supported yet (" + std::to_string(P.ver) + ")"; return false; }
    for (int i = 0; i < nsec; i++) { int v[5]; memcpy(v, D.data() + secStart + (size_t)i * secSize, 20); secs.push_back({ v[0], v[1], v[2], v[3], v[4] }); }
    r.o = secStart + (size_t)nsec * secSize;
    r.get<float>(); int cookedOut = r.get<int>(), inlined = r.get<int>();
    if (cookedOut) { err = "the most detailed version of this mesh isn't in the game's files"; return false; }
    std::string streamed; const std::string* B = &D; size_t bo = r.o;
    if (!inlined) {   // the LOD's buffers are in the .ubulk file
        uint32_t fl = r.get<uint32_t>(); long long cnt, disk; if (fl & 0x2000) { cnt = r.get<long long>(); disk = r.get<long long>(); } else { cnt = r.get<int>(); disk = r.get<int>(); }
        long long off = r.get<long long>(); (void)cnt;
        if ((fl & 0x100) && off >= 0 && off + disk <= (long long)P.bulk.size()) streamed = P.bulk.substr((size_t)off, (size_t)disk);
        else if ((fl & 0x40) && r.o + disk <= D.size()) streamed = D.substr(r.o, (size_t)disk);
        else { err = "couldn't find the mesh's streamed data"; return false; }
        B = &streamed; bo = 0;
    }
    Rd q(*B, bo);
    q.skip(2);
    int stride = q.get<int>(), nv = q.get<int>(); (void)stride;
    int es = q.get<int>(), n = q.get<int>();
    if (q.bad || nv <= 0 || nv > 4000000 || es != 12 || n != nv || q.o + 12ull * nv > B->size()) { err = "unexpected vertex data"; return false; }
    std::vector<V3> pos(nv);
    for (int i = 0; i < nv; i++) { float f[3]; memcpy(f, B->data() + q.o + 12ull * i, 12); pos[i] = { f[0], f[1], f[2] }; }
    q.skip(12ull * nv);
    q.skip(2);
    uint32_t ntc = q.get<uint32_t>(), nv2 = q.get<uint32_t>(); int fullUV = q.get<int>(), hiTan = q.get<int>();
    int tes = q.get<int>(), tn = q.get<int>(); size_t tanAt = q.o; q.skip((size_t)tes * tn);
    int ues = q.get<int>(), un = q.get<int>(); size_t uvAt = q.o; q.skip((size_t)ues * un);
    if (q.bad || nv2 != (uint32_t)nv || ntc == 0 || ntc > 8) { err = "unexpected vertex attributes"; return false; }
    q.skip(2); q.get<int>(); int cnv = q.get<int>(); if (cnv > 0) { int ces = q.get<int>(), cn = q.get<int>(); q.skip((size_t)ces * cn); }
    int b32 = q.get<int>(); int ies = q.get<int>(), ibytes = q.get<int>();
    if (q.bad || ies != 1 || q.o + (size_t)ibytes > B->size()) { err = "unexpected index data"; return false; }
    std::vector<uint32_t> idx;
    if (b32) { idx.resize(ibytes / 4); memcpy(idx.data(), B->data() + q.o, idx.size() * 4); }
    else { idx.resize(ibytes / 2); for (size_t i = 0; i < idx.size(); i++) { uint16_t v; memcpy(&v, B->data() + q.o + 2 * i, 2); idx[i] = v; } }
    // fill Src
    s.pos = pos; s.nrm.resize(nv); s.u.resize(nv); s.v.resize(nv); s.vMat.assign(nv, 0); s.vBone.assign(nv, 0);
    for (int i = 0; i < nv; i++) {
        size_t ta = tanAt + (size_t)i * (hiTan ? 16 : 8) + (hiTan ? 8 : 4);   // second packed normal = TangentZ (the normal)
        double nn[3];
        for (int k = 0; k < 3; k++) { if (hiTan) { int16_t c; memcpy(&c, B->data() + ta + 2 * k, 2); nn[k] = c / 32767.0; } else { nn[k] = ((int8_t)(*B)[ta + k]) / 127.0; } }
        s.nrm[i] = { nn[0], nn[1], nn[2] };
        size_t ua = uvAt + (size_t)i * ntc * (fullUV ? 8 : 4);
        if (fullUV) { float f[2]; memcpy(f, B->data() + ua, 8); s.u[i] = f[0]; s.v[i] = f[1]; }
        else { uint16_t hh[2]; memcpy(hh, B->data() + ua, 4); s.u[i] = Half(hh[0]); s.v[i] = Half(hh[1]); }
    }
    // materials (one per section) and their colour textures
    std::vector<std::string> matPkgs;
    if (auto sm = Find(pr, "StaticMaterials")) for (auto& el : StructArray(P, *sm)) { int o = ObjAt(P, Find(el, "MaterialInterface")); matPkgs.push_back(o < 0 ? P.ImportPackage(o) : ""); }
    if (matPkgs.empty()) for (auto& im : P.imports) if (im.cls.find("Material") != std::string::npos && im.name.rfind("Default__", 0) != 0) {
        for (int i = 0; i < (int)P.imports.size(); i++) if (&P.imports[i] == &im) matPkgs.push_back(P.ImportPackage(-i - 1)); }
    std::map<int, int> matSlot;
    for (auto& sc : secs) {
        if (!matSlot.count(sc.mat)) {
            Mat m; m.name = sc.mat < (int)matPkgs.size() ? matPkgs[sc.mat] : "Material";
            if (sc.mat < (int)matPkgs.size() && !matPkgs[sc.mat].empty()) {
                std::string tex = MaterialColourTexture(ps, matPkgs[sc.mat], m.factor);
                if (!tex.empty()) { m.tex = LoadGameTexture(ps, tex, note); m.texPath = U2W(tex); }
                std::string nt = MaterialNormalTexture(ps, matPkgs[sc.mat]);
                if (!nt.empty()) m.nrm = LoadGameTexture(ps, nt, note);
                AttachMasks(ps, matPkgs[sc.mat], m, note);
                MaterialGlow(ps, matPkgs[sc.mat], m, note);
                MaterialLook(ps, matPkgs[sc.mat], m, note);
                { const std::string& pk = matPkgs[sc.mat]; size_t sl = pk.find_last_of('/'); m.orig = pk + "." + (sl == std::string::npos ? pk : pk.substr(sl + 1)); }
            }
            s.mats.push_back(m); matSlot[sc.mat] = (int)s.mats.size() - 1;
        }
        int mi = matSlot[sc.mat];
        for (long long t = sc.first; t < sc.first + 3ll * sc.tris && t < (long long)idx.size(); t++) {
            uint32_t v = idx[(size_t)t]; if (v >= (uint32_t)nv) { err = "bad index data"; return false; }
            s.idx.push_back(v); s.vMat[v] = mi;
        }
    }
    s.bones.push_back(Bone{ "Root" });
    return true;
}

// skeletal mesh -> Src (bind pose; one part per bone so imported animations could still move them)
static bool LoadSkeletalMesh(PakSet& ps, const std::string& objPath, Src& s, std::string& err, std::string& note) {
    Package P; if (!LoadPackage(ps, objPath, P)) { err = P.err; return false; }
    const Exp* e = nullptr; for (auto& x : P.exports) if (P.ClassOf(x) == "SkeletalMesh") { e = &x; break; }
    if (!e) { err = "that isn't a skeletal mesh"; return false; }
    const std::string& D = P.Data(); size_t start = (size_t)P.DataOff(*e), end = start + (size_t)e->size;
    if (end > D.size()) { err = "the mesh data is cut short"; return false; }
    Rd r(D, start); Props mprops = ReadProps(P, r, end);
    { int so = ObjAt(P, Find(mprops, "Skeleton")); if (so < 0) s.skeleton = P.ImportPackage(so); }
    if (r.get<int>()) r.skip(16);
    uint8_t gstrip = r.get<uint8_t>(); r.get<uint8_t>();
    if (!(gstrip & 1)) { err = "the mesh isn't in cooked game form"; return false; }
    r.skip(28);                                            // bounds
    int nm = r.get<int>(); if (r.bad || nm < 0 || nm > 1000) { err = "unexpected mesh layout (materials)"; return false; }
    std::vector<std::string> matPkgs;
    for (int i = 0; i < nm && !r.bad; i++) {
        int o = r.get<int>(); matPkgs.push_back(o < 0 ? P.ImportPackage(o) : "");
        P.N(r);                                            // slot name
        if (P.ver >= 516 || true) { int imp = r.get<int>(); if (imp == 1) P.N(r); else if (imp != 0) { r.o -= 4; } }
        r.skip(4 + 4 + 16);                                // UV channel data
    }
    int nb = r.get<int>(); if (r.bad || nb <= 0 || nb > 4000) { err = "unexpected mesh layout (skeleton)"; return false; }
    for (int i = 0; i < nb && !r.bad; i++) { Bone b; b.name = P.N(r); b.parent = r.get<int>(); s.bones.push_back(b); }
    int np = r.get<int>(); if (np != nb) { err = "unexpected mesh layout (bind pose)"; return false; }
    for (int i = 0; i < np && !r.bad; i++) {
        float f[10]; for (auto& x : f) x = r.get<float>();
        s.bones[i].bindLocal = { qnorm({ f[0], f[1], f[2], f[3] }), { f[4], f[5], f[6] } };
    }
    int nmap = r.get<int>(); if (nmap < 0 || nmap > 100000) { err = "unexpected mesh layout"; return false; } r.skip(12ull * nmap);
    for (size_t i = 0; i < s.bones.size(); i++) {
        int pa = s.bones[i].parent;
        s.bones[i].bindGlobal = pa >= 0 && pa < (int)i ? compose(s.bones[pa].bindGlobal, s.bones[i].bindLocal) : s.bones[i].bindLocal;
    }
    int cooked = r.get<int>(); int nlod = r.get<int>();
    if (r.bad || !cooked || nlod <= 0 || nlod > 16) { err = "the mesh isn't in cooked game form"; return false; }
    // LOD 0
    r.skip(2); int cookedOut = r.get<int>(), inlined = r.get<int>();
    int nreq = r.get<int>(); if (nreq < 0 || nreq > 4000) { err = "unexpected mesh layout (LOD)"; return false; } r.skip(2ull * nreq);
    if (cookedOut) { err = "the most detailed version of this mesh isn't in the game's files"; return false; }
    struct Sec { int mat, base, tris, basev, nv, maxinf; std::vector<uint16_t> boneMap; };
    std::vector<Sec> secs; size_t after = 0; std::string streamed; const std::string* B = &D;
    // sections: the record changed a little between engine versions (and some games add fields); try the known layouts
    auto parseSecs = [&](const std::string& buf, size_t at, int layout, std::vector<Sec>& got, size_t& endAt) -> bool {
        bool hasMask = layout & 1, hasRT = layout & 2, noDup = layout & 4;
        Rd q(buf, at); int ns = q.get<int>(); if (q.bad || ns <= 0 || ns > 1000) return false;
        got.clear();
        for (int i = 0; i < ns; i++) {
            Sec sc; q.skip(2); sc.mat = q.get<uint16_t>(); sc.base = q.get<int>(); sc.tris = q.get<int>();
            q.skip(4); if (hasMask) q.skip(1); q.skip(4); if (hasRT) q.skip(4);
            sc.basev = q.get<int>();
            int nc = q.get<int>(); if (nc < 0 || nc > 10000000) return false; q.skip(64ull * nc);   // cloth mapping (the cloth part exports as a normal part)
            int nbm = q.get<int>(); if (nbm < 0 || nbm > 4000) return false;
            for (int k = 0; k < nbm; k++) sc.boneMap.push_back(q.get<uint16_t>());
            sc.nv = q.get<int>(); sc.maxinf = q.get<int>(); q.skip(2 + 16 + 4);
            if (!noDup) { int dn = q.get<int>(); if (dn < 0 || dn > 10000000) return false; q.skip(4ull * dn); int di = q.get<int>(); if (di < 0 || di > 10000000) return false; q.skip(8ull * di); }
            q.skip(4);
            if (q.bad || sc.tris < 0 || sc.nv < 0 || sc.maxinf < 0 || sc.maxinf > 16) return false;
            got.push_back(sc);
        }
        endAt = q.o; return true;
    };
    auto findIndices = [&](const std::string& buf, size_t from, const std::vector<Sec>& got) -> size_t {
        long long total = 0; for (auto& sc : got) total += 3ll * sc.tris;
        for (size_t k = from; k < from + 65536 && k + 9 <= buf.size(); k++) {
            uint8_t ds = (uint8_t)buf[k]; int es, cnt; memcpy(&es, buf.data() + k + 1, 4); memcpy(&cnt, buf.data() + k + 5, 4);
            if ((ds == 2 || ds == 4) && es == ds && cnt == total) return k;
        }
        return 0;
    };
    // (1) Unreal 4.24+: sections in the mesh's own data, then active bones, a buffer size, then the LOD's buffers
    //     (right there when inlined, otherwise a pointer into the .ubulk file)
    for (int layout = 0; layout < 8 && secs.empty(); layout++) {
        std::vector<Sec> got; size_t endAt;
        if (!parseSecs(D, r.o, layout, got, endAt)) continue;
        Rd q(D, endAt); int nab = q.get<int>(); if (q.bad || nab < 0 || nab > 65536) continue; q.skip(2ull * nab); q.skip(4);
        if (inlined) { size_t k = findIndices(D, q.o, got); if (k) { secs = got; after = k; B = &D; } continue; }
        uint32_t fl = q.get<uint32_t>(); long long cnt, disk; if (fl & 0x2000) { cnt = q.get<long long>(); disk = q.get<long long>(); } else { cnt = q.get<int>(); disk = q.get<int>(); }
        long long off = q.get<long long>(); (void)cnt;
        if (q.bad || disk <= 0) continue;
        std::string st;
        if ((fl & 0x100) && off >= 0 && off + disk <= (long long)P.bulk.size()) st = P.bulk.substr((size_t)off, (size_t)disk);
        else if ((fl & 0x40) && q.o + disk <= D.size()) st = D.substr(q.o, (size_t)disk);
        else continue;
        size_t k = findIndices(st, 0, got);
        if (k) { streamed = st; B = &streamed; secs = got; after = k; }
    }
    // (2) older layout: the LOD's buffers (sections included) straight after the required bones
    if (secs.empty()) {
        size_t bo = r.o; bool okB = true;
        if (!inlined) {
            uint32_t fl = r.get<uint32_t>(); long long cnt, disk; if (fl & 0x2000) { cnt = r.get<long long>(); disk = r.get<long long>(); } else { cnt = r.get<int>(); disk = r.get<int>(); }
            long long off = r.get<long long>(); (void)cnt;
            if ((fl & 0x100) && off >= 0 && off + disk <= (long long)P.bulk.size()) streamed = P.bulk.substr((size_t)off, (size_t)disk);
            else if ((fl & 0x40) && r.o + disk <= D.size()) streamed = D.substr(r.o, (size_t)disk);
            else okB = false;
            if (okB) { B = &streamed; bo = 2; }      // streamed data starts with its own strip flags
        }
        for (int layout = 0; okB && layout < 8 && secs.empty(); layout++) {
            std::vector<Sec> got; size_t endAt;
            if (!parseSecs(*B, bo, layout, got, endAt)) continue;
            size_t k = findIndices(*B, endAt, got); if (k) { secs = got; after = k; }
        }
        if (!okB && secs.empty()) { err = "couldn't find the mesh's streamed data"; return false; }
    }
    if (secs.empty()) { err = "this game's character mesh format isn't supported yet (or it uses cloth)"; return false; }
    uint8_t ds = (uint8_t)(*B)[after]; long long total = 0; for (auto& sc : secs) total += 3ll * sc.tris;
    size_t ia = after + 9; if (ia + ds * total > B->size()) { err = "bad index data"; return false; }
    std::vector<uint32_t> idx((size_t)total);
    for (size_t i = 0; i < idx.size(); i++) { if (ds == 2) { uint16_t v; memcpy(&v, B->data() + ia + 2 * i, 2); idx[i] = v; } else memcpy(&idx[i], B->data() + ia + 4 * i, 4); }
    size_t p = ia + ds * total;
    int nvt = 0; for (auto& sc : secs) nvt += sc.nv;
    size_t pp = 0;
    for (size_t k = p; k + 16 <= B->size() && k < p + 65536; k++) { int a[4]; memcpy(a, B->data() + k, 16); if (a[0] == 12 && a[1] == nvt && a[2] == 12 && a[3] == nvt) { pp = k; break; } }
    if (!pp || pp + 16 + 12ull * nvt > B->size()) { err = "couldn't find the vertex positions"; return false; }
    Rd q(*B, pp + 16);
    s.pos.resize(nvt); for (int i = 0; i < nvt; i++) { float f[3]; for (auto& x : f) x = q.get<float>(); s.pos[i] = { f[0], f[1], f[2] }; }
    q.skip(2);
    uint32_t ntc = q.get<uint32_t>(), nv2 = q.get<uint32_t>(); int fullUV = q.get<int>(), hiTan = q.get<int>();
    int tes = q.get<int>(), tn = q.get<int>(); size_t tanAt = q.o; q.skip((size_t)tes * tn);
    int ues = q.get<int>(), un = q.get<int>(); size_t uvAt = q.o; q.skip((size_t)ues * un);
    if (q.bad || nv2 != (uint32_t)nvt || ntc == 0 || ntc > 8) { err = "unexpected vertex attributes"; return false; }
    s.nrm.resize(nvt); s.u.resize(nvt); s.v.resize(nvt); s.vMat.assign(nvt, 0); s.vBone.assign(nvt, 0);
    for (int i = 0; i < nvt; i++) {
        size_t ta = tanAt + (size_t)i * (hiTan ? 16 : 8) + (hiTan ? 8 : 4);
        double nn[3];
        for (int k = 0; k < 3; k++) { if (hiTan) { int16_t c; memcpy(&c, B->data() + ta + 2 * k, 2); nn[k] = c / 32767.0; } else nn[k] = ((int8_t)(*B)[ta + k]) / 127.0; }
        s.nrm[i] = { nn[0], nn[1], nn[2] };
        size_t ua = uvAt + (size_t)i * ntc * (fullUV ? 8 : 4);
        if (fullUV) { float f[2]; memcpy(f, B->data() + ua, 8); s.u[i] = f[0]; s.v[i] = f[1]; }
        else { uint16_t hh[2]; memcpy(hh, B->data() + ua, 4); s.u[i] = Half(hh[0]); s.v[i] = Half(hh[1]); }
    }
    // skin weights: which bone moves each vertex the most
    {
        Rd w(*B, q.o); w.skip(2);
        int variable = w.get<int>(); uint32_t maxInf = w.get<uint32_t>(); w.get<uint32_t>(); uint32_t nvw = w.get<uint32_t>();
        size_t save = w.o; bool found = false;
        for (int with16 = 1; with16 >= 0 && !found; with16--) {
            w.o = save; int b16 = with16 ? w.get<int>() : 0;
            if (with16 && b16 != 0 && b16 != 1) continue;
            int es = w.get<int>(), cnt = w.get<int>();
            size_t stride = (size_t)maxInf * (b16 ? 2 : 1) + maxInf;
            if (!variable && nvw == (uint32_t)nvt && es == 1 && (size_t)cnt == stride * nvt && w.o + cnt <= B->size()) {
                found = true;
                const uint8_t* d = (const uint8_t*)B->data() + w.o;
                s.vw.assign(nvt, {});
                for (auto& sc : secs) for (int v = sc.basev; v < sc.basev + sc.nv && v < nvt; v++) {
                    const uint8_t* vd = d + stride * v; int best = 0, bw = -1;
                    for (uint32_t k = 0; k < maxInf; k++) {
                        int bi = b16 ? (vd[2 * k] | (vd[2 * k + 1] << 8)) : vd[k];
                        int wt = vd[maxInf * (b16 ? 2 : 1) + k];
                        if (wt > 0 && bi < (int)sc.boneMap.size()) s.vw[v].push_back({ sc.boneMap[bi], wt / 255.f });
                        if (wt > bw) { bw = wt; best = bi; }
                    }
                    s.vBone[v] = best < (int)sc.boneMap.size() ? sc.boneMap[best] : 0;
                }
            }
        }
        if (!found) for (auto& sc : secs) for (int v = sc.basev; v < sc.basev + sc.nv && v < nvt; v++) s.vBone[v] = sc.boneMap.empty() ? 0 : sc.boneMap[0];
        s.skinned = true;
    }
    std::map<int, int> matSlot;
    for (auto& sc : secs) {
        if (!matSlot.count(sc.mat)) {
            Mat m; m.name = sc.mat < (int)matPkgs.size() ? matPkgs[sc.mat] : "Material";
            if (sc.mat < (int)matPkgs.size() && !matPkgs[sc.mat].empty()) {
                std::string tex = MaterialColourTexture(ps, matPkgs[sc.mat], m.factor);
                if (!tex.empty()) { m.tex = LoadGameTexture(ps, tex, note); m.texPath = U2W(tex); }
                std::string nt = MaterialNormalTexture(ps, matPkgs[sc.mat]);
                if (!nt.empty()) m.nrm = LoadGameTexture(ps, nt, note);
                AttachMasks(ps, matPkgs[sc.mat], m, note);
                MaterialGlow(ps, matPkgs[sc.mat], m, note);
                MaterialLook(ps, matPkgs[sc.mat], m, note);
                { const std::string& pk = matPkgs[sc.mat]; size_t sl = pk.find_last_of('/'); m.orig = pk + "." + (sl == std::string::npos ? pk : pk.substr(sl + 1)); }
            }
            s.mats.push_back(m); matSlot[sc.mat] = (int)s.mats.size() - 1;
        }
        int mi = matSlot[sc.mat];
        for (long long t = sc.base; t < sc.base + 3ll * sc.tris && t < (long long)idx.size(); t++) {
            uint32_t v = idx[(size_t)t]; if (v >= (uint32_t)nvt) { err = "bad index data"; return false; }
            s.idx.push_back(v); s.vMat[v] = mi;
        }
    }
    return true;
}
}   // namespace uex

// rotator (degrees) -> quaternion, the engine's own maths
static Q QuatOf(double pitch, double yaw, double roll) {
    double d = 3.14159265358979323846 / 360;
    double SP = std::sin(pitch * d), CP = std::cos(pitch * d), SY = std::sin(yaw * d), CY = std::cos(yaw * d), SR = std::sin(roll * d), CR = std::cos(roll * d);
    return { CR * SP * SY - SR * CP * CY, -CR * SP * CY - SR * CP * SY, CR * CP * SY - SR * SP * CY, CR * CP * CY + SR * SP * SY };
}
static bool ParseXf(const std::string& t, double loc[3], double rot[3], double scl[3]) {
    return sscanf(t.c_str(), "%lf,%lf,%lf\t%lf,%lf,%lf\t%lf,%lf,%lf", &loc[0], &loc[1], &loc[2], &rot[0], &rot[1], &rot[2], &scl[0], &scl[1], &scl[2]) == 9;
}
// Exports every mesh an object is made of (static and skeletal pieces), placed as they are on the object.
// parts: "kind\\tpath\\tx,y,z\\tp,y,r\\tsx,sy,sz" (world transforms); actorXf: the object's own "x,y,z\\tp,y,r\\tsx,sy,sz"

// ---- every model in the game's files: looks at each package's header for a StaticMesh / SkeletalMesh export
namespace uex {
static bool SkipByName(const std::string& key) {   // key: "/game/x/y.uasset" (lower case); obvious non-models only
    size_t sl = key.find_last_of('/'); std::string n = key.substr(sl + 1, key.size() - sl - 1 - 7);
    static const char* pre[] = { "t_", "tx_", "m_", "mi_", "mf_", "mm_", "mpc_", "abp_", "bp_", "wbp_", "w_", "dt_", "da_", "ns_", "ne_", "am_", "bs_",
        "as_", "phys_", "pa_", "sc_", "sw_", "lut_", "rt_", "fx_", "vfx_", "sfx_", "cue_", "mus_", "vo_" };
    for (auto x : pre) if (n.rfind(x, 0) == 0) return true;
    static const char* suf[] = { "_builtdata", "_anm", "_anim", "_skeleton", "_physicsasset", "_physics", "_mat", "_tex", "_cue", "_montage", "_blendspace" };
    for (auto x : suf) { size_t l = strlen(x); if (n.size() > l && n.compare(n.size() - l, l, x) == 0) return true; }
    for (auto x : { "/textures/", "/texture/", "/materials/", "/sounds/", "/sound/", "/audio/", "/wwiseaudio/", "/movies/", "/localization/", "/ui/", "/fonts/" })
        if (key.find(x) != std::string::npos) return true;
    return false;
}
// 0 not a model, 1 static, 2 skeletal; outPath gets the object path with the game's own letter case where known
static int MeshKindOf(PakSet& ps, const std::string& key, std::string& outPath, bool& exportable) {
    std::string b, err;
    std::string pkg = key.substr(0, key.size() - 7);
    std::string folder = pkg.substr(0, pkg.find_last_of('/'));
    exportable = true;
    if (ps.ioFiles.count(key)) {
        if (!ReadFileFromPak(ps, key, b, err, 65536)) return 0;
        if (!IsZen427(b)) {
            // Unreal 5 package: its header isn't read yet, so go by the usual names (can be placed, not exported)
            exportable = false;
            std::string n = pkg.substr(pkg.find_last_of('/') + 1);
            bool sk = n.rfind("sk_", 0) == 0 || n.rfind("skm_", 0) == 0;
            bool sm = n.rfind("sm_", 0) == 0 || n.rfind("s_", 0) == 0 || n.rfind("mesh_", 0) == 0 || folder.find("/mesh") != std::string::npos ||
                      folder.find("/staticmesh") != std::string::npos || folder.find("/props") != std::string::npos || folder.find("/models") != std::string::npos;
            if (!sk && !sm) return 0;
            outPath = pkg + "." + n;
            return sk ? 2 : 1;
        }
        uint32_t gOff = 0, gSize = 0; memcpy(&gOff, b.data() + 0x34, 4); memcpy(&gSize, b.data() + 0x38, 4);
        if ((size_t)gOff + gSize > b.size()) { if ((size_t)gOff + gSize > (64u << 20) || !ReadFileFromPak(ps, key, b, err, (long long)gOff + gSize)) return 0; }
        ZenHdr z; if (!ParseZen427(b, z)) return 0;
        LoadScriptObjects(ps);
        for (auto& e : z.exports) {
            if ((int)(e.outer >> 62) != 3) continue;                 // top-level objects only
            if ((int)(e.cls >> 62) != 1) continue;                   // engine class
            auto it = ps.scriptObjects.find(e.cls); if (it == ps.scriptObjects.end()) continue;
            int kind = it->second.first == "StaticMesh" ? 1 : it->second.first == "SkeletalMesh" ? 2 : 0;
            if (!kind) continue;
            std::string orig; uint32_t ni = 0; memcpy(&ni, b.data(), 4); ni &= 0x3FFFFFFF;
            if (ni < z.names.size() && LowerA(z.names[ni]) == pkg) orig = z.names[ni];
            outPath = (orig.empty() ? folder + "/" + e.name : orig) + "." + e.name;
            return kind;
        }
        return 0;
    }
    if (!ReadFileFromPak(ps, key, b, err)) return 0;
    Package P; P.asset.swap(b);
    if (!ParseLegacyHeader(P)) return 0;
    for (auto& e : P.exports) {
        if (e.outer != 0) continue;
        std::string c = P.ClassOf(e);
        int kind = c == "StaticMesh" ? 1 : c == "SkeletalMesh" ? 2 : 0;
        if (!kind) continue;
        std::string orig;
        for (auto& n : P.names) if (n.size() == pkg.size() && LowerA(n) == pkg) { orig = n; break; }
        outPath = (orig.empty() ? folder + "/" + e.name : orig) + "." + e.name;
        return kind;
    }
    return 0;
}
}  // namespace uex
static std::wstring MeshCacheFile(const std::wstring& paksDir, std::string& sig) {
    // signature: every container's name, size and date, plus the key in use
    std::vector<std::wstring> files; int budget = 4000;
    Walk(paksDir, 2, [&](const std::wstring& p) { std::wstring e = ExtLower(p); if (e == L".pak" || e == L".utoc") files.push_back(p); }, budget);
    std::sort(files.begin(), files.end());
    sig = "v2|" + W2U(Lower(paksDir));
    for (auto& f : files) {
        WIN32_FILE_ATTRIBUTE_DATA a{}; GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a);
        sig += "|" + W2U(Lower(f.substr(f.find_last_of(L"\\/") + 1))) + ":" + std::to_string(((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow) + ":" + std::to_string(a.ftLastWriteTime.dwLowDateTime);
    }
    { unsigned long long kh = 1469598103934665603ull; for (unsigned char c : g_aesKey) { kh ^= c; kh *= 1099511628211ull; } char kb[24]; snprintf(kb, sizeof kb, "|k%016llx", g_aesKey.empty() ? 0ull : kh); sig += kb; }
    unsigned long long h = 1469598103934665603ull; for (unsigned char c : sig) { h ^= c; h *= 1099511628211ull; }
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"UE4SS-LauncherBridge\\meshlists";
    CreateDirectoryW((std::wstring(tmp) + L"UE4SS-LauncherBridge").c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr);
    char hx[20]; snprintf(hx, sizeof hx, "%016llx", h);
    return dir + L"\\" + U2W(hx) + L".txt";
}
// Every asset path in the game's files (packed and IoStore), filtered by file name ("/game/x/y.uasset", lower case)
bool ModelListGameFiles(const std::wstring& paksDir, const std::function<bool(const std::string& fileName)>& want, std::vector<std::string>& out, std::string& note) {
    out.clear(); note.clear();
    if (paksDir.empty()) { note = "no game selected"; return false; }
    uex::PakSet ps; std::string err;
    if (!uex::OpenPakSetAt(ps, paksDir, err)) { note = err; return false; }
    auto take = [&](const std::string& k) {
        std::string fn = k.substr(k.find_last_of('/') + 1);
        if (want(fn)) out.push_back(k);
    };
    for (auto& kv : ps.files) take(kv.first);
    for (auto& kv : ps.ioFiles) if (!ps.files.count(kv.first)) take(kv.first);
    std::sort(out.begin(), out.end());
    if (ps.anyEncrypted && out.empty()) note = ps.badKey ? "the game's files are encrypted and the key doesn't fit" : "the game's files are encrypted";
    return true;
}
bool ModelListGameMeshes(const std::wstring& paksDir, std::vector<GameMesh>& out, std::string& note,
                         std::atomic<int>* done, std::atomic<int>* total, const std::function<void(const GameMesh&)>& found) {
    out.clear(); note.clear();
    if (paksDir.empty()) { note = "no game selected"; return false; }
    std::string sig; std::wstring cache = MeshCacheFile(paksDir, sig);
    {   // saved list from last time (same game files)
        FILE* f = _wfopen(cache.c_str(), L"rb");
        if (f) {
            std::string all; char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n); fclose(f);
            size_t nl = all.find('\n');
            if (nl != std::string::npos && all.compare(0, nl, sig) == 0) {
                size_t a = nl + 1;
                while (a < all.size()) {
                    size_t e = all.find('\n', a); if (e == std::string::npos) e = all.size();
                    if (e > a + 2) { GameMesh m; m.skeletal = all[a] == 'k' || all[a] == 'K'; m.exportable = all[a] == 's' || all[a] == 'k'; m.path = all.substr(a + 2, e - a - 2); out.push_back(m); }
                    a = e + 1;
                }
                if (done) *done = (int)out.size(); if (total) *total = (int)out.size();
                for (auto& m : out) if (found) found(m);
                if (out.empty()) note = "no models found in the game's files";
                return true;
            }
        }
    }
    std::string err;
    uex::PakSet ps;    // its own copy, so exporting at the same time is safe
    if (!uex::OpenPakSetAt(ps, paksDir, err)) { note = err; return false; }
    std::vector<std::string> keys;
    for (auto& kv : ps.files) if (kv.first.size() > 7 && kv.first.compare(kv.first.size() - 7, 7, ".uasset") == 0 && !uex::SkipByName(kv.first)) keys.push_back(kv.first);
    for (auto& kv : ps.ioFiles) if (kv.first.size() > 7 && kv.first.compare(kv.first.size() - 7, 7, ".uasset") == 0 && !ps.files.count(kv.first) && !uex::SkipByName(kv.first)) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    if (total) *total = (int)keys.size();
    if (!ps.io.empty()) uex::LoadScriptObjects(ps);       // shared, read-only from here on
    uex::LoadOodle(paksDir);                               // before the threads (it loads once)
    std::mutex mx; std::atomic<size_t> next{ 0 }; std::atomic<int> count{ 0 };
    auto one = [&](size_t idx) {
        std::string pth; int kind = 0; bool ex = true;
        try { kind = uex::MeshKindOf(ps, keys[idx], pth, ex); } catch (...) { kind = 0; }
        if (kind) { GameMesh m; m.path = pth; m.skeletal = kind == 2; m.exportable = ex; std::lock_guard<std::mutex> lk(mx); out.push_back(m); if (found) found(m); }
        int c = ++count; if (done) *done = c;
    };
    if (!keys.empty()) { one(0); next = 1; }               // first one alone (loads the decompressor)
    unsigned nt = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; t++) th.emplace_back([&] {
        uex::HandleCache hc; uex::t_handles = &hc;
        for (size_t idx; (idx = next++) < keys.size();) one(idx);
        uex::t_handles = nullptr;
    });
    for (auto& t : th) t.join();
    std::sort(out.begin(), out.end(), [](const GameMesh& a, const GameMesh& b) { return LowerA(a.path) < LowerA(b.path); });
    FILE* f = _wfopen(cache.c_str(), L"wb");
    if (f) {
        fwrite(sig.data(), 1, sig.size(), f); fputc('\n', f);
        for (auto& m : out) { fputs(m.skeletal ? (m.exportable ? "k\t" : "K\t") : (m.exportable ? "s\t" : "S\t"), f); fwrite(m.path.data(), 1, m.path.size(), f); fputc('\n', f); }
        fclose(f);
    }
    if (out.empty()) note = ps.badKey ? "the AES key doesn't open this game's files" : "no models found in the game's files (Unreal 5 games aren't supported yet)";
    return true;
}
// The selected object's shape, for the editor's yellow selection outline: every piece's triangles in world space
// (its own copy of the game's files, so it never gets in the way of an export running at the same time)
bool ModelOutlineMesh(const std::wstring& paksDir, const std::vector<std::string>& parts, std::vector<float>& xyz, std::vector<uint32_t>& idx, std::string& err) {
    static std::mutex mx; std::lock_guard<std::mutex> lk(mx);
    static uex::PakSet ps; static std::wstring dirUsed; static std::string keyUsed; static bool opened = false;
    if (!opened || Lower(dirUsed) != Lower(paksDir) || keyUsed != g_aesKey) {
        dirUsed = paksDir; keyUsed = g_aesKey;
        opened = uex::OpenPakSetAt(ps, paksDir, err);
        if (!opened) return false;
    }
    xyz.clear(); idx.clear();
    for (auto& line : parts) {
        std::vector<std::string> t; size_t a = 0;
        while (true) { size_t b = line.find('\t', a); t.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
        if (t.size() < 2 || (t[0] != "static" && t[0] != "skeletal")) continue;
        Src m; std::string e2, note;
        bool ok = t[0] == "skeletal" ? uex::LoadSkeletalMesh(ps, t[1], m, e2, note) : uex::LoadStaticMesh(ps, t[1], m, e2, note);
        if (!ok) { if (err.empty()) err = e2; continue; }
        if (idx.size() / 3 + m.idx.size() / 3 > 600000) break;          // a huge piece: keep what we have
        double cl[3] = { 0, 0, 0 }, cr[3] = { 0, 0, 0 }, cs[3] = { 1, 1, 1 };
        if (t.size() >= 5) ParseXf(t[2] + "\t" + t[3] + "\t" + t[4], cl, cr, cs);
        Q cq = QuatOf(cr[0], cr[1], cr[2]);
        uint32_t base = (uint32_t)(xyz.size() / 3);
        for (auto& v : m.pos) {
            V3 w = add(qrot(cq, { v.x * cs[0], v.y * cs[1], v.z * cs[2] }), { cl[0], cl[1], cl[2] });
            xyz.push_back((float)w.x); xyz.push_back((float)w.y); xyz.push_back((float)w.z);
        }
        for (auto i : m.idx) idx.push_back(base + i);
    }
    if (idx.empty() && err.empty()) err = "no model";
    return !idx.empty();
}
static thread_local int t_exportFmt = 0; static thread_local std::wstring t_exportPath;
bool ModelExportPartsAs(const std::wstring& paksDir, const std::string& name, const std::vector<std::string>& parts, const std::string& actorXf, const std::wstring& outFile, int fmt, std::string& msg) {
    t_exportFmt = fmt; t_exportPath = outFile;
    std::wstring o; bool ok = false;
    try { ok = ModelExportParts(paksDir, name, parts, actorXf, o, msg); } catch (...) { t_exportFmt = 0; throw; }
    t_exportFmt = 0;
    return ok;
}
bool ModelExportParts(const std::wstring& paksDir, const std::string& name, const std::vector<std::string>& parts, const std::string& actorXf, std::wstring& outFile, std::string& msg) {
    std::string err, note;
    uex::PakSet* ps = uex::OpenGame(paksDir, err);
    if (!ps) { msg = "Couldn't export: " + err + "."; return false; }
    double al[3] = { 0, 0, 0 }, ar[3] = { 0, 0, 0 }, as[3] = { 1, 1, 1 };
    if (!actorXf.empty()) ParseXf(actorXf, al, ar, as);
    Q aq = QuatOf(ar[0], ar[1], ar[2]), aqi = qconj(aq);
    Src all; std::vector<std::string> failed; int done = 0;
    all.bones.push_back(Bone{ "Root" });
    struct Piece { int boneBase; std::vector<Bone> bones; Xf toActor; std::string skeleton, pkg; };
    std::vector<Piece> pieces;          // animated pieces (they get the game's animations below)
    for (auto& line : parts) {
        std::vector<std::string> t; size_t a = 0;
        while (true) { size_t b = line.find('\t', a); t.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
        if (t.size() < 2) continue;
        if (t[0] == "niagara" || t[0] == "cascade" || t[0] == "light") {      // an effect or a light: kept as a reference / its settings, placed relative to the object
            double fl[3] = { 0, 0, 0 }, fr[3] = { 0, 0, 0 }, fs[3] = { 1, 1, 1 };
            if (t.size() >= 5) ParseXf(t[2] + "\t" + t[3] + "\t" + t[4], fl, fr, fs);
            V3 o = qrot(aqi, sub({ fl[0], fl[1], fl[2] }, { al[0], al[1], al[2] }));
            Q rq = qnorm(qmul(aqi, QuatOf(fr[0], fr[1], fr[2]))); double pi, ya, ro; Rotator(rq, pi, ya, ro);
            char b[700];
            snprintf(b, sizeof b, "%s\t%s\t%.3f,%.3f,%.3f\t%.3f,%.3f,%.3f\t%.3f,%.3f,%.3f", t[0].c_str(), t[1].c_str(),
                     o.x / (as[0] ? as[0] : 1), o.y / (as[1] ? as[1] : 1), o.z / (as[2] ? as[2] : 1), pi, ya, ro,
                     fs[0] / (as[0] ? as[0] : 1), fs[1] / (as[1] ? as[1] : 1), fs[2] / (as[2] ? as[2] : 1));
            all.fx.push_back(b);
            continue;
        }
        Src s; std::string e2;
        bool ok = t[0] == "skeletal" ? uex::LoadSkeletalMesh(*ps, t[1], s, e2, note) : uex::LoadStaticMesh(*ps, t[1], s, e2, note);
        std::string short_ = t[1].substr(t[1].find_last_of('/') + 1); size_t dt = short_.find('.'); if (dt != std::string::npos) short_ = short_.substr(0, dt);
        if (!ok) { failed.push_back(short_ + ": " + e2); continue; }
        double cl[3] = { 0, 0, 0 }, cr[3] = { 0, 0, 0 }, cs[3] = { 1, 1, 1 };
        if (t.size() >= 5) ParseXf(t[2] + "\t" + t[3] + "\t" + t[4], cl, cr, cs);
        Q cq = QuatOf(cr[0], cr[1], cr[2]);
        {   // remember the original piece, so the game it came from can show the real thing (true skinning and animation)
            V3 o = qrot(aqi, sub({ cl[0], cl[1], cl[2] }, { al[0], al[1], al[2] }));
            Q rq = qnorm(qmul(aqi, cq)); double pi, ya, ro; Rotator(rq, pi, ya, ro);
            char b[800];
            snprintf(b, sizeof b, "%s\t%s\t%.3f,%.3f,%.3f\t%.3f,%.3f,%.3f\t%.4f,%.4f,%.4f", t[0].c_str(), t[1].c_str(),
                     o.x / (as[0] ? as[0] : 1), o.y / (as[1] ? as[1] : 1), o.z / (as[2] ? as[2] : 1), pi, ya, ro,
                     cs[0] / (as[0] ? as[0] : 1), cs[1] / (as[1] ? as[1] : 1), cs[2] / (as[2] ? as[2] : 1));
            all.real.push_back(b);
        }
        // piece -> world -> the object's own space
        auto toLocal = [&](V3 v) {
            V3 w = add(qrot(cq, { v.x * cs[0], v.y * cs[1], v.z * cs[2] }), { cl[0], cl[1], cl[2] });
            V3 l = qrot(aqi, sub(w, { al[0], al[1], al[2] }));
            return V3{ l.x / (as[0] ? as[0] : 1), l.y / (as[1] ? as[1] : 1), l.z / (as[2] ? as[2] : 1) };
        };
        uint32_t base = (uint32_t)all.pos.size(); int matBase = (int)all.mats.size(), boneBase = (int)all.bones.size();
        // turned inside out only when the piece is mirrored relative to the object (a mirrored statue exported on its
        // own is mirrored back by the object's own scale); normals go through the inverse-transpose of the same mapping
        bool mirrored = cs[0] * cs[1] * cs[2] * as[0] * as[1] * as[2] < 0;
        for (size_t i = 0; i < s.pos.size(); i++) {
            all.pos.push_back(toLocal(s.pos[i]));
            V3 n = s.nrm[i]; n = { n.x / (cs[0] ? cs[0] : 1), n.y / (cs[1] ? cs[1] : 1), n.z / (cs[2] ? cs[2] : 1) };
            n = qrot(aqi, qrot(cq, n));
            n = { n.x * (as[0] ? as[0] : 1), n.y * (as[1] ? as[1] : 1), n.z * (as[2] ? as[2] : 1) };
            all.nrm.push_back(norm(n));
            all.u.push_back(s.u[i]); all.v.push_back(s.v[i]); all.vMat.push_back(s.vMat[i] + matBase);
            all.vBone.push_back(s.skinned ? s.vBone[i] + boneBase : 0);
            {   // full skin weights (for real skinning in other games); kept only when this piece has them
                std::vector<std::pair<int, float>> w;
                if (s.skinned && i < s.vw.size()) for (auto& x : s.vw[i]) w.push_back({ x.first + boneBase, x.second });
                if (!w.empty() || !all.vw.empty()) { if (all.vw.size() < all.pos.size() - 1) all.vw.resize(all.pos.size() - 1); all.vw.push_back(std::move(w)); }
            }
        }
        for (size_t i = 0; i + 2 < s.idx.size(); i += 3) {
            if (mirrored) { all.idx.push_back(base + s.idx[i]); all.idx.push_back(base + s.idx[i + 2]); all.idx.push_back(base + s.idx[i + 1]); }
            else for (int k = 0; k < 3; k++) all.idx.push_back(base + s.idx[i + k]);
        }
        for (auto& m : s.mats) all.mats.push_back(m);
        if (s.skinned) for (auto& b : s.bones) { Bone nb = b; if (nb.parent >= 0) nb.parent += boneBase; all.bones.push_back(nb); }
        if (s.skinned) {
            V3 o = qrot(aqi, sub({ cl[0], cl[1], cl[2] }, { al[0], al[1], al[2] }));
            pieces.push_back({ boneBase, s.bones, { qnorm(qmul(aqi, cq)), { o.x / (as[0] ? as[0] : 1), o.y / (as[1] ? as[1] : 1), o.z / (as[2] ? as[2] : 1) } }, s.skeleton, t[1] });
        }
        done++;
    }
    if (!done) { msg = "Couldn't export " + name + ": " + (failed.empty() ? "it has no model" : failed[0]) + "."; return false; }
    // the game's own animations for the animated pieces (by skeleton), decoded and applied to every piece in step.
    // Characters (a body rig: pelvis, spine, head, limbs...) are exported without them: a skinned body can't be bent
    // properly from rigid pieces, so they come in standing in their own pose. Props (doors, chests, chains...) keep them.
    std::string animNote;
    bool character = false;
    for (auto& pc : pieces) {
        std::set<std::string> hit;
        for (auto& b : pc.bones) {
            std::string n = LowerA(b.name);
            for (const char* k : { "pelvis", "spine", "hips", "neck", "head", "clavicle", "thigh", "calf", "upperarm", "forearm", "shoulder", "knee" })
                if (n.find(k) != std::string::npos) hit.insert(k);
        }
        if (hit.size() >= 3) character = true;
    }
    if (character && !pieces.empty()) animNote = " Characters are exported without animations.";
    if (!character) for (auto& pc : pieces) if (!pc.skeleton.empty()) {
        std::vector<std::string> list = uex::FindAnims(*ps, pc.skeleton, pc.pkg, 8);
        // the file stores one line per part per frame: keep it to a sensible size
        std::set<std::pair<int, int>> used; for (size_t v = 0; v < all.vBone.size(); v++) used.insert({ all.vBone[v], all.vMat[v] });
        size_t perFrame = std::max<size_t>(1, used.size() + (all.vw.empty() ? 0 : all.bones.size())), budget = 700000;
        int got = 0, tried = 0; std::string lastErr;
        for (auto& ap : list) {
            if (budget < perFrame * 10) break;
            int maxF = (int)std::min<size_t>(300, budget / perFrame);
            uex::DecodedAnim da; std::string e3; tried++;
            if (getenv("UEX_DEBUG")) fprintf(stderr, "anim %s\n", ap.c_str());
            if (!uex::DecodeAnim(*ps, ap, da, e3, maxF)) { lastErr = e3; if (getenv("UEX_DEBUG")) fprintf(stderr, "  failed: %s\n", e3.c_str()); continue; }
            if (getenv("UEX_DEBUG")) fprintf(stderr, "  ok %d frames %.1f fps %zu bones\n", da.frames, da.fps, da.bones.size());
            Anim an; an.name = da.name; an.fps = da.fps;
            { std::string pk = ap; size_t dot = pk.find('.'); if (dot != std::string::npos) pk = pk.substr(0, dot); an.path = pk + "." + da.name; }
            for (int f = 0; f < da.frames; f++) {
                std::vector<Xf> k(all.bones.size());
                for (auto& q : pieces) {
                    size_t nb = q.bones.size();
                    std::vector<Xf> local(nb), global(nb); std::vector<int> depth(nb, 0);
                    for (size_t j = 0; j < nb; j++) {
                        int pa = q.bones[j].parent; depth[j] = pa >= 0 && pa < (int)j ? depth[pa] + 1 : 0;
                        local[j] = q.bones[j].bindLocal;
                        auto it = da.bones.find(LowerA(q.bones[j].name));
                        if (it != da.bones.end() && f < (int)it->second.size()) {
                            local[j].r = it->second[f].r;
                            if (depth[j] <= 1) local[j].t = it->second[f].t;   // root and hips move; the rest keep this mesh's proportions
                        }
                        global[j] = pa >= 0 && pa < (int)j ? compose(global[pa], local[j]) : local[j];
                        Xf kc = compose(global[j], inverse(q.bones[j].bindGlobal));
                        k[q.boneBase + j] = compose(q.toActor, compose(kc, inverse(q.toActor)));
                    }
                }
                an.k.push_back(std::move(k));
            }
            budget -= std::min(budget, (size_t)da.frames * perFrame);
            all.anims.push_back(std::move(an)); got++;
        }
        if (got) animNote = " Brought " + std::to_string(got) + " of its animation" + (got == 1 ? "" : "s") + " from the game.";
        else if (!list.empty()) animNote = " Its animations couldn't be read (" + lastErr + ").";
        else animNote = " No animations for it were found in the game's files.";
        break;   // one skeleton drives them all
    }
    bool ok;
    if (t_exportFmt) {           // saving for other tools (.glb / .fbx) instead of the launcher's own format
        outFile = t_exportPath;
        ok = t_exportFmt == 1 ? WriteGlb(all, outFile, name, msg) : WriteFbx(all, outFile, name, msg);
        if (ok) msg = "Saved " + name + (done > 1 ? " (" + std::to_string(done) + " pieces)" : "") + " as " + W2U(NameOf(outFile)) + " (" +
                      std::to_string(all.idx.size() / 3) + " triangles, " + std::to_string(all.mats.size()) + " material" + (all.mats.size() == 1 ? "" : "s") + ")." +
                      (all.anims.empty() ? "" : " (Animations aren't saved in this format.)");
        return ok;
    }
    outFile = UniqueOut(U2W(name + ".game"));
    ok = WriteModel(all, U2W(name), outFile, L"", msg);
    if (ok) {
        msg = "Exported " + name + (done > 1 ? " (" + std::to_string(done) + " pieces)" : "") + " from the game" + msg.substr(msg.find(':'));
        if (!failed.empty()) msg += " Skipped " + std::to_string(failed.size()) + " piece" + (failed.size() == 1 ? "" : "s") + " (" + failed[0] + ").";
        msg += animNote;
        if (!all.fx.empty()) {
            int nl = 0; for (auto& f : all.fx) if (f.rfind("light\t", 0) == 0) nl++;
            int ne = (int)all.fx.size() - nl;
            if (ne) msg += " Kept " + std::to_string(ne) + " effect" + (ne == 1 ? "" : "s") + " (they show when it's placed in this game).";
            if (nl) msg += " Kept " + std::to_string(nl) + " light" + (nl == 1 ? "" : "s") + " (they work in any game).";
        }
    }
    return ok;
}

bool ModelExportFromGame(const std::wstring& paksDir, const std::string& objectPath, std::wstring& outFile, std::string& msg) {
    std::string name = objectPath.substr(objectPath.find_last_of('/') + 1); size_t dot = name.find('.'); if (dot != std::string::npos) name = name.substr(0, dot);
    std::string kind = "static";
    { std::string err; uex::PakSet* ps = uex::OpenGame(paksDir, err); uex::Package P;
      if (ps && uex::LoadPackage(*ps, objectPath, P)) for (auto& x : P.exports) if (P.ClassOf(x) == "SkeletalMesh") kind = "skeletal"; }
    return ModelExportParts(paksDir, name, { kind + "\t" + objectPath }, "", outFile, msg);
}

// ---------------------------------------------------------------- FModel
std::wstring FModelExe() {
    std::wstring e = FModelDir() + L"\\FModel.exe";
    if (FileExists(e)) return e;
    for (auto& f : ListFiles(FModelDir(), L"*.exe")) if (Lower(f).find(L"fmodel") != std::wstring::npos) return FModelDir() + L"\\" + f;
    return L"";
}
std::wstring FModelExports() {
    // FModel keeps its settings in %AppData%\FModel\AppSettings.json ("ModelDirectory" / "OutputDirectory")
    wchar_t app[MAX_PATH * 2] = {}; std::wstring dir;
    if (GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH * 2)) dir = app;
    std::string js;
    if (!dir.empty() && ReadBytes(dir + L"\\FModel\\AppSettings.json", js)) {
        for (auto key : { "\"ModelDirectory\"", "\"OutputDirectory\"" }) {
            size_t k = js.find(key); if (k == std::string::npos) continue;
            size_t q1 = js.find('"', js.find(':', k + strlen(key)) + 1), q2 = q1 == std::string::npos ? q1 : js.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) continue;
            std::string v = js.substr(q1 + 1, q2 - q1 - 1), c;
            for (size_t i = 0; i < v.size(); i++) { if (v[i] == '\\' && i + 1 < v.size()) i++; c += v[i]; }
            std::wstring w = U2W(c);
            if (std::string(key) == "\"OutputDirectory\"") w += L"\\Exports";
            if (DirExists(w)) return w;
        }
    }
    return FModelDir() + L"\\Output\\Exports";
}
bool FModelDownload(std::string& err) {
    std::string json;
    if (!NetGet(L"https://api.github.com/repos/4sval/FModel/releases/latest", json, err)) return false;
    std::string url;
    const std::string key = "\"browser_download_url\"";
    for (size_t p = json.find(key); p != std::string::npos; p = json.find(key, p + 1)) {
        size_t q1 = json.find('"', json.find(':', p + key.size()) + 1), q2 = json.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) break;
        std::string u = json.substr(q1 + 1, q2 - q1 - 1);
        if (LowerA(u).size() > 4 && LowerA(u).compare(u.size() - 4, 4, ".zip") == 0) { url = u; break; }
    }
    if (url.empty()) { err = "Couldn't find FModel's download on GitHub."; return false; }
    std::string zip;
    if (!NetGet(U2W(url), zip, err)) return false;
    if (!ZipExtract(zip, FModelDir())) { err = "Couldn't unpack FModel."; return false; }
    if (FModelExe().empty()) { err = "FModel was downloaded but FModel.exe wasn't in it."; return false; }
    return true;
}
bool ModelFindAesKey(unsigned long pid, const std::wstring& paksDir, std::string& keyHex, std::string& msg) { return uex::FindAesKey(pid, paksDir, keyHex, msg); }

// A small picture of one of the game's textures, read straight from its files (for the material editor).
// Has its own copy of the opened files, so it can run beside an export.
static std::mutex g_texPreviewMx;
static uex::PakSet* TexturePaks(const std::wstring& paksDir, std::string& err) {     // (call with g_texPreviewMx held)
    static uex::PakSet ps; static std::wstring dirUsed; static std::string keyUsed; static bool opened = false;
    if (!opened || Lower(dirUsed) != Lower(paksDir) || keyUsed != g_aesKey) {
        dirUsed = paksDir; keyUsed = g_aesKey;
        opened = uex::OpenPakSetAt(ps, paksDir, err);
        if (!opened) return nullptr;
    }
    return &ps;
}
// One of the game's textures at full size, saved as a PNG (for the material editor's "Export texture")
bool GameTextureExport(const std::wstring& paksDir, const std::string& objectPath, const std::wstring& outPng, int& w, int& h, std::string& err) {
    std::lock_guard<std::mutex> lk(g_texPreviewMx);
    uex::PakSet* ps = TexturePaks(paksDir, err); if (!ps) return false;
    Image im; std::string note;
    if (!uex::ReadGameTexture(*ps, objectPath, note, 16384, im) || !im.w) { err = note.empty() ? "couldn't read the texture" : note; return false; }
    std::string png;
    stbi_write_png_to_func([](void* ctx, void* d, int sz) { ((std::string*)ctx)->append((const char*)d, sz); }, &png, im.w, im.h, 4, im.px.data(), im.w * 4);
    if (png.empty()) { err = "couldn't make the PNG"; return false; }
    if (!WriteBytes(outPng, png)) { err = "couldn't write " + W2U(outPng); return false; }
    w = im.w; h = im.h;
    return true;
}
bool GameTexturePreview(const std::wstring& paksDir, const std::string& objectPath, int maxDim, std::vector<unsigned char>& rgba, int& w, int& h, std::string& err) {
    std::lock_guard<std::mutex> lk(g_texPreviewMx);
    uex::PakSet* psp = TexturePaks(paksDir, err); if (!psp) return false;
    uex::PakSet& ps = *psp;
    Image im; std::string note;
    // ask for a mip a little bigger than wanted (games often only ship the big ones in the files), then shrink
    if (!uex::ReadGameTexture(ps, objectPath, note, std::max(512, maxDim), im) || !im.w) { err = note.empty() ? "couldn't read the texture" : note; return false; }
    int s = 1; while (std::max(im.w, im.h) / s > maxDim) s *= 2;
    w = std::max(1, im.w / s); h = std::max(1, im.h / s);
    rgba.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        unsigned sum[4] = { 0, 0, 0, 0 }; unsigned n = 0;
        for (int yy = y * s; yy < std::min(im.h, (y + 1) * s); yy++) for (int xx = x * s; xx < std::min(im.w, (x + 1) * s); xx++) {
            const unsigned char* p = &im.px[((size_t)yy * im.w + xx) * 4];
            for (int c = 0; c < 4; c++) sum[c] += p[c];
            n++;
        }
        for (int c = 0; c < 4; c++) rgba[((size_t)y * w + x) * 4 + c] = (unsigned char)(n ? sum[c] / n : 0);
    }
    return true;
}

// ---- for the Material Graph: any texture as pixels (a picture file, or a game texture), and writing a PNG
bool PictureRGBA(const std::wstring& paksDir, const std::string& value, int maxDim, std::vector<unsigned char>& px, int& w, int& h, std::string& err) {
    if (value.rfind("file:", 0) == 0) {
        std::string data; if (!ReadBytes(U2W(value.substr(5)), data)) { err = "couldn't open the picture"; return false; }
        int n = 0; unsigned char* d = stbi_load_from_memory((const unsigned char*)data.data(), (int)data.size(), &w, &h, &n, 4);
        if (!d) { err = "couldn't read the picture"; return false; }
        int s = 1; while (std::max(w, h) / s > maxDim) s *= 2;
        int ow = std::max(1, w / s), oh = std::max(1, h / s);
        px.resize((size_t)ow * oh * 4);
        for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) memcpy(&px[((size_t)y * ow + x) * 4], &d[((size_t)(y * s) * w + (size_t)x * s) * 4], 4);
        stbi_image_free(d); w = ow; h = oh;
        return true;
    }
    if (paksDir.empty()) { err = "the game's files aren't found"; return false; }
    return GameTexturePreview(paksDir, value, maxDim, px, w, h, err);
}
bool WritePngRGBA(const std::wstring& path, const std::vector<unsigned char>& px, int w, int h) {
    std::string d;
    stbi_write_png_to_func([](void* ctx, void* dd, int sz) { ((std::string*)ctx)->append((const char*)dd, sz); }, &d, w, h, 4, px.data(), w * 4);
    return !d.empty() && WriteBytes(path, d);
}

// ---- any picture (PNG, JPG, TGA, BMP, or a high dynamic range .hdr) as floats: 0..1 as stored for ordinary pictures,
// real light values for .hdr
bool ImageLoadAny(const std::wstring& path, std::vector<float>& rgba, int& w, int& h, bool& hdr, std::string& err) {
    std::string bytes;
    if (!ReadBytes(path, bytes) || bytes.empty()) { err = "Couldn't read the file"; return false; }
    int n = 0;
    hdr = stbi_is_hdr_from_memory((const unsigned char*)bytes.data(), (int)bytes.size()) != 0;
    if (hdr) {
        float* d = stbi_loadf_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
        if (!d) { err = std::string("Not a picture the launcher can read (") + (stbi_failure_reason() ? stbi_failure_reason() : "?") + ")"; return false; }
        rgba.assign(d, d + (size_t)w * h * 4); stbi_image_free(d);
        return true;
    }
    unsigned char* d = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!d) { err = std::string("Not a picture the launcher can read (") + (stbi_failure_reason() ? stbi_failure_reason() : "?") + ")"; return false; }
    rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < rgba.size(); i++) rgba[i] = d[i] / 255.f;
    stbi_image_free(d);
    return true;
}
// fmt: 0 PNG, 1 TGA, 2 JPG, 3 HDR (floats as they are), 4 BMP; channels 1 (grey) or 4
bool ImageWrite(const std::wstring& path, const std::vector<float>& rgba, int w, int h, int fmt, int channels, std::string& err) {
    std::string d;
    auto sink = [](void* ctx, void* dd, int sz) { ((std::string*)ctx)->append((const char*)dd, sz); };
    if (fmt == 3) {
        std::vector<float> rgb((size_t)w * h * 3);
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) for (int k = 0; k < 3; k++) rgb[i * 3 + k] = rgba[i * 4 + k];
        // Radiance RGBE, flat scanlines (every .hdr reader accepts them)
        char head[128]; snprintf(head, sizeof head, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", h, w);
        d = head; d.reserve(d.size() + (size_t)w * h * 4);
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
            float r = std::max(0.f, rgb[i * 3]), g = std::max(0.f, rgb[i * 3 + 1]), b = std::max(0.f, rgb[i * 3 + 2]), m = std::max(r, std::max(g, b));
            unsigned char e[4] = { 0, 0, 0, 0 };
            if (m >= 1e-32f) { int ex; float f = frexpf(m, &ex) * 256.f / m; e[0] = (unsigned char)(r * f); e[1] = (unsigned char)(g * f); e[2] = (unsigned char)(b * f); e[3] = (unsigned char)(ex + 128); }
            d.append((const char*)e, 4);
        }
    } else {
        int ch = channels == 1 ? 1 : (fmt == 2 ? 3 : 4);
        std::vector<unsigned char> px((size_t)w * h * ch);
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) for (int k = 0; k < ch; k++) {
            float v = ch == 1 ? rgba[i * 4] : rgba[i * 4 + k];
            px[i * ch + k] = (unsigned char)std::lround(std::clamp(v, 0.f, 1.f) * 255.f);
        }
        if (fmt == 0) stbi_write_png_to_func(sink, &d, w, h, ch, px.data(), w * ch);
        else if (fmt == 1) stbi_write_tga_to_func(sink, &d, w, h, ch, px.data());
        else if (fmt == 2) stbi_write_jpg_to_func(sink, &d, w, h, ch, px.data(), 92);
        else stbi_write_bmp_to_func(sink, &d, w, h, ch, px.data());
    }
    if (d.empty()) { err = "Couldn't encode the picture"; return false; }
    if (!WriteBytes(path, d)) { err = "Couldn't write the file"; return false; }
    return true;
}
