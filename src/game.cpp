#include "game.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "xr.h"
#include "planes.h"

#include <windows.h>
#include <cstring>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------
// Luau (GIANTS Engine 10 embeds Luau). Layout verified against the engine's own lua_pushnumber:
//   lua_State: +0x08 top, +0x10 base; TValue is 16 bytes, type tag at +0x0C.
struct lua_State;
using lua_CFunction = int (*)(lua_State*);
using PFN_pushcclosurek = void (*)(lua_State*, lua_CFunction, const char*, int, void*);
using PFN_setfield = void (*)(lua_State*, int, const char*);
using PFN_createtable = void (*)(lua_State*, int, int);
using PFN_pushstring = void (*)(lua_State*, const char*);

constexpr int LUA_TNIL = 0, LUA_TBOOLEAN = 1, LUA_TNUMBER = 3;

PFN_pushcclosurek lua_pushcclosurek = nullptr;
PFN_setfield      lua_setfield = nullptr;
PFN_createtable   lua_createtable = nullptr;
PFN_pushstring    lua_pushstring = nullptr;

struct TValue {
    union { double n; int b; void* p; } value;
    int extra;
    int tt;
};

TValue*& Top(lua_State* L) { return *(TValue**)((char*)L + 0x08); }
TValue*  Base(lua_State* L) { return *(TValue**)((char*)L + 0x10); }
int      GetTop(lua_State* L) { return (int)(Top(L) - Base(L)); }

void PushNumber(lua_State* L, double n)
{
    TValue* t = Top(L);
    t->value.n = n;
    t->extra = 0;
    t->tt = LUA_TNUMBER;
    Top(L) = t + 1;
}

void PushBool(lua_State* L, bool b)
{
    TValue* t = Top(L);
    t->value.p = nullptr;
    t->value.b = b ? 1 : 0;
    t->extra = 0;
    t->tt = LUA_TBOOLEAN;
    Top(L) = t + 1;
}

bool ArgBool(lua_State* L, int idx)
{
    if (idx > GetTop(L)) return false;
    const TValue& v = Base(L)[idx - 1];
    if (v.tt == LUA_TNIL) return false;
    if (v.tt == LUA_TBOOLEAN) return v.value.b != 0;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Lua-visible VR API

// vr.getView() -> ok, eye, px, py, pz, qx, qy, qz, qw, fovY, offX, offY, frame, second
int L_getView(lua_State* L)
{
    EyeView v;
    if (!vr::GetView(v)) {
        PushBool(L, false);
        return 1;
    }
    PushBool(L, true);
    PushNumber(L, v.eye);
    for (float f : v.pos) PushNumber(L, f);
    for (float f : v.quat) PushNumber(L, f);
    PushNumber(L, v.fovY);
    PushNumber(L, v.offX);
    PushNumber(L, v.offY);
    PushNumber(L, (double)v.frame);
    PushBool(L, v.second);
    return 14;
}

// vr.prepareOverlay(on, quad, renderScale) -> width, height, ...: the sizes to create the render overlays for plane
// stereo with (one pair per overlay: the right eye; with quad views also the left and right focus
// views; 0, 0 = off); the bridge watches for the render targets of those sizes
int L_prepareOverlay(lua_State* L)
{
    uint32_t w[3] = {}, h[3] = {};
    float scale = GetTop(L) >= 3 && Base(L)[2].tt == LUA_TNUMBER ? (float)Base(L)[2].value.n : 1.0f;
    int n = vr::PrepareOverlay(ArgBool(L, 1), ArgBool(L, 2), scale > 0.1f ? scale : 1.0f, w, h);
    for (int i = 0; i < (n ? n : 1); i++) {
        PushNumber(L, w[i]);
        PushNumber(L, h[i]);
    }
    return 2 * (n ? n : 1);
}

// vr.setPlaneStereo(on) -> ok: the render overlay just queued with updateRenderOverlay becomes the
// output of a second engine view (the right eye through the main render path)
int L_setPlaneStereo(lua_State* L)
{
    PushBool(L, vr::SetPlaneStereo(ArgBool(L, 1)));
    return 1;
}

// vr.headsetInfo() -> recommended width, height per eye, focus view share of the field of view (w, h)
int L_headsetInfo(lua_State* L)
{
    uint32_t w = 0, h = 0;
    float fw = 0, fh = 0;
    vr::HeadsetInfo(w, h, fw, fh);
    PushNumber(L, w);
    PushNumber(L, h);
    PushNumber(L, fw);
    PushNumber(L, fh);
    return 4;
}

int L_isRunning(lua_State* L)
{
    PushBool(L, vr::IsRunning());
    return 1;
}

int L_calibrating(lua_State* L)
{
    PushBool(L, vr::IsCalibrating());
    return 1;
}

int L_setSymmetric(lua_State* L)
{
    vr::SetSymmetricFrustum(ArgBool(L, 1));
    return 0;
}

int L_recenter(lua_State* L)
{
    vr::RequestRecenter();
    return 0;
}

int L_status(lua_State* L)
{
    if (lua_pushstring) {
        lua_pushstring(L, vr::Status());
        return 1;
    }
    return 0;
}

// Replaces the engine's empty setStereoRendering binding.
// setStereoRendering(true) returns the VR API table; any other call is a no-op as before.
int Hook_setStereoRendering(lua_State* L)
{
    if (!ArgBool(L, 1)) return 0;
    static const struct { const char* name; lua_CFunction fn; } funcs[] = {
        {"getView", L_getView},
        {"isRunning", L_isRunning},
        {"recenter", L_recenter},
        {"calibrating", L_calibrating},
        {"setSymmetric", L_setSymmetric},
        {"status", L_status},
        {"prepareOverlay", L_prepareOverlay},
        {"setPlaneStereo", L_setPlaneStereo},
        {"headsetInfo", L_headsetInfo},
    };
    lua_createtable(L, 0, (int)std::size(funcs) + 1);
    for (auto& f : funcs) {
        lua_pushcclosurek(L, f.fn, f.name, 0, nullptr);
        lua_setfield(L, -2, f.name);
    }
    PushNumber(L, 1);  // API version
    lua_setfield(L, -2, "version");
    Log("Lua bridge handed to script (L=%p)", L);
    return 1;
}

int Hook_isHeadTrackingAvailable(lua_State* L)
{
    PushBool(L, vr::IsRunning());
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Binary scanning

struct Section { BYTE* start; size_t size; bool code; };
std::vector<Section> g_sections;
Section g_text{};

bool g_drmWrapper = false;  // exe carries Steam's DRM wrapper (".bind" section)

bool FindSections()
{
    if (g_text.start) return true;
    auto base = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        Section s{base + sec->VirtualAddress, sec->Misc.VirtualSize,
                  (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0};
        g_sections.push_back(s);
        if (memcmp(sec->Name, ".text", 5) == 0) g_text = s;
        if (memcmp(sec->Name, ".bind", 5) == 0) g_drmWrapper = true;
    }
    return g_text.start != nullptr;
}

bool InText(const BYTE* p) { return p >= g_text.start && p < g_text.start + g_text.size; }

BYTE* FindString(const char* str)
{
    size_t n = strlen(str) + 1;  // include the terminator
    for (auto& s : g_sections) {
        if (s.code || s.size < n) continue;
        for (BYTE* p = s.start + 1; p + n <= s.start + s.size; p++)
            if (*p == (BYTE)str[0] && p[-1] == 0 && memcmp(p, str, n) == 0) return p;
    }
    return nullptr;
}

// "AB ?? CD" style pattern search in .text.
BYTE* FindPattern(const char* pattern)
{
    std::vector<int> bytes;
    for (const char* c = pattern; *c;) {
        if (*c == ' ') { c++; continue; }
        if (*c == '?') { bytes.push_back(-1); c += (c[1] == '?') ? 2 : 1; continue; }
        bytes.push_back((int)strtoul(c, nullptr, 16));
        c += 2;
    }
    size_t n = bytes.size();
    BYTE* found = nullptr;
    for (BYTE* p = g_text.start; p + n <= g_text.start + g_text.size; p++) {
        size_t i = 0;
        while (i < n && (bytes[i] < 0 || p[i] == bytes[i])) i++;
        if (i == n) {
            if (found) return nullptr;  // ambiguous
            found = p;
        }
    }
    return found;
}

BYTE* Rel32(BYTE* insn, int dispOffset, int insnLen)
{
    return insn + insnLen + *(int32_t*)(insn + dispOffset);
}

// The engine registers every script binding with the same sequence:
//   lea REG, [wrapper]   ; mov rdx, REG ; ... ; lea r8, [name] ; call lua_pushcclosurek
//   mov rcx, [rbx+10h] ; lea r8, [name] ; mov edx, LUA_GLOBALSINDEX ; call lua_setfield
struct Binding { BYTE* wrapper = nullptr; BYTE* pushcclosurek = nullptr; BYTE* setfield = nullptr; };

Binding FindBinding(const char* name, bool diagnose)
{
    Binding b;
    BYTE* str = FindString(name);
    if (!str) {
        if (diagnose) Log("binding '%s': name string not found", name);
        return b;
    }
    for (BYTE* p = g_text.start; p + 24 < g_text.start + g_text.size; p++) {
        if (!(p[0] == 0x4C && p[1] == 0x8D && p[2] == 0x05)) continue;  // lea r8, [rip+disp32]
        if (Rel32(p, 3, 7) != str) continue;
        // the call to lua_pushcclosurek follows, allowing a few register moves in between
        BYTE* call = nullptr;
        for (BYTE* q = p + 7; q < p + 20; q++)
            if (*q == 0xE8 && InText(Rel32(q, 1, 5))) { call = q; break; }
        if (!call) continue;
        BYTE* closure = Rel32(call, 1, 5);
        // closest preceding rip-relative lea into .text = the wrapper function
        for (BYTE* q = p - 7; q > p - 40; q--) {
            if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8D && (q[2] & 0xC7) == 0x05) {
                BYTE* t = Rel32(q, 3, 7);
                if (InText(t)) { b.wrapper = t; break; }
            }
        }
        for (BYTE* q = p + 12; q < p + 40; q++) {
            if (q[0] == 0xBA && *(int32_t*)(q + 1) == -10002 && q[5] == 0xE8) {
                b.setfield = Rel32(q + 5, 1, 5);
                break;
            }
        }
        if (b.wrapper && b.setfield) {
            b.pushcclosurek = closure;
            return b;
        }
        b = Binding{};
    }
    if (!diagnose) return b;
    Log("binding '%s': registration site not found; code around each use of the name:", name);
    // Diagnostics for unsupported game builds: every rip-relative lea that references the name,
    // with the surrounding instruction bytes, so support can be added from a log file.
    int uses = 0;
    for (BYTE* p = g_text.start + 48; p + 64 < g_text.start + g_text.size && uses < 6; p++) {
        if (!((p[0] & 0xFB) == 0x48 && p[1] == 0x8D && (p[2] & 0xC7) == 0x05)) continue;
        if (Rel32(p, 3, 7) != str) continue;
        uses++;
        char hex[3 * 96 + 1] = {};
        for (int i = 0; i < 96; i++) sprintf(hex + i * 3, "%02X ", p[i - 40]);
        Log("  use at exe+%llx (lea at byte 40): %s",
            (unsigned long long)(p - (BYTE*)GetModuleHandleW(nullptr)), hex);
    }
    if (!uses) Log("  (no code references the name)");
    return b;
}

// Identifies the game build in the log (file version, link timestamp, image size).
void LogGameBuild()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    char version[64] = "unknown";
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path, &dummy);
    if (size) {
        std::vector<BYTE> buf(size);
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(path, 0, size, buf.data()) && VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) && fi)
            snprintf(version, sizeof(version), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                     HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    }
    auto base = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    Log("game build: file version %s, link stamp %08lx, image size %lu", version,
        (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage);
}

} // namespace

uint8_t* GameFindPattern(const char* pattern)
{
    return FindSections() ? FindPattern(pattern) : nullptr;
}

bool InstallGamePatches(bool final)
{
    static bool done = false, logged = false;
    if (done) return true;
    if (!logged) {
        logged = true;
        LogGameBuild();
    }
    if (!FindSections()) {
        Log("game: .text not found");
        return false;
    }
    // Steam's DRM wrapper keeps the game code encrypted until the game itself starts running,
    // so at load time there is nothing to find yet: try again once the game has started.
    if (!final && (g_drmWrapper || g_config.deferPatches)) {
        Log("game: exe uses Steam's DRM wrapper; engine patches are applied once the game has started");
        return false;
    }

    Binding stereo = FindBinding("setStereoRendering", final);
    Binding headTracking = FindBinding("isHeadTrackingAvailable", final);
    if (!stereo.wrapper || !headTracking.wrapper) {
        if (!final) Log("game: engine functions not found yet; trying again once the game has started");
        else Log("game patches failed: the Lua mod will report VR as unavailable");
        return false;
    }
    if (stereo.pushcclosurek != headTracking.pushcclosurek || stereo.setfield != headTracking.setfield) {
        Log("game: registration sites disagree, refusing to patch");
        return false;
    }
    lua_pushcclosurek = (PFN_pushcclosurek)stereo.pushcclosurek;
    lua_setfield = (PFN_setfield)stereo.setfield;
    lua_createtable = (PFN_createtable)FindPattern(
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 4C 8B 49 18 41 8B F0 8B EA");
    lua_pushstring = (PFN_pushstring)FindPattern(
        "48 85 D2 75 0D 48 8B 41 08 89 50 0C 48 83 41 08 10 C3 49 C7 C0 FF FF FF FF");

    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("game: pushcclosurek=+%llx setfield=+%llx createtable=+%llx pushstring=+%llx",
        (unsigned long long)((BYTE*)lua_pushcclosurek - base), (unsigned long long)((BYTE*)lua_setfield - base),
        (unsigned long long)((BYTE*)lua_createtable - base), (unsigned long long)((BYTE*)lua_pushstring - base));
    Log("game: setStereoRendering wrapper=+%llx isHeadTrackingAvailable wrapper=+%llx",
        (unsigned long long)(stereo.wrapper - base), (unsigned long long)(headTracking.wrapper - base));
    if (!lua_createtable) {
        Log("game: lua_createtable not found");
        return false;
    }

    bool ok = WriteJump(stereo.wrapper, (void*)Hook_setStereoRendering) &&
              WriteJump(headTracking.wrapper, (void*)Hook_isHeadTrackingAvailable);
    Log("game: binding patches %s", ok ? "installed" : "FAILED");

    planes::Install();
    done = ok;
    return ok;
}
