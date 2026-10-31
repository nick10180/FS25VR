#include <windows.h>
#include "config.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "mode.h"
#include "window.h"
#include "sync.h"
#include "vrs.h"

static void Startup(HMODULE self)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir = path;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    LogInit(dir);
    Log("fs25vr " FS25VR_VERSION);
    LoadConfig();
    if (!g_config.enabled) {
        Log("disabled in fs25vr.ini; acting as a plain dinput8 proxy");
        return;  // flat play: game.xml and the window are left to the game
    }
    mode::Install();  // VR or flat: decided when the game opens game.xml, before it reads it
    InstallGamePatches(false);  // retried once the game runs (see hooks.cpp LateGameInit)
    if (!InstallDxgiHooks()) Log("dxgi hooks failed: no VR output");
    vrs::HookDeviceEarly();
    InstallWindowHooks();
    InstallEyeSync(false);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Startup(module);
    }
    return TRUE;
}
