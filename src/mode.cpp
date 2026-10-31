// VR or flat play, decided when the game first opens its game.xml (before it reads it).
//
// The render size for the headset has to be in game.xml before the game reads it: resolution and
// window mode only change with a restart. So at that moment:
//   * headset ready: the player's display settings are kept aside (game.xml.fs25vr-flat) and the
//     VR render size (renderWidth x renderHeight, windowed, vsync off) is written;
//   * no headset: if game.xml still holds the VR size from the last VR session, the player's
//     display settings are put back, and the bridge stays idle (no OpenXR, SteamVR is not started).
// "Headset ready": the active OpenXR runtime reports a head-mounted display. SteamVR is only asked
// while it is running (asking would start it).
//
// The VR render size is renderWidth x renderHeight from fs25vr.ini, or (0, the default) the size the
// bridge measured for the headset in an earlier VR session (fs25vr_size.ini next to this DLL). The
// first VR session therefore runs at the game's own size and asks for one restart.

#include "mode.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <openxr/openxr.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>

namespace mode {
namespace {

std::once_flag g_decideOnce;
bool           g_vr = true;
std::atomic<bool> g_gameXmlHandled{false};
std::wstring   g_gameXml;  // the profile's game.xml (lower case), found at start-up

// VR render size: from fs25vr.ini, or measured in an earlier session (g_auto). g_applied* is the
// size last written into game.xml, so a changed measurement still counts as "the VR display".
int  g_width = 0, g_height = 0, g_appliedWidth = 0, g_appliedHeight = 0;
bool g_auto = false;
std::atomic<bool> g_restartNote{false};

std::wstring SizeFile() { return ModuleDir() + L"fs25vr_size.ini"; }

int ReadSize(const wchar_t* key)
{
    return std::max(0, (int)GetPrivateProfileIntW(L"size", key, 0, SizeFile().c_str()));
}

void WriteSize(const wchar_t* key, int value)
{
    WritePrivateProfileStringW(L"size", key, std::to_wstring(value).c_str(), SizeFile().c_str());
}

// ---------------------------------------------------------------------------------------------
// files

bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring EnvPath(const wchar_t* name)
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf) : std::wstring();
}

// Lower case, backslashes, without a \\?\ prefix: for comparing paths.
std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    if (s.rfind(L"\\\\?\\", 0) == 0) s.erase(0, 4);
    return s;
}

// The game's profile folder holds game.xml: the Xbox app / Microsoft Store build keeps it in its
// package folder, Steam and the GIANTS build in Documents\My Games (same order as install.ps1).
std::wstring FindGameXml()
{
    std::wstring local = EnvPath(L"LOCALAPPDATA");
    using PFN_PackageName = LONG(WINAPI*)(UINT32*, PWSTR);
    auto packageName = (PFN_PackageName)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentPackageFamilyName");
    wchar_t family[256];
    UINT32 len = 256;
    if (packageName && !local.empty() && packageName(&len, family) == ERROR_SUCCESS) {
        std::wstring p = local + L"\\Packages\\" + family + L"\\LocalCache\\Local\\game.xml";
        if (FileExists(p)) return p;
    }
    wchar_t docs[MAX_PATH];
    DWORD size = sizeof(docs);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                     L"Personal", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, docs, &size) == ERROR_SUCCESS) {
        std::wstring p = std::wstring(docs) + L"\\My Games\\FarmingSimulator2025\\game.xml";
        if (FileExists(p)) return p;
    }
    if (!local.empty()) {
        std::wstring p = local + L"\\GIANTSSoftware.FarmingSimulator25PC\\game.xml";
        if (FileExists(p)) return p;
    }
    return std::wstring();
}

bool ReadFileStr(const std::wstring& path, std::string& out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    out.assign(GetFileSize(f, nullptr), '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(f);
    return ok && read == out.size();
}

// Writes via a temporary file so a failure never leaves a half-written file behind.
bool WriteFileStr(const std::wstring& path, const std::string& data)
{
    std::wstring tmp = path + L".fs25vr-tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// game.xml <display> values

struct Display { std::string width, height, mode, vsync; };
const char* kTags[4] = {"width", "height", "fullscreenMode", "vsync"};

std::string* Field(Display& d, int i) { return i == 0 ? &d.width : i == 1 ? &d.height : i == 2 ? &d.mode : &d.vsync; }

// The text of <tag ...>text</tag> inside <section>; optionally replaced by 'value'.
bool TagText(std::string& xml, const char* tag, std::string& text, const std::string* value,
             const char* section = "display")
{
    size_t from = xml.find(std::string("<") + section + ">"), to = xml.find(std::string("</") + section + ">");
    if (from == std::string::npos || to == std::string::npos) return false;
    std::string open = std::string("<") + tag, close = std::string("</") + tag + ">";
    size_t a = xml.find(open, from);
    if (a == std::string::npos || a >= to) return false;
    size_t b = xml.find('>', a);
    size_t c = b == std::string::npos ? b : xml.find(close, b);
    if (c == std::string::npos || c > to) return false;
    text = xml.substr(b + 1, c - b - 1);
    if (value) xml.replace(b + 1, c - b - 1, *value);
    return true;
}

bool GetDisplay(std::string& xml, Display& d)
{
    bool ok = true;
    for (int i = 0; i < 4; i++) ok &= TagText(xml, kTags[i], *Field(d, i), nullptr);
    return ok;
}

void SetDisplay(std::string& xml, const Display& d)
{
    std::string old;
    for (int i = 0; i < 4; i++) {
        Display copy = d;
        TagText(xml, kTags[i], old, Field(copy, i));
    }
}

bool IsVrDisplay(const Display& d)
{
    if (d.mode != "windowed") return false;
    auto is = [&](int w, int h) { return w > 0 && d.width == std::to_string(w) && d.height == std::to_string(h); };
    return is(g_width, g_height) || is(g_appliedWidth, g_appliedHeight);
}

// The player's own display settings: from the last flat session, or from the backup install.ps1
// made before VR was first set up.
bool LoadFlatDisplay(const std::wstring& gameXml, Display& d)
{
    std::string s;
    if (ReadFileStr(gameXml + L".fs25vr-flat", s) && GetDisplay(s, d) && !IsVrDisplay(d)) return true;
    if (ReadFileStr(gameXml + L".fs25vr-backup", s) && GetDisplay(s, d) && !IsVrDisplay(d)) return true;
    return false;
}

void PrepareGameXml(const std::wstring& path, bool vr)
{
    if (g_width <= 0 && g_appliedWidth <= 0) {
        // nothing measured yet: the first VR session runs at the game's size and measures it
        if (vr) Log("display: render size for this headset not known yet; it is measured in this session");
        return;
    }
    std::string xml;
    Display cur;
    if (!ReadFileStr(path, xml) || !GetDisplay(xml, cur)) {
        Log("display: could not read the <display> settings in game.xml");
        return;
    }
    if (vr) {
        if (g_width <= 0) return;
        if (!IsVrDisplay(cur)) {
            // keep the player's settings for the next flat start (a copy of game.xml: the same format)
            if (!WriteFileStr(path + L".fs25vr-flat", xml)) Log("display: could not keep the flat display settings");
        }
        Display want{std::to_string(g_width), std::to_string(g_height), "windowed", "false"};
        // the bridge works with Direct3D 12 only
        std::string renderer, d3d12 = "D3D_12";
        bool rendererOk = !TagText(xml, "renderer", renderer, nullptr, "graphic") || renderer == d3d12;
        if (cur.width == want.width && cur.height == want.height && cur.mode == want.mode && cur.vsync == want.vsync &&
            rendererOk) {
            Log("display: VR render size %sx%s already set%s", want.width.c_str(), want.height.c_str(),
                g_auto ? " (measured for this headset)" : "");
            return;
        }
        SetDisplay(xml, want);
        if (!rendererOk) TagText(xml, "renderer", renderer, &d3d12, "graphic");
        if (WriteFileStr(path, xml)) {
            g_appliedWidth = g_width, g_appliedHeight = g_height;
            WriteSize(L"appliedWidth", g_width);
            WriteSize(L"appliedHeight", g_height);
            Log("display: VR render size %sx%s windowed, vsync off%s%s (was %sx%s %s)", want.width.c_str(),
                want.height.c_str(), rendererOk ? "" : ", Direct3D 12", g_auto ? ", measured for this headset" : "",
                cur.width.c_str(), cur.height.c_str(), cur.mode.c_str());
        }
        return;
    }
    if (!IsVrDisplay(cur)) {
        Log("display: flat play with the game's own settings (%sx%s %s)", cur.width.c_str(), cur.height.c_str(),
            cur.mode.c_str());
        return;
    }
    Display flat;
    if (!LoadFlatDisplay(path, flat)) {
        flat = {"0", "0", "windowed_fullscreen", "true"};  // the game's defaults: the whole screen
    }
    SetDisplay(xml, flat);
    if (WriteFileStr(path, xml))
        Log("display: flat play, display settings back to %sx%s %s", flat.width.c_str(), flat.height.c_str(),
            flat.mode.c_str());
}

// ---------------------------------------------------------------------------------------------
// headset detection

bool ProcessRunning(const wchar_t* exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, exe) == 0;
    CloseHandle(snap);
    return found;
}

std::wstring ActiveRuntime()
{
    std::wstring env = EnvPath(L"XR_RUNTIME_JSON");
    if (!env.empty()) return env;
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr, buf,
                     &size) == ERROR_SUCCESS)
        return buf;
    return std::wstring();
}

// Asks the runtime for a head-mounted display (no session, nothing is shown).
DWORD WINAPI ProbeThread(void* result)
{
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "fs25vr headset check");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XrInstance inst = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateInstance(&ci, &inst))) return 0;
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    if (XR_SUCCEEDED(xrGetSystem(inst, &sgi, &sys))) *(LONG*)result = 1;
    xrDestroyInstance(inst);
    return 0;
}

bool HeadsetReady(const char*& why)
{
    std::wstring runtime = Lower(ActiveRuntime());
    if (runtime.empty()) {
        why = "no OpenXR runtime is set up";
        return false;
    }
    if (runtime.find(L"steamvr") != std::wstring::npos || runtime.find(L"steamxr") != std::wstring::npos) {
        if (!ProcessRunning(L"vrserver.exe")) {
            why = "SteamVR is not running";
            return false;
        }
    }
    static LONG result = 0;
    HANDLE t = CreateThread(nullptr, 0, ProbeThread, &result, 0, nullptr);
    if (!t) {
        why = "could not ask the runtime";
        return true;
    }
    DWORD w = WaitForSingleObject(t, 8000);
    CloseHandle(t);
    if (w != WAIT_OBJECT_0) {
        why = "the runtime did not answer in time (assuming the headset is coming)";
        return true;
    }
    why = result ? "the runtime reports a headset" : "the runtime reports no headset";
    return result != 0;
}

void Decide()
{
    if (g_config.vrMode == 1) {
        g_vr = true;
        Log("mode: VR (vrMode=1)");
    } else if (g_config.vrMode == 2) {
        g_vr = false;
        Log("mode: flat (vrMode=2)");
    } else {
        const char* why = "";
        g_vr = HeadsetReady(why);
        Log("mode: %s - %s", g_vr ? "VR" : "flat play, the bridge stays idle", why);
    }
}

// The first time the game opens its profile's game.xml: decide, then prepare the file.
void OnFileOpen(const std::wstring& path)
{
    if (g_gameXmlHandled.load(std::memory_order_relaxed) || g_gameXml.empty()) return;
    if (Lower(path) != g_gameXml) return;
    if (g_gameXmlHandled.exchange(true)) return;
    std::call_once(g_decideOnce, Decide);
    PrepareGameXml(path, g_vr);
}

void OnFileOpenA(const char* path)
{
    if (!path || g_gameXmlHandled.load(std::memory_order_relaxed)) return;
    size_t n = strlen(path);
    if (n < 8 || _stricmp(path + n - 8, "game.xml") != 0) return;
    int len = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    std::wstring w(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_ACP, 0, path, -1, w.data(), len);
    OnFileOpen(w);
}

// ---------------------------------------------------------------------------------------------
// file open hooks (the game's import table; which one it uses depends on how it was built)

using PFN_CreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PFN_CreateFile2 = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using PFN_wfopen = FILE*(__cdecl*)(const wchar_t*, const wchar_t*);
using PFN_wfopen_s = errno_t(__cdecl*)(FILE**, const wchar_t*, const wchar_t*);
using PFN_fopen = FILE*(__cdecl*)(const char*, const char*);
using PFN_fopen_s = errno_t(__cdecl*)(FILE**, const char*, const char*);

PFN_CreateFileW o_CreateFileW = nullptr;
PFN_CreateFile2 o_CreateFile2 = nullptr;
PFN_wfopen      o_wfopen = nullptr;
PFN_wfopen_s    o_wfopen_s = nullptr;
PFN_fopen       o_fopen = nullptr;
PFN_fopen_s     o_fopen_s = nullptr;

HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                               DWORD flags, HANDLE tmpl)
{
    if (name && (access & GENERIC_READ)) OnFileOpen(name);
    return o_CreateFileW(name, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI Hook_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disp,
                               LPCREATEFILE2_EXTENDED_PARAMETERS p)
{
    if (name && (access & GENERIC_READ)) OnFileOpen(name);
    return o_CreateFile2(name, access, share, disp, p);
}

FILE* __cdecl Hook_wfopen(const wchar_t* name, const wchar_t* m)
{
    if (name && m && wcschr(m, L'r')) OnFileOpen(name);
    return o_wfopen(name, m);
}

errno_t __cdecl Hook_wfopen_s(FILE** f, const wchar_t* name, const wchar_t* m)
{
    if (name && m && wcschr(m, L'r')) OnFileOpen(name);
    return o_wfopen_s(f, name, m);
}

FILE* __cdecl Hook_fopen(const char* name, const char* m)
{
    if (m && strchr(m, 'r')) OnFileOpenA(name);
    return o_fopen(name, m);
}

errno_t __cdecl Hook_fopen_s(FILE** f, const char* name, const char* m)
{
    if (m && strchr(m, 'r')) OnFileOpenA(name);
    return o_fopen_s(f, name, m);
}

// Replaces an entry in the game's import address table (any of the given DLL names).
bool PatchIat(const char* const* dlls, const char* func, void* hook, void** original)
{
    auto base = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool done = false;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        const char* name = (char*)(base + imp->Name);
        bool match = false;
        for (const char* const* d = dlls; *d; d++) match |= _stricmp(name, *d) == 0;
        if (!match || !imp->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, func) != 0) continue;
            void** slot = (void**)&addrs->u1.Function;
            if (*slot == hook) return true;
            DWORD old;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) continue;
            if (!*original) *original = *slot;  // stored before the slot is switched
            *slot = hook;
            VirtualProtect(slot, sizeof(void*), old, &old);
            done = true;
        }
    }
    return done;
}

} // namespace

void Install()
{
    g_appliedWidth = ReadSize(L"appliedWidth");
    g_appliedHeight = ReadSize(L"appliedHeight");
    if (g_config.renderWidth > 0 && g_config.renderHeight > 0) {
        g_width = g_config.renderWidth, g_height = g_config.renderHeight;
        Log("display: VR render size %dx%d (fs25vr.ini)", g_width, g_height);
    } else {
        g_auto = true;
        g_width = ReadSize(L"width"), g_height = ReadSize(L"height");
        if (g_width <= 0 || g_height <= 0) g_width = g_height = 0;
        if (g_width) Log("display: VR render size %dx%d (measured for this headset)", g_width, g_height);
    }
    g_gameXml = Lower(FindGameXml());
    static const char* const kKernel[] = {"kernel32.dll", "api-ms-win-core-file-l1-1-0.dll",
                                          "api-ms-win-core-file-l1-2-0.dll", "api-ms-win-core-file-l2-1-0.dll", nullptr};
    static const char* const kCrt[] = {"ucrtbase.dll", "api-ms-win-crt-stdio-l1-1-0.dll", "msvcrt.dll", nullptr};
    int hooked = 0;
    hooked += PatchIat(kKernel, "CreateFileW", (void*)Hook_CreateFileW, (void**)&o_CreateFileW);
    hooked += PatchIat(kKernel, "CreateFile2", (void*)Hook_CreateFile2, (void**)&o_CreateFile2);
    hooked += PatchIat(kCrt, "_wfopen", (void*)Hook_wfopen, (void**)&o_wfopen);
    hooked += PatchIat(kCrt, "_wfopen_s", (void*)Hook_wfopen_s, (void**)&o_wfopen_s);
    hooked += PatchIat(kCrt, "fopen", (void*)Hook_fopen, (void**)&o_fopen);
    hooked += PatchIat(kCrt, "fopen_s", (void*)Hook_fopen_s, (void**)&o_fopen_s);
    Log("mode: watching for game.xml (%d file functions hooked, profile %s)", hooked,
        g_gameXml.empty() ? "not found" : "found");
}

bool VrWanted()
{
    // normally decided when the game opened game.xml; otherwise now (the display is as it is)
    std::call_once(g_decideOnce, [] {
        if (!g_gameXmlHandled) Log("mode: the game was not seen opening game.xml; deciding now");
        Decide();
    });
    return g_vr;
}

void SaveMeasuredSize(unsigned width, unsigned height, unsigned currentWidth, unsigned currentHeight)
{
    if (!g_auto || !width || !height) return;
    // more than 10% fewer pixels than the headset needs in either direction: worth a restart
    g_restartNote = currentWidth * 10 < width * 9 || currentHeight * 10 < height * 9;
    if ((int)width == g_width && (int)height == g_height) return;
    g_width = (int)width, g_height = (int)height;
    WriteSize(L"width", g_width);
    WriteSize(L"height", g_height);
    Log("display: render size for this headset %ux%u saved (fs25vr_size.ini), used from the next start", width,
        height);
}

const char* RenderSizeNote()
{
    return g_restartNote ? "VR: restart the game once for the full sharpness in the headset" : nullptr;
}

} // namespace mode
