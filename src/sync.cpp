// Eye synchronisation: both images of a stereo pair must show the same moment of the world.
//
// The engine runs each frame through one function, RunFrame(this, dt), and every simulated
// system (game logic, physics stepping, animation) takes its time step from that dt. On frames
// that render the second (right) eye we pass a near-zero dt, so the world holds still while the
// camera moves to the other eye; the held-back time is added to the next first-eye frame, so the
// game still runs at real-time speed. (Not exactly zero: game scripts divide by dt.)
#include "sync.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "profile.h"
#include "xr.h"

#include <windows.h>
#include <vector>

namespace {

using PFN_RunFrame = uintptr_t (*)(void* self, float dt);
PFN_RunFrame o_RunFrame = nullptr;

float  g_carry = 0.0f;
int    g_logged = 0;

uintptr_t Hook_RunFrame(void* self, float dt)
{
    if (g_logged < 5) {
        Log("eye sync: RunFrame dt=%f", dt);
        g_logged++;
    }
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    uintptr_t r;
    if (vr::NextFrameIsSecondEye()) {
        g_carry += dt;
        if (g_carry > 250.0f) g_carry = 250.0f;  // never release more than a quarter second at once
        r = o_RunFrame(self, dt * 0.001f);
    } else {
        float total = dt + g_carry;
        g_carry = 0.0f;
        r = o_RunFrame(self, total);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    prof::RunFrameTime((double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);
    return r;
}

BYTE* FindPattern(const char* pattern)
{
    auto base = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    BYTE* start = nullptr;
    size_t size = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (memcmp(sec->Name, ".text", 5) == 0) start = base + sec->VirtualAddress, size = sec->Misc.VirtualSize;
    std::vector<int> bytes;
    for (const char* c = pattern; *c;) {
        if (*c == ' ') { c++; continue; }
        if (*c == '?') { bytes.push_back(-1); c += (c[1] == '?') ? 2 : 1; continue; }
        bytes.push_back((int)strtoul(c, nullptr, 16));
        c += 2;
    }
    BYTE* found = nullptr;
    for (BYTE* p = start; p && p + bytes.size() <= start + size; p++) {
        size_t i = 0;
        while (i < bytes.size() && (bytes[i] < 0 || p[i] == bytes[i])) i++;
        if (i == bytes.size()) {
            if (found) return nullptr;  // ambiguous
            found = p;
        }
    }
    return found;
}

} // namespace

bool InstallEyeSync(bool final)
{
    static bool done = false, disabledLogged = false;
    if (done) return true;
    if (!final && g_config.deferPatches) return false;
    if (!g_config.syncEyes) {
        if (!disabledLogged) Log("eye sync: disabled in fs25vr.ini");
        disabledLogged = true;
        return false;
    }
    // RunFrame is found by its body, which has survived game updates: it keeps dt (xmm1) in xmm6,
    // keeps 'this' in rbx and first tests a flag at this+0x295. The prologue before it changes
    // with compiler versions, so it is matched against known forms that can be relocated.
    BYTE* anchor = FindPattern("0F 28 F1 48 8B D9 33 F6 40 38 B1 95 02 00 00 0F 84");
    if (!anchor) anchor = FindPattern("0F 28 F1 48 8B D9 33 F6 40 38 B1 ?? ?? 00 00 0F 84");  // flag moved
    if (!anchor) {
        if (final) Log("eye sync: RunFrame not found (game version changed?); eyes will not be synchronised");
        return false;
    }
    BYTE* fn = anchor;
    while (fn > anchor - 0x80 && !(fn[-1] == 0xCC && ((uintptr_t)fn & 15) == 0)) fn--;

    struct Prologue { const char* bytes; size_t len; };
    static const Prologue kKnown[] = {
        // mov rax,rsp; mov [rax+18h],rbx; mov [rax+20h],rsi; push rbp; push rdi; push r14  (v1.5)
        {"\x48\x8B\xC4\x48\x89\x58\x18\x48\x89\x70\x20\x55\x57\x41\x56", 15},
        // mov [rsp+18h],rbx; push rbp; push rsi; push rdi; push r14; push r15  (v1.20)
        {"\x48\x89\x5C\x24\x18\x55\x56\x57\x41\x56\x41\x57", 12},
        // mov [rsp+18h],rbx; push rbp; push rsi; push rdi; lea rbp,[rsp-160h]  (v1.24 Microsoft Store)
        {"\x48\x89\x5C\x24\x18\x55\x56\x57\x48\x8D\xAC\x24\xA0\xFE\xFF\xFF", 16},
    };
    size_t kPrologue = 0;
    for (const auto& p : kKnown)
        if (memcmp(fn, p.bytes, p.len) == 0) { kPrologue = p.len; break; }
    if (kPrologue == 0) {
        char hex[3 * 24 + 1] = {};
        for (int i = 0; i < 24; i++) sprintf(hex + i * 3, "%02X ", fn[i]);
        Log("eye sync: unknown RunFrame prologue (%s); eyes will not be synchronised", hex);
        return false;
    }
    // trampoline: the relocated prologue, then a jump back. Written while writable, then locked
    // to execute-only (never writable and executable at the same time).
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tramp) return false;
    memcpy(tramp, fn, kPrologue);
    BYTE jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0] ; dq target
    BYTE* back = fn + kPrologue;
    memcpy(jmp + 6, &back, 8);
    memcpy(tramp + kPrologue, jmp, sizeof(jmp));
    DWORD old;
    if (!VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    o_RunFrame = (PFN_RunFrame)tramp;
    bool ok = WriteJump(fn, (void*)Hook_RunFrame);
    Log("eye sync: RunFrame at exe+%llx hooked %s", (unsigned long long)(fn - (BYTE*)GetModuleHandleW(nullptr)),
        ok ? "ok" : "FAILED");
    done = ok;
    return ok;
}
