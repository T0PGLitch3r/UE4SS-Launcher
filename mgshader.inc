// ---- Material graph -> HLSL, compiled with D3DCompile, and the real-time 3D preview (Direct3D 11, the launcher's
// own device). Everything the graph does is turned into one shader: every node becomes a float4 (with a known number
// of components, like Unreal's float1..float4), the material's inputs come out of Eval(), and the pixel shader lights
// it (metal / roughness, GGX, image-based light from the environment). The game itself can't compile shaders, so
// in the game the same graph is baked into textures (MgBake); this shader is the preview, and can be exported.
//
// Every value you can edit (numbers, colours, a maths node's constants, a texture's tiling) lives in a constant
// buffer, so changing it is instant; the shader is only rebuilt when the graph's shape changes (nodes, wires,
// textures, the material's blend mode...).
#include <d3dcompiler.h>
#include <d3d11shader.h>

static const int MS_MAX_TEX = 14;                 // t0..t13 for the graph's textures, t15 the environment
static const int MS_MAX_SLOTS = 256;              // float4 P[256] in the parameter buffer

struct MsTexSlot { int node = 0; std::string value; int sampler = 0; int filter = 0; };
struct MsCompiled {                                // what the code generator made
    std::string hlsl, key;
    std::vector<MsTexSlot> tex;
    std::map<int, int> slotOf;                     // node id -> first constant slot
    int slots = 0;
    std::vector<std::pair<int, std::string>> errs; // (node id or -1, message)
    std::vector<std::pair<int, std::string>> warns;
    MeProps props;
};
// the compiled shader, ready to draw
static struct MsShader {
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11InputLayout* il = nullptr;
    MsCompiled src; bool ok = false;
    std::string log;                               // the compiler's messages
    int vsInstr = 0, psInstr = 0, psTex = 0; double ms = 0;
    std::string builtKey;                          // the graph shape it was built from
} g_ms;
// compiling runs in the background (D3DCompile can take a moment); the result is picked up on the main thread
struct MsJob { MsCompiled c; ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; std::string log; bool ok = false; double ms = 0; int vsI = 0, psI = 0, psT = 0; };
static std::mutex g_msJobMx; static std::unique_ptr<MsJob> g_msDone; static std::atomic<bool> g_msBusy{ false };
static std::string g_msWantKey; static ULONGLONG g_msWantAt = 0; static bool g_msForce = false;

// ---- code generation
struct MsGen {
    MsCompiled* out;
    std::map<int, int> n;                          // node id -> components (1..4), 0 = not made yet, -1 = being made
    std::string body;
    std::set<int> errNodes;
    int ensureSlot(int id, int count = 1) {
        auto it = out->slotOf.find(id); if (it != out->slotOf.end()) return it->second;
        int s = out->slots; out->slots += count; out->slotOf[id] = s; return s;
    }
    void err(int id, const std::string& m) { if (!errNodes.count(id)) { out->errs.push_back({ id, m }); errNodes.insert(id); } }
    static std::string P(int slot) { return "P[" + std::to_string(slot) + "]"; }
    // a value with c components, broadcast / padded the same way the CPU evaluator does
    static std::string Fit(const std::string& e, int c) {
        switch (c) { case 1: return "(" + e + ").xxxx"; case 2: return "float4((" + e + ").xy, 0, 0)"; case 3: return "float4((" + e + ").xyz, 0)"; default: return e; }
    }
    // the value going into input k of a node, as a float4 expression, and its components
    std::pair<std::string, int> in(const MeNode& node, int k, const std::string& def, int defN = 1) {
        auto it = M.nin.find(MeInKey(node.id, k));
        if (it == M.nin.end()) return { def, defN };
        MeNode* s = MeFind(it->second.first); if (!s) return { def, defN };
        int c = make(*s); if (c <= 0) return { def, defN };
        return swz(*s, it->second.second, c);
    }
    std::pair<std::string, int> swz(const MeNode& s, int outIdx, int c) {
        std::string v = "v" + std::to_string(s.id);
        if ((s.kind == MEK_TEX || s.kind == MEK_VEC) && outIdx >= 1 && outIdx <= 4) { const char* ch = "xyzw"; return { v + "." + std::string(4, ch[outIdx - 1]), 1 }; }
        if ((s.kind == MEK_TEX || s.kind == MEK_VEC) && outIdx == 0) return { "float4(" + v + ".xyz, 0)", 3 };
        return { v, c };
    }
    // emit a node (and what feeds it); returns its components
    int make(const MeNode& nd) {
        int& st = n[nd.id];
        if (st > 0) return st;
        if (st < 0) { err(nd.id, "This node feeds itself (a loop)"); return 0; }
        st = -1;
        std::string v = "v" + std::to_string(nd.id), e; int c = 1;
        auto C = [&](int k) { int s = ensureSlot(nd.id, nd.op == OP_LAYERBLEND ? 3 : 1); const char* ch = "xyzw"; return P(s) + "." + std::string(4, ch[k]); };
        auto Ck = [&](int k) { int s = ensureSlot(nd.id, nd.op == OP_LAYERBLEND ? 3 : 1); const char* ch = "xyzw"; return P(s) + "." + std::string(1, ch[k]); };
        std::string uvDef = "float4(c.uv, 0, 0)";
        if (nd.kind == MEK_SCA) { e = P(ensureSlot(nd.id)) + ".xxxx"; c = 1; }
        else if (nd.kind == MEK_VEC) { e = P(ensureSlot(nd.id)); c = 4; }
        else if (nd.kind == MEK_TEX) {
            int slot = ensureSlot(nd.id, 2);
            int t = (int)out->tex.size();
            if (t >= MS_MAX_TEX) { err(nd.id, "Too many Texture Samples (the preview can use " + std::to_string(MS_MAX_TEX) + ")"); st = 0; return 0; }
            MsTexSlot ts; ts.node = nd.id; ts.value = nd.value; ts.sampler = nd.sampler; ts.filter = nd.filter; out->tex.push_back(ts);
            if (nd.value.empty()) out->warns.push_back({ nd.id, "A Texture Sample has no texture (it reads as grey)" });
            auto uv = in(nd, 0, uvDef, 2);
            std::string uvn = "uv" + std::to_string(nd.id);
            body += "    float2 " + uvn + " = TexXf((" + uv.first + ").xy, " + P(slot) + ", " + P(slot + 1) + ".x);\n";
            std::string smp = "SMP(T" + std::to_string(t) + ", S" + std::to_string(t) + ", " + uvn + ")";
            if (nd.sampler == 1) e = "UnpackNormal(" + smp + ")";
            else e = smp;
            c = 4;
        } else if (nd.kind == MEK_OP) {
            switch (nd.op) {
            case OP_TEXCOORD: e = "float4(c.uv * " + P(ensureSlot(nd.id)) + ".xy, 0, 0)"; c = 2; break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MIN: case OP_MAX: {
                auto a = in(nd, 0, C(0)), b = in(nd, 1, C(1)); c = std::max(a.second, b.second);
                const char* o = nd.op == OP_ADD ? "+" : nd.op == OP_SUB ? "-" : "*";
                if (nd.op == OP_DIV) e = "SafeDiv(" + a.first + ", " + b.first + ")";
                else if (nd.op == OP_MIN) e = "min(" + a.first + ", " + b.first + ")";
                else if (nd.op == OP_MAX) e = "max(" + a.first + ", " + b.first + ")";
                else e = "(" + a.first + " " + o + " " + b.first + ")";
                break;
            }
            case OP_LERP: { auto a = in(nd, 0, C(0)), b = in(nd, 1, C(1)), t = in(nd, 2, C(2)); c = std::max(a.second, b.second);
                if (t.second > 1 && t.second != c) out->warns.push_back({ nd.id, "Lerp's Alpha has a different number of channels from A / B" });
                e = "lerp(" + a.first + ", " + b.first + ", " + (t.second == 1 ? t.first : t.first) + ")"; break; }
            case OP_ONEMINUS: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"); c = a.second; e = "(1.0 - " + a.first + ")"; break; }
            case OP_POW: { auto a = in(nd, 0, C(0)), x = in(nd, 1, C(1)); c = std::max(a.second, x.second); e = "pow(max(" + a.first + ", 0.0), " + x.first + ")"; break; }
            case OP_CLAMP: { auto a = in(nd, 0, C(0)), lo = in(nd, 1, C(1)), hi = in(nd, 2, C(2)); c = a.second; e = "min(max(" + a.first + ", " + lo.first + "), " + hi.first + ")"; break; }
            case OP_DESAT: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"), f = in(nd, 1, C(1)); c = a.second;
                e = "(" + a.first + " + (dot(" + a.first + ".xyz, float3(0.3, 0.59, 0.11)).xxxx - " + a.first + ") * (" + f.first + ").x)"; break; }
            case OP_MASK: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"); std::vector<std::string> comps; const char* ch = "xyzw";
                for (int k = 0; k < 4; k++) if (nd.c[k] > 0.5f) comps.push_back("(" + a.first + ")." + std::string(1, ch[k]));
                if (comps.empty()) { err(nd.id, "Component Mask keeps no channels"); comps.push_back("0.0"); }
                c = (int)comps.size(); std::string j; for (int k = 0; k < 4; k++) j += (k ? ", " : "") + (k < c ? comps[k] : std::string("0.0"));
                e = "float4(" + j + ")"; break; }
            case OP_APPEND: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"), b = in(nd, 1, "float4(0, 0, 0, 0)"); std::vector<std::string> comps; const char* ch = "xyzw";
                for (int k = 0; k < a.second; k++) comps.push_back("(" + a.first + ")." + std::string(1, ch[k]));
                for (int k = 0; k < b.second; k++) comps.push_back("(" + b.first + ")." + std::string(1, ch[k]));
                if (comps.size() > 4) { err(nd.id, "Append makes more than 4 channels (" + std::to_string(comps.size()) + ")"); comps.resize(4); }
                c = std::max(1, (int)comps.size()); std::string j; for (int k = 0; k < 4; k++) j += (k ? ", " : "") + (k < (int)comps.size() ? comps[k] : std::string("0.0"));
                e = "float4(" + j + ")"; break; }
            case OP_LAYERBLEND: {
                int s = ensureSlot(nd.id, 3); std::string acc = "lb" + std::to_string(nd.id), sum = "lw" + std::to_string(nd.id);
                body += "    float4 " + acc + " = 0; float " + sum + " = 0;\n"; int cc = 1;
                for (size_t l = 0; l < nd.layers.size() && l < 8; l++) {
                    auto L = in(nd, (int)(2 * l), "float4(0, 0, 0, 0)"); cc = std::max(cc, L.second);
                    const char* ch = "xyzw";
                    std::string wdef = P(s + 1 + (int)l / 4) + "." + std::string(1, ch[l % 4]);
                    auto W = in(nd, (int)(2 * l + 1), "(" + wdef + ").xxxx");
                    bool plugged = M.nin.count(MeInKey(nd.id, (int)(2 * l + 1))) > 0;
                    std::string w = plugged ? "saturate((" + W.first + ").x)" : "max(" + wdef + ", 0.0)";
                    body += "    { float w = " + w + "; " + acc + " += " + L.first + " * w; " + sum + " += w; }\n";
                }
                c = cc; e = "(" + sum + " > 0 ? " + acc + " / " + sum + " : 0.0.xxxx)"; break;
            }
            case OP_SATURATE: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"); c = a.second; e = "saturate(" + a.first + ")"; break; }
            case OP_ABS: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"); c = a.second; e = "abs(" + a.first + ")"; break; }
            case OP_FRAC: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"); c = a.second; e = "frac(" + a.first + ")"; break; }
            case OP_FLOOR: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"); c = a.second; e = "floor(" + a.first + ")"; break; }
            case OP_SQRT: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"); c = a.second; e = "sqrt(max(" + a.first + ", 0.0))"; break; }
            case OP_SIN: case OP_COS: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"); c = a.second;
                e = std::string(nd.op == OP_SIN ? "sin(" : "cos(") + a.first + " * Period(" + Ck(0) + "))"; break; }
            case OP_DOT: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"), b = in(nd, 1, "float4(0, 0, 0, 0)"); int m = std::max(a.second, b.second);
                if (a.second > 1 && b.second > 1 && a.second != b.second) err(nd.id, "Dot: A has " + std::to_string(a.second) + " channels and B has " + std::to_string(b.second));
                e = m == 1 ? "((" + a.first + ").x * (" + b.first + ").x).xxxx" : "dot(" + Fit(a.first, m) + " * Mask" + std::to_string(m) + ", " + Fit(b.first, m) + ").xxxx"; c = 1; break; }
            case OP_NORMALIZE: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"); c = a.second;
                e = c == 1 ? "sign(" + a.first + ")" : "SafeNormalize(" + a.first + " * Mask" + std::to_string(c) + ")"; break; }
            case OP_FRESNEL: { auto x = in(nd, 0, C(0)), b = in(nd, 1, C(1)); c = 1;
                e = "Fresnel(c, (" + x.first + ").x, (" + b.first + ").x).xxxx"; break; }
            case OP_NORMALSTR: { auto a = in(nd, 0, "float4(0, 0, 1, 0)", 3), s = in(nd, 1, C(1)); c = 3;
                e = "float4(normalize(float3((" + a.first + ").xy * (" + s.first + ").x, (" + a.first + ").z)), 0)"; break; }
            case OP_TIME: e = "(c.time * " + Ck(0) + ").xxxx"; c = 1; break;
            case OP_PANNER: { auto uv = in(nd, 0, uvDef, 2), t = in(nd, 1, "c.time.xxxx"); c = 2;
                e = "float4((" + uv.first + ").xy + " + P(ensureSlot(nd.id)) + ".xy * (" + t.first + ").x, 0, 0)"; break; }
            case OP_ROTATOR: { auto uv = in(nd, 0, uvDef, 2), t = in(nd, 1, "c.time.xxxx"); c = 2; int s = ensureSlot(nd.id);
                e = "float4(Rotate((" + uv.first + ").xy, " + P(s) + ".xy, " + P(s) + ".z * (" + t.first + ").x), 0, 0)"; break; }
            case OP_BUMPOFFSET: { auto uv = in(nd, 0, uvDef, 2), h = in(nd, 1, "float4(0, 0, 0, 0)"), r = in(nd, 2, C(0)); c = 2; int s = ensureSlot(nd.id);
                e = "float4(BumpOffset(c, (" + uv.first + ").xy, (" + h.first + ").x, (" + r.first + ").x, " + P(s) + ".y), 0, 0)"; break; }
            case OP_VNORMAL: e = "float4(c.N, 0)"; c = 3; break;
            case OP_CAMVEC: e = "float4(c.V, 0)"; c = 3; break;
            case OP_WORLDPOS: e = "float4(c.wpos * 100.0, 0)"; c = 3; break;
            case OP_IF: { auto a = in(nd, 0, "float4(0, 0, 0, 0)"), b = in(nd, 1, "float4(0, 0, 0, 0)"), gt = in(nd, 2, "float4(0, 0, 0, 0)"), eq = in(nd, 3, "float4(0, 0, 0, 0)"), lt = in(nd, 4, "float4(0, 0, 0, 0)");
                c = std::max(gt.second, std::max(eq.second, lt.second));
                e = "((" + a.first + ").x > (" + b.first + ").x ? " + gt.first + " : (abs((" + a.first + ").x - (" + b.first + ").x) < 1e-5 ? " + eq.first + " : " + lt.first + "))"; break; }
            default: err(nd.id, "Unknown node"); e = "float4(0, 0, 0, 0)"; c = 1; break;
            }
        } else { err(nd.id, "This node can't be used here"); st = 0; return 0; }
        // keep the unused channels like the CPU evaluator (broadcast for one, zero past the end)
        std::string fitted = c == 1 ? "(" + e + ").xxxx" : c == 4 ? e : Fit(e, c);
        if (c == 1 && (nd.kind == MEK_SCA || (nd.kind == MEK_OP && (nd.op == OP_TIME)))) fitted = e;
        body += "    float4 " + v + " = " + fitted + ";\n";
        st = c;
        return c;
    }
};

// the parts of the shader that don't depend on the graph
static const char* kMsHead = R"HLSL(
cbuffer Frame : register(b0) {
    row_major float4x4 gViewProj; row_major float4x4 gWorld; row_major float4x4 gInvViewProj;
    float4 gCam;        // xyz camera position
    float4 gLight;      // xyz direction towards the light, w intensity
    float4 gLightCol;
    float4 gMisc;       // x time, y environment intensity, z environment turn (0..1), w environment mips
    float4 gProps;      // x opacity mask clip, y displacement scale, z world position offset scale, w two sided
    float4 gView;       // x exposure, y show background, z channel view (0 all, 1 r, 2 g, 3 b, 4 a), w unused
};
cbuffer Params : register(b1) { float4 P[256]; };
Texture2D gEnv : register(t15); SamplerState gEnvS : register(s15);
Texture2D T0 : register(t0); Texture2D T1 : register(t1); Texture2D T2 : register(t2); Texture2D T3 : register(t3);
Texture2D T4 : register(t4); Texture2D T5 : register(t5); Texture2D T6 : register(t6); Texture2D T7 : register(t7);
Texture2D T8 : register(t8); Texture2D T9 : register(t9); Texture2D T10 : register(t10); Texture2D T11 : register(t11);
Texture2D T12 : register(t12); Texture2D T13 : register(t13);
SamplerState S0 : register(s0); SamplerState S1 : register(s1); SamplerState S2 : register(s2); SamplerState S3 : register(s3);
SamplerState S4 : register(s4); SamplerState S5 : register(s5); SamplerState S6 : register(s6); SamplerState S7 : register(s7);
SamplerState S8 : register(s8); SamplerState S9 : register(s9); SamplerState S10 : register(s10); SamplerState S11 : register(s11);
SamplerState S12 : register(s12); SamplerState S13 : register(s13);
#ifdef VERTEX
#define SMP(t, s, uv) t.SampleLevel(s, uv, 0)
#else
#define SMP(t, s, uv) t.Sample(s, uv)
#endif
static const float PI = 3.14159265;
static const float4 Mask1 = float4(1, 0, 0, 0), Mask2 = float4(1, 1, 0, 0), Mask3 = float4(1, 1, 1, 0), Mask4 = float4(1, 1, 1, 1);
struct Ctx { float2 uv; float3 wpos; float3 N; float3 V; float3 Vts; float time; };
struct Mat { float3 base; float metal; float spec; float rough; float3 emis; float opac; float opmask; float3 nrm; float ao; float3 wpo; float disp; };
float4 SafeDiv(float4 a, float4 b) { return abs(b) > 1e-6 ? a / b : 0.0; }
float4 SafeNormalize(float4 a) { float l = length(a); return l > 1e-8 ? a / l : 0.0; }
float Period(float p) { return p != 0 ? 6.2831853 / p : 1.0; }
float2 Rotate(float2 uv, float2 centre, float a) { float s = sin(a), co = cos(a); float2 d = uv - centre; return float2(d.x * co - d.y * s, d.x * s + d.y * co) + centre; }
float2 TexXf(float2 uv, float4 tileOff, float rotDeg) {
    uv = uv * tileOff.xy + tileOff.zw;
    if (rotDeg != 0) uv = Rotate(uv, float2(0.5, 0.5), rotDeg * PI / 180.0);
    return uv;
}
float4 UnpackNormal(float4 t) { float2 xy = t.xy * 2 - 1; return float4(xy, sqrt(saturate(1 - dot(xy, xy))), t.w); }
float Fresnel(Ctx c, float e, float base) { float ndv = saturate(dot(normalize(c.N), normalize(c.V))); return base + (1 - base) * pow(max(1 - ndv, 0.0), e); }
float2 BumpOffset(Ctx c, float2 uv, float h, float ratio, float refPlane) { return uv + c.Vts.xy * (h * ratio - ratio * refPlane); }
)HLSL";

static const char* kMsTail = R"HLSL(
struct VIn { float3 pos : POSITION; float3 nrm : NORMAL; float4 tan : TANGENT; float2 uv : TEXCOORD0; };
struct VOut { float4 pos : SV_Position; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float4 tan : TEXCOORD2; float2 uv : TEXCOORD3; };
VOut VSMain(VIn i) {
    VOut o;
    float3 wp = mul(gWorld, float4(i.pos, 1)).xyz;
    float3 n = normalize(mul((float3x3)gWorld, i.nrm));
    float3 t = normalize(mul((float3x3)gWorld, i.tan.xyz));
    Ctx c; c.uv = i.uv; c.wpos = wp; c.N = n; c.V = normalize(gCam.xyz - wp); c.Vts = float3(0, 0, 1); c.time = gMisc.x;
#ifdef VERTEX
    Mat m = Eval(c);
    wp += m.wpo * 0.01 * gProps.z + n * m.disp * gProps.y;
#endif
    o.pos = mul(gViewProj, float4(wp, 1)); o.wpos = wp; o.nrm = n; o.tan = float4(t, i.tan.w); o.uv = i.uv;
    return o;
}
// (atan2 / acos spelled out as polynomials: some HLSL compilers, like Wine's, don't have them)
float AtanP(float x) { float x2 = x * x; return x * (0.99997726 + x2 * (-0.33262347 + x2 * (0.19354346 + x2 * (-0.11643287 + x2 * (0.05265332 + x2 * -0.01172120))))); }
float Atan2(float y, float x) {
    float ax = abs(x), ay = abs(y), a = AtanP(min(ax, ay) / max(max(ax, ay), 1e-8));
    if (ay > ax) a = PI / 2 - a;
    if (x < 0) a = PI - a;
    return y < 0 ? -a : a;
}
float Acos(float x) { float ax = abs(x); float r = sqrt(max(1 - ax, 0.0)) * (1.5707288 + ax * (-0.2121144 + ax * (0.0742610 - 0.0187293 * ax))); return x < 0 ? PI - r : r; }
float2 EnvUv(float3 d) { d = normalize(d); return float2(Atan2(d.x, d.z) / (2 * PI) + 0.5 + gMisc.z, Acos(clamp(d.y, -1, 1)) / PI); }
float3 Env(float3 d, float mip) { return gEnv.SampleLevel(gEnvS, EnvUv(d), mip).rgb * gMisc.y; }
float3 Aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float4 PSMain(VOut i, bool front : SV_IsFrontFace) : SV_Target {
    float3 N = normalize(i.nrm), T = normalize(i.tan.xyz - N * dot(i.tan.xyz, N));
    float3 B = cross(N, T) * (i.tan.w < 0 ? -1.0 : 1.0);
    if (!front) { N = -N; T = -T; B = -B; }
    float3 V = normalize(gCam.xyz - i.wpos);
    Ctx c; c.uv = i.uv; c.wpos = i.wpos; c.N = N; c.V = V; c.Vts = float3(dot(V, T), dot(V, B), dot(V, N)); c.time = gMisc.x;
    Mat m = Eval(c);
#if BLEND == 1
    clip(m.opmask - gProps.x);
#endif
    float3 nt = normalize(m.nrm);
    float3 Nw = normalize(T * nt.x + B * nt.y + N * nt.z);
    float3 col;
#if UNLIT
    col = m.emis;
#else
    float3 base = saturate(m.base); float metal = saturate(m.metal); float rough = clamp(m.rough, 0.03, 1.0); float spec = saturate(m.spec);
    float3 F0 = lerp(0.08 * spec.xxx, base, metal), diff = base * (1 - metal);
    float3 L = normalize(gLight.xyz), H = normalize(L + V);
    float ndl = saturate(dot(Nw, L)), ndv = max(dot(Nw, V), 1e-4), ndh = saturate(dot(Nw, H)), vdh = saturate(dot(V, H));
    float a = rough * rough, a2 = a * a, dd = ndh * ndh * (a2 - 1) + 1, D = a2 / (PI * dd * dd);
    float k = (rough + 1) * (rough + 1) / 8, G = (ndv / (ndv * (1 - k) + k)) * (ndl / (ndl * (1 - k) + k));
    float3 F = F0 + (1 - F0) * pow(1 - vdh, 5);
    float3 direct = (diff / PI * (1 - F) + D * G * F / max(4 * ndl * ndv, 1e-4)) * gLightCol.rgb * gLight.w * ndl;
    float mips = gMisc.w;
    float3 irr = Env(Nw, max(mips - 2, 0)), pre = Env(reflect(-V, Nw), rough * (mips - 1));
    float4 c0 = float4(-1, -0.0275, -0.572, 0.022), c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = rough * c0 + c1; float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y; float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    float3 amb = (diff * irr + pre * (F0 * AB.x + AB.y)) * saturate(m.ao);
    col = direct + amb + m.emis;
#endif
    col *= gView.x;
    float alpha = 1;
#if BLEND == 2
    alpha = saturate(m.opac);
#endif
    float3 outc = pow(Aces(col), 1 / 2.2);
    return float4(outc, alpha);
}
// the background: the environment, seen through the camera
struct BOut { float4 pos : SV_Position; float2 ndc : TEXCOORD0; };
BOut BgVS(uint id : SV_VertexID) { BOut o; float2 p = float2((id << 1) & 2, id & 2); o.ndc = p * float2(2, -2) + float2(-1, 1); o.pos = float4(o.ndc, 1, 1); return o; }
float4 BgPS(BOut i) : SV_Target {
    float4 w = mul(gInvViewProj, float4(i.ndc, 1, 1)); float3 d = normalize(w.xyz / w.w - gCam.xyz);
    float3 col = gView.y > 0.5 ? Env(d, 1.5) : float3(0.04, 0.04, 0.045);
    return float4(pow(Aces(col * gView.x), 1 / 2.2), 1);
}
)HLSL";

// The whole shader for the graph as it is now (also what "Export shader code" saves)
static MsCompiled MsGenerate() {
    MsCompiled r; r.props = M.props;
    MsGen g; g.out = &r;
    std::string evalBody;
    struct Want { int k; const char* field; int comps; const char* def; };
    const Want want[] = {
        { STD_BASE, "base", 3, "float3(0, 0, 0)" }, { STD_METAL, "metal", 1, "0" }, { STD_SPEC, "spec", 1, "0.5" }, { STD_ROUGH, "rough", 1, "0.5" },
        { STD_EMIS, "emis", 3, "float3(0, 0, 0)" }, { STD_OPAC, "opac", 1, "1" }, { STD_OPMASK, "opmask", 1, "1" }, { STD_NORMAL, "nrm", 3, "float3(0, 0, 1)" },
        { STD_AO, "ao", 1, "1" }, { STD_WPO, "wpo", 3, "float3(0, 0, 0)" }, { STD_DISP, "disp", 1, "0" } };
    std::string assign;
    for (auto& w : want) {
        std::string val = w.def;
        auto it = M.links.find(M.stdStart + w.k);
        if (it != M.links.end()) {
            MeNode* s = MeFind(it->second);
            if (s) {
                int c = g.make(*s);
                if (c > 0) {
                    auto sw = g.swz(*s, M.linkOut.count(M.stdStart + w.k) ? M.linkOut[M.stdStart + w.k] : 0, c);
                    val = w.comps == 1 ? "(" + sw.first + ").x" : "(" + sw.first + ").xyz";
                    if (w.comps == 3 && sw.second == 2) r.warns.push_back({ s->id, std::string(g_meStd[w.k]) + " gets 2 channels; the third is 0" });
                }
            }
        }
        assign += std::string("    m.") + w.field + " = " + val + ";\n";
    }
    // what can't be baked for the game
    auto wired = [&](int k) { return M.links.count(M.stdStart + k) > 0; };
    if (wired(STD_WPO)) r.warns.push_back({ -1, "World Position Offset moves the mesh in the preview only (a baked texture can't move vertices)" });
    if (wired(STD_DISP)) r.warns.push_back({ -1, "Displacement moves the mesh in the preview only" });
    for (auto& kv : g.n) if (MeNode* nd = MeFind(kv.first)) if (nd->kind == MEK_OP) {
        if (nd->op == OP_TIME || nd->op == OP_PANNER || nd->op == OP_ROTATOR) { r.warns.push_back({ nd->id, std::string(g_meOpName[nd->op]) + " moves in the preview; baked textures are frozen at time 0" }); }
        if (nd->op == OP_FRESNEL || nd->op == OP_CAMVEC || nd->op == OP_BUMPOFFSET || nd->op == OP_VNORMAL || nd->op == OP_WORLDPOS)
            r.warns.push_back({ nd->id, std::string(g_meOpName[nd->op]) + " depends on the camera or the world: baked as if seen straight on" });
    }
    if (r.slots > MS_MAX_SLOTS) r.errs.push_back({ -1, "The graph has too many values for the preview (" + std::to_string(r.slots) + " of " + std::to_string(MS_MAX_SLOTS) + ")" });
    std::string defs = "#define BLEND " + std::to_string(r.props.blend == BLEND_MASKED ? 1 : (r.props.blend == BLEND_TRANSLUCENT || r.props.blend == BLEND_ADDITIVE) ? 2 : 0) + "\n#define UNLIT " + (r.props.unlit ? "1" : "0") + "\n";
    r.hlsl = "// Generated by the UE4SS Launcher material editor\n" + defs + kMsHead + "\nMat Eval(Ctx c) {\n    Mat m;\n" + g.body + assign + "    return m;\n}\n" + kMsTail;
    // the shape key: everything that changes the code (values that live in the constant buffer don't)
    r.key = r.hlsl;
    for (auto& t : r.tex) r.key += "|" + t.value + "#" + std::to_string(t.sampler) + "#" + std::to_string(t.filter);
    return r;
}

static void MsReflect(ID3DBlob* b, int& instr, int* tex) {
    ID3D11ShaderReflection* rf = nullptr;
    if (b && SUCCEEDED(D3DReflect(b->GetBufferPointer(), b->GetBufferSize(), IID_ID3D11ShaderReflection, (void**)&rf)) && rf) {
        D3D11_SHADER_DESC d{}; rf->GetDesc(&d); instr = (int)d.InstructionCount; if (tex) *tex = (int)d.TextureNormalInstructions + (int)d.TextureLoadInstructions + (int)d.TextureBiasInstructions + (int)d.TextureGradientInstructions + (int)d.TextureCompInstructions;
        rf->Release();
    }
}
static void MsStartCompile(MsCompiled c) {
    g_msBusy = true;
    std::thread([c]() mutable {
        auto job = std::make_unique<MsJob>(); job->c = std::move(c);
        ULONGLONG t0 = GetTickCount64();
        if (job->c.errs.empty()) {
            ID3DBlob* err = nullptr;
            D3D_SHADER_MACRO vdef[] = { { "VERTEX", "1" }, { nullptr, nullptr } };
            UINT fl = D3DCOMPILE_OPTIMIZATION_LEVEL2;
            HRESULT hv = D3DCompile(job->c.hlsl.data(), job->c.hlsl.size(), "material.hlsl", vdef, nullptr, "VSMain", "vs_5_0", fl, 0, &job->vs, &err);
            if (err) { job->log += std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()); err->Release(); err = nullptr; }
            HRESULT hp = D3DCompile(job->c.hlsl.data(), job->c.hlsl.size(), "material.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", fl, 0, &job->ps, &err);
            if (err) { job->log += std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()); err->Release(); }
            job->ok = SUCCEEDED(hv) && SUCCEEDED(hp);
            MsReflect(job->vs, job->vsI, nullptr); MsReflect(job->ps, job->psI, &job->psT);
        }
        job->ms = (double)(GetTickCount64() - t0);
        std::lock_guard<std::mutex> lk(g_msJobMx); g_msDone = std::move(job); g_msBusy = false; g_framesLeft = 3;
    }).detach();
}

// ---- the preview: meshes, environment, render target
struct MsVert { float p[3], n[3], t[4], uv[2]; };
struct MsMesh { ID3D11Buffer* vb = nullptr; ID3D11Buffer* ib = nullptr; UINT count = 0; std::string err; };
enum { MS_SPHERE, MS_CUBE, MS_PLANE, MS_CYLINDER, MS_TORUS, MS_CUSTOM, MS_SHAPES };
static const char* g_msShapeName[MS_SHAPES] = { "Sphere", "Cube", "Plane", "Cylinder", "Torus", "Custom mesh" };
enum { ENV_STUDIO, ENV_OUTDOOR, ENV_SUNSET, ENV_NIGHT, ENV_GREY, ENV_HDRI, ENV_COUNT };
static const char* g_msEnvName[ENV_COUNT] = { "Studio", "Outdoor (sun and sky)", "Sunset", "Night", "Plain grey", "HDRI file..." };
static struct MsPreview {
    MsMesh mesh[MS_SHAPES]; int shape = MS_SPHERE; std::wstring customFile; std::string customName;
    ID3D11Texture2D* rt = nullptr; ID3D11Texture2D* rtMs = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D* ds = nullptr; ID3D11DepthStencilView* dsv = nullptr; int w = 0, h = 0;
    ID3D11Buffer* cbFrame = nullptr; ID3D11Buffer* cbParams = nullptr;
    ID3D11VertexShader* bgVs = nullptr; ID3D11PixelShader* bgPs = nullptr; bool bgTried = false;
    ID3D11SamplerState* smp[3] = {}; ID3D11SamplerState* envSmp = nullptr;
    ID3D11RasterizerState* rsBack = nullptr; ID3D11RasterizerState* rsNone = nullptr;
    ID3D11BlendState* bsOpaque = nullptr; ID3D11BlendState* bsAlpha = nullptr; ID3D11BlendState* bsAdd = nullptr;
    ID3D11DepthStencilState* dsWrite = nullptr; ID3D11DepthStencilState* dsRead = nullptr; ID3D11DepthStencilState* dsOff = nullptr;
    // environment
    int env = ENV_STUDIO, envBuilt = -1; std::wstring hdri, hdriBuilt; ID3D11ShaderResourceView* envSrv = nullptr; int envMips = 1; std::string envErr;
    float envIntensity = 1.f, envTurn = 0.f, exposure = 1.f; bool showBg = true;
    // camera and light
    float yaw = 0.6f, pitch = 0.25f, dist = 3.2f, tx = 0, ty = 0, tz = 0;
    float lightYaw = -0.9f, lightPitch = 0.75f, lightInt = 1.6f;
    bool spin = false; float spinAngle = 0;
    ULONGLONG start = 0; bool realtime = true;
} g_mp;


// tangents worked out from the UVs (so normal maps light the right way on every mesh)
static void MsTangents(std::vector<MsVert>& v, const std::vector<uint32_t>& idx) {
    std::vector<float> tan(v.size() * 3, 0), bit(v.size() * 3, 0);
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        MsVert &a = v[idx[t]], &b = v[idx[t + 1]], &c = v[idx[t + 2]];
        float e1[3] = { b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2] }, e2[3] = { c.p[0] - a.p[0], c.p[1] - a.p[1], c.p[2] - a.p[2] };
        float du1 = b.uv[0] - a.uv[0], dv1 = b.uv[1] - a.uv[1], du2 = c.uv[0] - a.uv[0], dv2 = c.uv[1] - a.uv[1];
        float det = du1 * dv2 - du2 * dv1; if (fabsf(det) < 1e-12f) continue;
        float r = 1 / det;
        for (int k = 0; k < 3; k++) {
            float T = (e1[k] * dv2 - e2[k] * dv1) * r, Bt = (e2[k] * du1 - e1[k] * du2) * r;
            for (uint32_t i : { idx[t], idx[t + 1], idx[t + 2] }) { tan[i * 3 + k] += T; bit[i * 3 + k] += Bt; }
        }
    }
    for (size_t i = 0; i < v.size(); i++) {
        float* n = v[i].n; float* t = &tan[i * 3];
        float d = n[0] * t[0] + n[1] * t[1] + n[2] * t[2];
        float o[3] = { t[0] - n[0] * d, t[1] - n[1] * d, t[2] - n[2] * d };
        float l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (l < 1e-8f) { float a[3] = { fabsf(n[1]) < 0.9f ? 0.f : 1.f, fabsf(n[1]) < 0.9f ? 1.f : 0.f, 0 }; o[0] = a[1] * n[2] - a[2] * n[1]; o[1] = a[2] * n[0] - a[0] * n[2]; o[2] = a[0] * n[1] - a[1] * n[0]; l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) + 1e-12f; }
        for (int k = 0; k < 3; k++) v[i].t[k] = o[k] / l;
        float cx = n[1] * v[i].t[2] - n[2] * v[i].t[1], cy = n[2] * v[i].t[0] - n[0] * v[i].t[2], cz = n[0] * v[i].t[1] - n[1] * v[i].t[0];
        float* b = &bit[i * 3];
        v[i].t[3] = (cx * b[0] + cy * b[1] + cz * b[2]) < 0 ? -1.f : 1.f;
    }
}
// front faces are clockwise for Direct3D: turn any triangle whose order disagrees with its normals
static void MsFixWinding(const std::vector<MsVert>& v, std::vector<uint32_t>& idx) {
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const MsVert &a = v[idx[t]], &b = v[idx[t + 1]], &c = v[idx[t + 2]];
        float e1[3] = { b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2] }, e2[3] = { c.p[0] - a.p[0], c.p[1] - a.p[1], c.p[2] - a.p[2] };
        float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float nn[3] = { a.n[0] + b.n[0] + c.n[0], a.n[1] + b.n[1] + c.n[1], a.n[2] + b.n[2] + c.n[2] };
        if (cr[0] * nn[0] + cr[1] * nn[1] + cr[2] * nn[2] < 0) std::swap(idx[t + 1], idx[t + 2]);
    }
}
static void MsUpload(MsMesh& m, std::vector<MsVert>& v, std::vector<uint32_t>& idx) {
    MsRel(m.vb); MsRel(m.ib); m.count = 0;
    if (v.empty() || idx.empty() || !g_dev) return;
    MsFixWinding(v, idx); MsTangents(v, idx);
    D3D11_BUFFER_DESC bd{}; bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.ByteWidth = (UINT)(v.size() * sizeof(MsVert));
    D3D11_SUBRESOURCE_DATA sd{ v.data(), 0, 0 };
    g_dev->CreateBuffer(&bd, &sd, &m.vb);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER; bd.ByteWidth = (UINT)(idx.size() * 4); sd.pSysMem = idx.data();
    g_dev->CreateBuffer(&bd, &sd, &m.ib);
    m.count = (UINT)idx.size();
}
static MsVert MsV(float x, float y, float z, float nx, float ny, float nz, float u, float v) { MsVert r{}; r.p[0] = x; r.p[1] = y; r.p[2] = z; r.n[0] = nx; r.n[1] = ny; r.n[2] = nz; r.uv[0] = u; r.uv[1] = v; return r; }
static void MsGrid(std::vector<MsVert>& v, std::vector<uint32_t>& idx, int cols, int rows, const std::function<MsVert(float, float)>& at) {
    uint32_t base = (uint32_t)v.size();
    for (int j = 0; j <= rows; j++) for (int i = 0; i <= cols; i++) v.push_back(at((float)i / cols, (float)j / rows));
    for (int j = 0; j < rows; j++) for (int i = 0; i < cols; i++) {
        uint32_t a = base + j * (cols + 1) + i, b = a + 1, c = a + cols + 1, d = c + 1;
        idx.insert(idx.end(), { a, c, b, b, c, d });
    }
}
static void MsBuildShape(int s) {
    std::vector<MsVert> v; std::vector<uint32_t> idx;
    const float PI = 3.14159265f;
    switch (s) {
    case MS_SPHERE:
        MsGrid(v, idx, 128, 64, [&](float u, float t) { float th = u * 2 * PI, ph = t * PI; float x = sinf(ph) * sinf(th), y = cosf(ph), z = sinf(ph) * cosf(th); return MsV(x, y, z, x, y, z, u * 2, t); });
        break;
    case MS_CUBE: {
        float n6[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for (auto& n : n6) {
            float up[3] = { 0, fabsf(n[1]) > 0.5f ? 0.f : 1.f, fabsf(n[1]) > 0.5f ? 1.f : 0.f };
            float rt[3] = { up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0] };
            MsGrid(v, idx, 32, 32, [&](float a, float b) {
                float x = (a * 2 - 1) * 0.8f, y = (1 - b * 2) * 0.8f;
                return MsV(n[0] * 0.8f + rt[0] * x + up[0] * y, n[1] * 0.8f + rt[1] * x + up[1] * y, n[2] * 0.8f + rt[2] * x + up[2] * y, n[0], n[1], n[2], a, b);
            });
        }
        break;
    }
    case MS_PLANE:
        MsGrid(v, idx, 96, 96, [&](float a, float b) { return MsV((a * 2 - 1) * 1.3f, 0, (1 - b * 2) * 1.3f, 0, 1, 0, a * 2, b * 2); });
        break;
    case MS_CYLINDER:
        MsGrid(v, idx, 96, 24, [&](float u, float t) { float th = u * 2 * PI; float x = sinf(th) * 0.75f, z = cosf(th) * 0.75f; return MsV(x, 1 - t * 2, z, sinf(th), 0, cosf(th), u * 3, t * 2); });
        for (int cap = 0; cap < 2; cap++) {
            float y = cap ? 1.f : -1.f, ny = cap ? 1.f : -1.f;
            MsGrid(v, idx, 96, 12, [&](float u, float t) { float th = u * 2 * PI, r = t * 0.75f; return MsV(sinf(th) * r, y, cosf(th) * r, 0, ny, 0, 0.5f + sinf(th) * t * 0.5f, 0.5f + cosf(th) * t * 0.5f); });
        }
        break;
    case MS_TORUS:
        MsGrid(v, idx, 128, 48, [&](float u, float t) {
            float th = u * 2 * PI, ph = t * 2 * PI, R = 0.9f, r = 0.38f;
            float cx = sinf(th), cz = cosf(th), nx = cx * cosf(ph), ny = sinf(ph), nz = cz * cosf(ph);
            return MsV(cx * R + nx * r, ny * r, cz * R + nz * r, nx, ny, nz, u * 4, t);
        });
        break;
    }
    MsUpload(g_mp.mesh[s], v, idx);
}
// a .uemodel (the launcher's own model format: Unreal's axes, centimetres), fitted into the preview
static bool MsLoadUemodel(const std::wstring& file, std::string& err) {
    std::string d; if (!MeReadAll(file, d) || d.rfind("UEMODEL", 0) != 0) { err = "Couldn't read that model"; return false; }
    std::vector<MsVert> v; std::vector<uint32_t> idx; uint32_t base = 0, partV = 0;
    std::istringstream ss(d); std::string line;
    while (std::getline(ss, line)) {
        if (line.size() < 2) continue;
        if (line.rfind("part\t", 0) == 0) { base = (uint32_t)v.size(); partV = 0; continue; }
        if (line[0] == 'v' && line[1] == ' ') {
            float p[3], n[3], col[3], uv[2];
            if (sscanf(line.c_str() + 2, "%f %f %f %f %f %f %f %f %f %f %f", &p[0], &p[1], &p[2], &n[0], &n[1], &n[2], &col[0], &col[1], &col[2], &uv[0], &uv[1]) < 11) continue;
            // Unreal: x forward, y right, z up (left-handed); the preview: x right, y up, z forward (left-handed too)
            v.push_back(MsV(p[1], p[2], p[0], n[1], n[2], n[0], uv[0], uv[1])); partV++;
        } else if (line[0] == 't' && line[1] == ' ') {
            unsigned a, b, c; if (sscanf(line.c_str() + 2, "%u %u %u", &a, &b, &c) == 3 && a < partV && b < partV && c < partV) idx.insert(idx.end(), { base + a, base + b, base + c });
        }
    }
    if (v.empty() || idx.empty()) { err = "That model has no triangles"; return false; }
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (auto& x : v) for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], x.p[k]); hi[k] = std::max(hi[k], x.p[k]); }
    float c[3] = { (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2 }, r = 0;
    for (auto& x : v) { float dx = x.p[0] - c[0], dy = x.p[1] - c[1], dz = x.p[2] - c[2]; r = std::max(r, sqrtf(dx * dx + dy * dy + dz * dz)); }
    if (r < 1e-6f) r = 1;
    for (auto& x : v) for (int k = 0; k < 3; k++) x.p[k] = (x.p[k] - c[k]) / r * 1.2f;
    MsUpload(g_mp.mesh[MS_CUSTOM], v, idx);
    return g_mp.mesh[MS_CUSTOM].count > 0;
}

// float -> half (for the environment, which needs values brighter than 1)
static uint16_t MsHalf(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000; int e = (int)((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff) return (uint16_t)(sign | 0x7c00 | (m ? 0x200 : 0));
    if (e <= 0) { if (e < -10) return (uint16_t)sign; m |= 0x800000; return (uint16_t)(sign | (m >> (14 - e))); }
    if (e >= 31) return (uint16_t)(sign | 0x7c00);
    return (uint16_t)(sign | (e << 10) | (m >> 13));
}
// The built-in environments are painted into a small panorama (the same layout as an HDRI file)
static void MsPaintEnv(int env, std::vector<float>& px, int W, int H) {
    px.assign((size_t)W * H * 4, 0);
    auto sun = [](float dx, float dy, float dz, float sx, float sy, float sz, float size, float power) {
        float d = dx * sx + dy * sy + dz * sz; return d > 1 - size ? power * powf((d - (1 - size)) / size, 2.f) : 0.f;
    };
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float u = (x + 0.5f) / W, v = (y + 0.5f) / H;
        float th = (u - 0.5f) * 2 * 3.14159265f, ph = v * 3.14159265f;
        float dx = sinf(ph) * sinf(th), dy = cosf(ph), dz = sinf(ph) * cosf(th);
        float r = 0, g = 0, b = 0;
        switch (env) {
        case ENV_STUDIO: {
            float t = dy * 0.5f + 0.5f; r = g = b = 0.06f + 0.22f * t;
            // soft boxes: key (front left), rim (back right), top
            float k = sun(dx, dy, dz, -0.6f, 0.45f, -0.66f, 0.12f, 6.f), rim = sun(dx, dy, dz, 0.75f, 0.3f, 0.6f, 0.08f, 4.f), top = sun(dx, dy, dz, 0, 1, 0, 0.18f, 2.f);
            r += k + rim + top; g += k + rim + top; b += k * 1.02f + rim * 1.05f + top;
            if (dy < -0.05f) { r = g = b = 0.05f; }
            break;
        }
        case ENV_OUTDOOR: {
            if (dy >= 0) { float t = powf(1 - dy, 3.f); r = 0.25f + 0.45f * t; g = 0.45f + 0.4f * t; b = 0.95f + 0.1f * t; }
            else { float t = std::min(1.f, -dy * 4); r = 0.22f - 0.08f * t; g = 0.2f - 0.06f * t; b = 0.16f - 0.05f * t; }
            float s = sun(dx, dy, dz, -0.5f, 0.6f, -0.62f, 0.002f, 400.f) + sun(dx, dy, dz, -0.5f, 0.6f, -0.62f, 0.03f, 4.f);
            r += s; g += s * 0.95f; b += s * 0.85f;
            break;
        }
        case ENV_SUNSET: {
            if (dy >= 0) { float t = powf(1 - dy, 4.f); r = 0.15f + 1.4f * t; g = 0.12f + 0.55f * t; b = 0.3f + 0.1f * t; }
            else { r = 0.06f; g = 0.05f; b = 0.06f; }
            float s = sun(dx, dy, dz, 0.0f, 0.05f, -1.0f, 0.003f, 120.f) + sun(dx, dy, dz, 0.0f, 0.05f, -1.0f, 0.08f, 3.f);
            r += s; g += s * 0.55f; b += s * 0.25f;
            break;
        }
        case ENV_NIGHT: {
            if (dy >= 0) { float t = 1 - dy; r = 0.01f + 0.03f * t; g = 0.015f + 0.04f * t; b = 0.04f + 0.08f * t; } else { r = g = b = 0.01f; }
            float m = sun(dx, dy, dz, 0.4f, 0.7f, 0.59f, 0.0015f, 80.f) + sun(dx, dy, dz, 0.4f, 0.7f, 0.59f, 0.02f, 1.f);
            r += m * 0.8f; g += m * 0.85f; b += m;
            uint32_t hsh = (uint32_t)(x * 73856093u ^ y * 19349663u); if (dy > 0.1f && (hsh % 997) == 0) { r += 2; g += 2; b += 2; }
            break;
        }
        default: r = g = b = 0.5f; break;
        }
        float* p = &px[((size_t)y * W + x) * 4]; p[0] = r; p[1] = g; p[2] = b; p[3] = 1;
    }
}
// an equirectangular .hdr file
static bool MsLoadHdr(const std::wstring& f, std::vector<float>& px, int& W, int& H, std::string& err);
static void MsMakeEnvTexture(const std::vector<float>& px, int W, int H) {
    MsRel(g_mp.envSrv);
    std::vector<uint16_t> hp(px.size()); for (size_t i = 0; i < px.size(); i++) hp[i] = MsHalf(px[i]);
    D3D11_TEXTURE2D_DESC d{}; d.Width = W; d.Height = H; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t))) return;
    g_ctx->UpdateSubresource(t, 0, nullptr, hp.data(), W * 8, 0);
    g_dev->CreateShaderResourceView(t, nullptr, &g_mp.envSrv);
    if (g_mp.envSrv) g_ctx->GenerateMips(g_mp.envSrv);
    t->GetDesc(&d); g_mp.envMips = (int)d.MipLevels;
    t->Release();
}
static void MsEnsureEnv() {
    bool want = g_mp.env != g_mp.envBuilt || (g_mp.env == ENV_HDRI && g_mp.hdri != g_mp.hdriBuilt) || !g_mp.envSrv;
    if (!want) return;
    std::vector<float> px; int W = 512, H = 256;
    g_mp.envErr.clear();
    if (g_mp.env == ENV_HDRI) {
        std::string err;
        if (g_mp.hdri.empty() || !MsLoadHdr(g_mp.hdri, px, W, H, err)) { g_mp.envErr = err.empty() ? "Pick an .hdr file" : err; MsPaintEnv(ENV_GREY, px, W = 64, H = 32); }
    } else MsPaintEnv(g_mp.env, px, W, H);
    MsMakeEnvTexture(px, W, H);
    g_mp.envBuilt = g_mp.env; g_mp.hdriBuilt = g_mp.hdri;
}

// the textures the preview samples: the graph's own pictures (read in the background, like baking does)
struct MsGpuTex { ID3D11ShaderResourceView* srv = nullptr; int state = 0; std::shared_ptr<MgImg> img; };
static std::map<std::string, MsGpuTex> g_msTex; static std::mutex g_msTexMx;
static ID3D11ShaderResourceView* g_msGrey = nullptr; static ID3D11ShaderResourceView* g_msFlat = nullptr;
static ID3D11ShaderResourceView* MsSolid(uint32_t rgba) {
    D3D11_TEXTURE2D_DESC d{}; d.Width = d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ &rgba, 4, 0 }; ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* s = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, &sd, &t))) { g_dev->CreateShaderResourceView(t, nullptr, &s); t->Release(); }
    return s;
}
static ID3D11ShaderResourceView* MsTexture(const std::string& value, bool srgb) {
    if (!g_msGrey) { g_msGrey = MsSolid(0xff808080u); g_msFlat = MsSolid(0xffff8080u); }
    if (value.empty()) return nullptr;
    std::string key = value + (srgb ? "|s" : "|l");
    std::lock_guard<std::mutex> lk(g_msTexMx);
    MsGpuTex& t = g_msTex[key];
    if (t.state == 0) {
        t.state = 1;
        AesSyncNow();
        std::wstring paks = PaksDir();
        std::thread([key, value, paks] {
            auto img = MgLoadImg(value, paks);
            std::lock_guard<std::mutex> lk2(g_msTexMx); auto& tt = g_msTex[key]; tt.img = img; tt.state = img && img->ok ? 2 : 4; g_framesLeft = 3;
        }).detach();
    }
    if (t.state == 2 && t.img) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = t.img->w; d.Height = t.img->h; d.MipLevels = 0; d.ArraySize = 1;
        d.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &tex))) {
            g_ctx->UpdateSubresource(tex, 0, nullptr, t.img->px.data(), t.img->w * 4, 0);
            g_dev->CreateShaderResourceView(tex, nullptr, &t.srv);
            if (t.srv) g_ctx->GenerateMips(t.srv);
            tex->Release();
        }
        t.state = t.srv ? 3 : 4; t.img.reset();
    }
    return t.state == 3 ? t.srv : nullptr;
}
// forget pictures of a file that changed (the texture editor saved it again)
static void MsForgetTexture(const std::string& value) {
    std::lock_guard<std::mutex> lk(g_msTexMx);
    for (auto it = g_msTex.begin(); it != g_msTex.end();) {
        if (it->first.rfind(value + "|", 0) == 0) { MsRel(it->second.srv); it = g_msTex.erase(it); } else ++it;
    }
    std::lock_guard<std::mutex> lk2(g_mgImgMx); g_mgImgs.erase(value);
}
static bool MsLoadingTextures() { std::lock_guard<std::mutex> lk(g_msTexMx); for (auto& kv : g_msTex) if (kv.second.state == 1) return true; return false; }

static void MsInitStatic() {
    if (g_mp.cbFrame || !g_dev) return;
    D3D11_BUFFER_DESC bd{}; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = 16 * 4 * 3 + 16 * 6; g_dev->CreateBuffer(&bd, nullptr, &g_mp.cbFrame);
    bd.ByteWidth = 16 * MS_MAX_SLOTS; g_dev->CreateBuffer(&bd, nullptr, &g_mp.cbParams);
    D3D11_SAMPLER_DESC sd{}; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; g_dev->CreateSamplerState(&sd, &g_mp.smp[0]);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; g_dev->CreateSamplerState(&sd, &g_mp.smp[1]);
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8; g_dev->CreateSamplerState(&sd, &g_mp.smp[2]);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.MaxAnisotropy = 1; sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; g_dev->CreateSamplerState(&sd, &g_mp.envSmp);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_BACK; rd.DepthClipEnable = TRUE; rd.MultisampleEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_mp.rsBack); rd.CullMode = D3D11_CULL_NONE; g_dev->CreateRasterizerState(&rd, &g_mp.rsNone);
    D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bl, &g_mp.bsOpaque);
    bl.RenderTarget[0].BlendEnable = TRUE; bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    g_dev->CreateBlendState(&bl, &g_mp.bsAlpha);
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; g_dev->CreateBlendState(&bl, &g_mp.bsAdd);
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    g_dev->CreateDepthStencilState(&dd, &g_mp.dsWrite); dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; g_dev->CreateDepthStencilState(&dd, &g_mp.dsRead);
    dd.DepthEnable = FALSE; g_dev->CreateDepthStencilState(&dd, &g_mp.dsOff);
    for (int s = 0; s < MS_CUSTOM; s++) MsBuildShape(s);
    g_mp.start = GetTickCount64();
}
static void MsEnsureBg() {
    if (g_mp.bgTried) return;
    g_mp.bgTried = true;
    std::string src = std::string("#define BLEND 0\n#define UNLIT 0\n") + kMsHead + "\nMat Eval(Ctx c) { Mat m = (Mat)0; return m; }\n" + kMsTail;
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    if (SUCCEEDED(D3DCompile(src.data(), src.size(), "bg", nullptr, nullptr, "BgVS", "vs_5_0", 0, 0, &vb, &err))) g_dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_mp.bgVs);
    MsRel(err);
    if (SUCCEEDED(D3DCompile(src.data(), src.size(), "bg", nullptr, nullptr, "BgPS", "ps_5_0", 0, 0, &pb, &err))) g_dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_mp.bgPs);
    MsRel(err); MsRel(vb); MsRel(pb);
}
static void MsEnsureTarget(int w, int h) {
    if (w == g_mp.w && h == g_mp.h && g_mp.rtv) return;
    MsRel(g_mp.rtv); MsRel(g_mp.srv); MsRel(g_mp.rt); MsRel(g_mp.rtMs); MsRel(g_mp.dsv); MsRel(g_mp.ds);
    g_mp.w = w; g_mp.h = h;
    UINT q = 0; g_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &q);
    UINT samples = q > 0 ? 4 : 1;
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = samples;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
    g_dev->CreateTexture2D(&d, nullptr, &g_mp.rtMs);
    if (g_mp.rtMs) g_dev->CreateRenderTargetView(g_mp.rtMs, nullptr, &g_mp.rtv);
    d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&d, nullptr, &g_mp.rt);
    if (g_mp.rt) g_dev->CreateShaderResourceView(g_mp.rt, nullptr, &g_mp.srv);
    d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL; d.SampleDesc.Count = samples;
    g_dev->CreateTexture2D(&d, nullptr, &g_mp.ds);
    if (g_mp.ds) g_dev->CreateDepthStencilView(g_mp.ds, nullptr, &g_mp.dsv);
}

// ---- once a frame: compile if the graph's shape changed, pick up a finished compile
static void MsUpdateShader() {
    {
        std::unique_ptr<MsJob> job;
        { std::lock_guard<std::mutex> lk(g_msJobMx); job.swap(g_msDone); }
        if (job) {
            MsRel(g_ms.vs); MsRel(g_ms.ps); MsRel(g_ms.il);
            g_ms.src = job->c; g_ms.log = job->log; g_ms.ok = false; g_ms.ms = job->ms; g_ms.vsInstr = job->vsI; g_ms.psInstr = job->psI; g_ms.psTex = job->psT;
            if (job->ok && job->vs && job->ps) {
                g_dev->CreateVertexShader(job->vs->GetBufferPointer(), job->vs->GetBufferSize(), nullptr, &g_ms.vs);
                g_dev->CreatePixelShader(job->ps->GetBufferPointer(), job->ps->GetBufferSize(), nullptr, &g_ms.ps);
                D3D11_INPUT_ELEMENT_DESC el[] = {
                    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                    { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                    { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
                g_dev->CreateInputLayout(el, 4, job->vs->GetBufferPointer(), job->vs->GetBufferSize(), &g_ms.il);
                g_ms.ok = g_ms.vs && g_ms.ps && g_ms.il;
            }
            MsRel(job->vs); MsRel(job->ps);
            M.errNode = -1; for (auto& e : g_ms.src.errs) if (e.first >= 0) { M.errNode = e.first; break; }
        }
    }
    if (g_msBusy) return;
    // (the graph's shape is checked a few times a second, not every frame)
    static ULONGLONG lastCheck = 0;
    if (!g_msForce && GetTickCount64() - lastCheck < 120) return;
    lastCheck = GetTickCount64();
    MsCompiled c = MsGenerate();
    if (!g_msForce && c.key == g_ms.builtKey) return;
    g_msForce = false;
    g_ms.builtKey = c.key;
    MsStartCompile(std::move(c));
}
// every value the shader reads, from the nodes as they are now
static void MsFillParams(std::vector<float>& P) {
    P.assign(MS_MAX_SLOTS * 4, 0.f);
    for (auto& kv : g_ms.src.slotOf) {
        MeNode* n = MeFind(kv.first); if (!n) continue;
        float* s = &P[kv.second * 4];
        if (kv.second + 1 > MS_MAX_SLOTS) continue;
        if (n->kind == MEK_SCA) { s[0] = (float)atof(MeParamValue(*n).c_str()); }
        else if (n->kind == MEK_VEC) { MeColour(MeParamValue(*n), s); }
        else if (n->kind == MEK_TEX && kv.second + 2 <= MS_MAX_SLOTS) { s[0] = n->tile[0]; s[1] = n->tile[1]; s[2] = n->off[0]; s[3] = n->off[1]; s[4] = n->rot; }
        else if (n->kind == MEK_OP) {
            memcpy(s, n->c, sizeof n->c);
            if (n->op == OP_LAYERBLEND && kv.second + 3 <= MS_MAX_SLOTS) for (size_t l = 0; l < n->layers.size() && l < 8; l++) s[4 + l] = n->layers[l].second;
        }
    }
}
static void MsMul(const float a[16], const float b[16], float o[16]) { for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { float s = 0; for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c]; o[r * 4 + c] = s; } }
static bool MsInvert(const float m[16], float inv[16]) {
    float t[16];
    t[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    t[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    t[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    t[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    t[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    t[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    t[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    t[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    t[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    t[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    t[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    t[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    t[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    t[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    t[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    t[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    float det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
    if (fabsf(det) < 1e-12f) return false;
    for (int i = 0; i < 16; i++) inv[i] = t[i] / det;
    return true;
}
// Draws the preview into its own picture (row-major matrices, used as mul(M, v) in the shader)
static void MsRender(int w, int h) {
    if (!g_dev || w < 8 || h < 8) return;
    MsInitStatic(); MsEnsureBg(); MsEnsureEnv(); MsEnsureTarget(w, h);
    if (!g_mp.rtv) return;
    // camera
    float cy = cosf(g_mp.yaw), sy = sinf(g_mp.yaw), cp = cosf(g_mp.pitch), sp = sinf(g_mp.pitch);
    float eye[3] = { g_mp.tx + g_mp.dist * cp * sy, g_mp.ty + g_mp.dist * sp, g_mp.tz - g_mp.dist * cp * cy };
    float f[3] = { g_mp.tx - eye[0], g_mp.ty - eye[1], g_mp.tz - eye[2] }; float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]); for (auto& x : f) x /= fl;
    float r[3] = { f[2], 0, -f[0] }; float rl = sqrtf(r[0] * r[0] + r[2] * r[2]) + 1e-9f; r[0] /= rl; r[2] /= rl;
    float u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    float view[16] = { r[0], r[1], r[2], -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]),
                       u[0], u[1], u[2], -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                       f[0], f[1], f[2], -(f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]),
                       0, 0, 0, 1 };
    float fov = 0.75f, asp = (float)w / h, zn = 0.05f, zf = 100.f, ys = 1 / tanf(fov / 2), xs = ys / asp;
    float proj[16] = { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), -zn * zf / (zf - zn), 0, 0, 1, 0 };
    struct Frame { float vp[16], world[16], inv[16], cam[4], light[4], lightCol[4], misc[4], props[4], view[4]; } fr{};
    MsMul(proj, view, fr.vp); MsInvert(fr.vp, fr.inv);
    float a = g_mp.spin ? g_mp.spinAngle : 0, ca = cosf(a), sa = sinf(a);
    float world[16] = { ca, 0, sa, 0, 0, 1, 0, 0, -sa, 0, ca, 0, 0, 0, 0, 1 }; memcpy(fr.world, world, sizeof world);
    fr.cam[0] = eye[0]; fr.cam[1] = eye[1]; fr.cam[2] = eye[2];
    float lc = cosf(g_mp.lightPitch);
    fr.light[0] = lc * sinf(g_mp.lightYaw); fr.light[1] = sinf(g_mp.lightPitch); fr.light[2] = -lc * cosf(g_mp.lightYaw); fr.light[3] = g_mp.lightInt;
    fr.lightCol[0] = 1; fr.lightCol[1] = 0.97f; fr.lightCol[2] = 0.92f;
    float time = (GetTickCount64() - g_mp.start) / 1000.f;
    fr.misc[0] = g_mp.realtime ? time : 0; fr.misc[1] = g_mp.envIntensity; fr.misc[2] = g_mp.envTurn; fr.misc[3] = (float)g_mp.envMips;
    fr.props[0] = M.props.clip; fr.props[1] = M.props.dispScale; fr.props[2] = M.props.wpoScale; fr.props[3] = M.props.twoSided ? 1.f : 0.f;
    fr.view[0] = g_mp.exposure; fr.view[1] = g_mp.showBg ? 1.f : 0.f;
    g_ctx->UpdateSubresource(g_mp.cbFrame, 0, nullptr, &fr, 0, 0);
    std::vector<float> P; MsFillParams(P);
    g_ctx->UpdateSubresource(g_mp.cbParams, 0, nullptr, P.data(), 0, 0);
    // draw
    float clear[4] = { 0.04f, 0.04f, 0.045f, 1 };
    g_ctx->OMSetRenderTargets(1, &g_mp.rtv, g_mp.dsv);
    g_ctx->ClearRenderTargetView(g_mp.rtv, clear);
    g_ctx->ClearDepthStencilView(g_mp.dsv, D3D11_CLEAR_DEPTH, 1, 0);
    D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 }; g_ctx->RSSetViewports(1, &vp);
    ID3D11Buffer* cbs[2] = { g_mp.cbFrame, g_mp.cbParams };
    g_ctx->VSSetConstantBuffers(0, 2, cbs); g_ctx->PSSetConstantBuffers(0, 2, cbs);
    g_ctx->PSSetShaderResources(15, 1, &g_mp.envSrv); g_ctx->PSSetSamplers(15, 1, &g_mp.envSmp);
    g_ctx->VSSetShaderResources(15, 1, &g_mp.envSrv); g_ctx->VSSetSamplers(15, 1, &g_mp.envSmp);
    if (g_mp.bgVs && g_mp.bgPs) {
        g_ctx->IASetInputLayout(nullptr); g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_mp.bgVs, nullptr, 0); g_ctx->PSSetShader(g_mp.bgPs, nullptr, 0);
        g_ctx->RSSetState(g_mp.rsNone); g_ctx->OMSetDepthStencilState(g_mp.dsOff, 0);
        float bf[4] = { 0, 0, 0, 0 }; g_ctx->OMSetBlendState(g_mp.bsOpaque, bf, 0xffffffff);
        g_ctx->Draw(3, 0);
    }
    MsMesh& mesh = g_mp.mesh[g_mp.shape];
    if (g_ms.ok && mesh.count) {
        ID3D11ShaderResourceView* srvs[MS_MAX_TEX] = {}; ID3D11SamplerState* ss[MS_MAX_TEX] = {};
        for (size_t t = 0; t < g_ms.src.tex.size() && t < (size_t)MS_MAX_TEX; t++) {
            const MsTexSlot& ts = g_ms.src.tex[t];
            MeNode* n = MeFind(ts.node);
            std::string val = n ? MeParamValue(*n) : ts.value;
            ID3D11ShaderResourceView* s = MsTexture(val, ts.sampler == 0);
            srvs[t] = s ? s : (ts.sampler == 1 ? g_msFlat : g_msGrey);
            ss[t] = g_mp.smp[std::clamp(n ? n->filter : ts.filter, 0, 2)];
        }
        g_ctx->PSSetShaderResources(0, MS_MAX_TEX, srvs); g_ctx->PSSetSamplers(0, MS_MAX_TEX, ss);
        g_ctx->VSSetShaderResources(0, MS_MAX_TEX, srvs); g_ctx->VSSetSamplers(0, MS_MAX_TEX, ss);
        UINT stride = sizeof(MsVert), off = 0;
        g_ctx->IASetInputLayout(g_ms.il); g_ctx->IASetVertexBuffers(0, 1, &mesh.vb, &stride, &off); g_ctx->IASetIndexBuffer(mesh.ib, DXGI_FORMAT_R32_UINT, 0);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_ms.vs, nullptr, 0); g_ctx->PSSetShader(g_ms.ps, nullptr, 0);
        bool translucent = g_ms.src.props.blend == BLEND_TRANSLUCENT || g_ms.src.props.blend == BLEND_ADDITIVE;
        g_ctx->RSSetState(M.props.twoSided || g_mp.shape == MS_PLANE ? g_mp.rsNone : g_mp.rsBack);
        g_ctx->OMSetDepthStencilState(translucent ? g_mp.dsRead : g_mp.dsWrite, 0);
        float bf[4] = { 0, 0, 0, 0 };
        g_ctx->OMSetBlendState(g_ms.src.props.blend == BLEND_ADDITIVE ? g_mp.bsAdd : translucent ? g_mp.bsAlpha : g_mp.bsOpaque, bf, 0xffffffff);
        g_ctx->DrawIndexed(mesh.count, 0, 0);
        ID3D11ShaderResourceView* none[MS_MAX_TEX] = {};
        g_ctx->PSSetShaderResources(0, MS_MAX_TEX, none); g_ctx->VSSetShaderResources(0, MS_MAX_TEX, none);
    }
    g_ctx->ResolveSubresource(g_mp.rt, 0, g_mp.rtMs, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    ID3D11RenderTargetView* nullRtv = nullptr; g_ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
}
// does the preview need drawing again and again (things that move over time, a spinning mesh)?
static bool MsAnimated() {
    if (g_mp.spin) return true;
    if (!g_mp.realtime) return false;
    if (g_ms.src.hlsl.find("c.time") == std::string::npos) return false;
    // (the helper functions mention c.time too: look only at the generated part)
    size_t at = g_ms.src.hlsl.find("Mat Eval(Ctx c) {");
    return at != std::string::npos && g_ms.src.hlsl.find("c.time", at) != std::string::npos && g_ms.src.hlsl.find("c.time", at) < g_ms.src.hlsl.find("return m;", at);
}

// ---- the Viewport panel: the preview, with the mouse to turn, move and zoom it
static bool g_msOpenCustom = false;
static std::mutex g_msDeferMx; static std::vector<std::function<void()>> g_msDefer;     // finished background work, run on the editor's thread
static void MsViewportPanel() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float barH = ImGui::GetFrameHeight() + Px(4);
    // viewport toolbar: mesh, environment, settings
    ImGui::SetNextItemWidth(Px(120));
    if (ImGui::BeginCombo("##shape", g_msShapeName[g_mp.shape])) {
        for (int s = 0; s < MS_SHAPES; s++) {
            std::string l = s == MS_CUSTOM ? (g_mp.customName.empty() ? std::string("Custom mesh...") : "Custom: " + g_mp.customName) : g_msShapeName[s];
            if (ImGui::Selectable(l.c_str(), g_mp.shape == s)) {
                if (s == MS_CUSTOM) g_msOpenCustom = true; else g_mp.shape = s;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the material is shown on");
    ImGui::SameLine();
    // (the custom mesh list: imported models; or any model file, imported first)
    if (g_msOpenCustom) { ImGui::OpenPopup("##custommesh"); g_msOpenCustom = false; }
    {
        std::vector<std::function<void()>> q;
        { std::lock_guard<std::mutex> lk(g_msDeferMx); q.swap(g_msDefer); }
        for (auto& f : q) f();
    }
    ImGui::SetNextItemWidth(Px(130));
    if (ImGui::BeginCombo("##env", g_msEnvName[g_mp.env])) {
        for (int e = 0; e < ENV_COUNT; e++) if (ImGui::Selectable(g_msEnvName[e], g_mp.env == e)) {
            if (e == ENV_HDRI) OvBrowse(L"HDR images (*.hdr)\0*.hdr\0", L"Pick an HDRI (an equirectangular .hdr)", ImpStartDir(), false, L"", nullptr, [](std::wstring f) { if (!f.empty()) { g_mp.hdri = f; g_mp.env = ENV_HDRI; } });
            else g_mp.env = e;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lighting environment (the sky the material reflects)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_SLIDERS "##vps")) ImGui::OpenPopup("##vpset");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview settings");
    ImGui::SameLine();
    if (ImGui::Button(ICON_ROTATE "##vreset")) { g_mp.yaw = 0.6f; g_mp.pitch = 0.25f; g_mp.dist = 3.2f; g_mp.tx = g_mp.ty = g_mp.tz = 0; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset the camera");
    if (ImGui::BeginPopup("##vpset")) {
        ImGui::SetNextItemWidth(Px(180)); ImGui::SliderFloat("Exposure", &g_mp.exposure, 0.1f, 4.f, "%.2f");
        ImGui::SetNextItemWidth(Px(180)); ImGui::SliderFloat("Sky brightness", &g_mp.envIntensity, 0.f, 4.f, "%.2f");
        ImGui::SetNextItemWidth(Px(180)); ImGui::SliderFloat("Sky turn", &g_mp.envTurn, 0.f, 1.f, "%.2f");
        ImGui::SetNextItemWidth(Px(180)); ImGui::SliderFloat("Light", &g_mp.lightInt, 0.f, 10.f, "%.2f");
        ImGui::Checkbox("Show the sky behind", &g_mp.showBg);
        ImGui::Checkbox("Real-time (animate Time, Panner, Rotator)", &g_mp.realtime);
        ImGui::Checkbox("Spin the mesh", &g_mp.spin);
        if (g_mp.env == ENV_HDRI) { ImGui::Separator(); ImGui::TextUnformatted(g_mp.hdri.empty() ? "(no HDRI picked)" : W2U(g_mp.hdri).c_str()); if (ImGui::Button("Pick another HDRI...")) OvBrowse(L"HDR images (*.hdr)\0*.hdr\0", L"Pick an HDRI", ImpStartDir(), false, L"", nullptr, [](std::wstring f) { if (!f.empty()) g_mp.hdri = f; }); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##custommesh")) {
        EdLabel("Show the material on one of your imported models");
        for (auto& m : g_imp) if (ImGui::Selectable((m.name + "##" + W2U(m.file)).c_str())) {
            std::string err; if (MsLoadUemodel(m.file, err)) { g_mp.shape = MS_CUSTOM; g_mp.customName = m.name; g_mp.customFile = m.file; } else g_toasts.push_back({ err, true, GetTickCount64() });
        }
        if (g_imp.empty()) EdLabel("(no imported models yet)");
        ImGui::Separator();
        if (ImGui::Selectable(ICON_FILE_IMPORT "  Import a model file (.fbx, .obj, .gltf, .psk)...")) {
            OvBrowse(L"3D models (*.fbx, *.psk, *.gltf, *.glb, *.obj)\0*.fbx;*.psk;*.pskx;*.gltf;*.glb;*.obj\0", L"A model to show the material on", ImpStartDir(), false, L"", nullptr, [](std::wstring f) {
                if (f.empty()) return;
                std::thread([f] {
                    std::wstring out; std::string msg;
                    bool ok = ModelImport(f, out, msg);
                    std::lock_guard<std::mutex> lk(g_msDeferMx); g_framesLeft = 3;
                    g_msDefer.push_back([ok, out, msg] {
                        if (!ok) { g_toasts.push_back({ msg, true, GetTickCount64() }); return; }
                        std::string err; if (MsLoadUemodel(out, err)) { g_mp.shape = MS_CUSTOM; g_mp.customFile = out; g_mp.customName = W2U(out.substr(out.find_last_of(L"\\/") + 1)); g_impLoaded = false; }
                        else g_toasts.push_back({ err, true, GetTickCount64() });
                    });
                }).detach();
            });
        }
        ImGui::EndPopup();
    }
    // the picture
    ImVec2 sz(avail.x, std::max(Px(60), avail.y - barH));
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    int pw = (int)sz.x, ph = (int)sz.y;
    MsUpdateShader();
    MsRender(pw, ph);
    ImGui::InvisibleButton("##vp", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_mp.srv) dl->AddImage((ImTextureID)g_mp.srv, p0, ImVec2(p0.x + sz.x, p0.y + sz.y));
    ImGuiIO& io = ImGui::GetIO();
    bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
    if (act && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        if (ImGui::IsKeyDown(ImGuiKey_L) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {          // L + drag: the light (like Unreal)
            g_mp.lightYaw += io.MouseDelta.x * 0.01f; g_mp.lightPitch = std::clamp(g_mp.lightPitch - io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
        } else if (io.KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {                     // Shift + drag: turn the sky
            g_mp.envTurn = fmodf(g_mp.envTurn + io.MouseDelta.x * 0.002f + 1.f, 1.f);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {                                    // drag: orbit
            g_mp.yaw += io.MouseDelta.x * 0.01f; g_mp.pitch = std::clamp(g_mp.pitch + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
        } else {                                                                                    // right / middle drag: pan
            float k = g_mp.dist * 0.0015f, cy = cosf(g_mp.yaw), sy = sinf(g_mp.yaw);
            g_mp.tx -= io.MouseDelta.x * k * cy; g_mp.tz -= io.MouseDelta.x * k * sy; g_mp.ty += io.MouseDelta.y * k;
        }
    }
    if (hov && io.MouseWheel != 0) g_mp.dist = std::clamp(g_mp.dist * (io.MouseWheel > 0 ? 0.9f : 1.1f), 0.6f, 20.f);
    if (g_mp.spin) g_mp.spinAngle += io.DeltaTime * 0.6f;
    // what's going on, over the picture
    ImGui::PushFont(F_small);
    float ty = p0.y + Px(6);
    auto note = [&](const std::string& t, ImU32 c) { dl->AddText(ImVec2(p0.x + Px(8), ty), c, t.c_str()); ty += ImGui::GetTextLineHeight() + Px(2); };
    if (!g_ms.ok && !g_msBusy) note(g_ms.src.errs.empty() && g_ms.log.empty() ? "Compiling..." : "The shader didn't compile: see Stats", IM_COL32(240, 120, 110, 255));
    if (g_msBusy) note("Compiling...", IM_COL32(200, 200, 120, 255));
    if (MsLoadingTextures()) note("Loading textures...", IM_COL32(180, 180, 190, 255));
    if (!g_mp.envErr.empty() && g_mp.env == ENV_HDRI) note(g_mp.envErr, IM_COL32(240, 160, 110, 255));
    bool anyStd = false; for (int k = 0; k < STD_N; k++) if (M.links.count(M.stdStart + k)) anyStd = true;
    if (!anyStd && !M.slots.empty()) note("Nothing is wired into the Unreal inputs yet: the preview shows the defaults", IM_COL32(170, 170, 178, 255));
    dl->AddText(ImVec2(p0.x + Px(8), p0.y + sz.y - ImGui::GetTextLineHeight() - Px(4)), IM_COL32(150, 150, 158, 200), "Drag: orbit  \xC2\xB7  right-drag: pan  \xC2\xB7  wheel: zoom  \xC2\xB7  L+drag: light  \xC2\xB7  Shift+drag: sky");
    ImGui::PopFont();
    if (MsAnimated() || g_msBusy || MsLoadingTextures() || act) g_framesLeft = std::max(g_framesLeft, 2);
}
