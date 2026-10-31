#include "config.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <string>

Config g_config;

static float ReadFloat(const wchar_t* file, const wchar_t* key, float def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(L"vr", key, L"", buf, 64, file);
    return buf[0] ? (float)_wtof(buf) : def;
}

static int ReadInt(const wchar_t* file, const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(L"vr", key, def, file);
}

void LoadConfig()
{
    std::wstring path = ModuleDir() + L"fs25vr.ini";
    const wchar_t* f = path.c_str();
    g_config.enabled      = ReadInt(f, L"enabled", 1) != 0;
    g_config.forceNoVsync = ReadInt(f, L"forceNoVsync", 1) != 0;
    g_config.presentLag   = ReadInt(f, L"presentLag", 0);
    g_config.worldScale   = ReadFloat(f, L"worldScale", 1.0f);
    g_config.menuDistance = ReadFloat(f, L"menuDistance", 2.0f);
    g_config.menuWidth    = ReadFloat(f, L"menuWidth", 2.6f);
    g_config.fitWindow    = ReadInt(f, L"fitWindow", 1) != 0;
    g_config.syncEyes     = ReadInt(f, L"syncEyes", 1) != 0;
    g_config.syncPhase    = ReadInt(f, L"syncPhase", 0);
    g_config.eyeOrder     = ReadInt(f, L"eyeOrder", 0) & 1;
    g_config.symmetricFrustum = ReadInt(f, L"symmetricFrustum", 1) != 0;
    g_config.clipMouse    = ReadInt(f, L"clipMouse", 1) != 0;
    g_config.showCursor   = ReadInt(f, L"showCursor", 1) != 0;
    g_config.asyncSubmit  = ReadInt(f, L"asyncSubmit", 1) != 0;
    g_config.hudArea      = std::clamp(ReadFloat(f, L"hudArea", 0.45f), 0.0f, 1.0f);
    g_config.hudDepth     = ReadFloat(f, L"hudDepth", 0.0f);
    if (g_config.hudDepth != 0.0f) g_config.hudDepth = std::clamp(g_config.hudDepth, 0.3f, 100.0f);
    g_config.hudLower     = std::clamp(ReadFloat(f, L"hudLower", 0.08f), -0.3f, 0.3f);
    g_config.hudEye       = std::clamp(ReadInt(f, L"hudEye", 0), 0, 2);
    g_config.depthLayer   = ReadInt(f, L"depthLayer", 1) != 0;
    g_config.vrMode       = std::clamp(ReadInt(f, L"vrMode", 0), 0, 2);
    g_config.renderWidth  = std::max(0, ReadInt(f, L"renderWidth", 0));
    g_config.renderHeight = std::max(0, ReadInt(f, L"renderHeight", 0));
    g_config.foveated     = ReadInt(f, L"foveated", 1) != 0;
    g_config.foveaInner   = std::clamp(ReadFloat(f, L"foveaInner", 0.40f), 0.1f, 2.0f);
    g_config.foveaOuter   = std::clamp(ReadFloat(f, L"foveaOuter", 0.65f), g_config.foveaInner, 3.0f);
    g_config.profile      = ReadInt(f, L"profile", 0) != 0;
    g_config.forceRender  = ReadInt(f, L"forceRender", 0) != 0;
    g_config.deferPatches = ReadInt(f, L"deferPatches", 0) != 0;
    g_config.debugLog     = ReadInt(f, L"debugLog", 0) != 0;
    if (g_config.worldScale <= 0.01f) g_config.worldScale = 1.0f;

    Log("config: enabled=%d forceNoVsync=%d presentLag=%d worldScale=%.2f menu=%.1fm/%.1fm fitWindow=%d syncEyes=%d/%d eyeOrder=%d symmetric=%d async=%d debug=%d",
        g_config.enabled, g_config.forceNoVsync, g_config.presentLag, g_config.worldScale,
        g_config.menuDistance, g_config.menuWidth, g_config.fitWindow, g_config.syncEyes, g_config.syncPhase,
        g_config.eyeOrder, g_config.symmetricFrustum, g_config.asyncSubmit, g_config.debugLog);
}

