#include "hud.h"
#include "log.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace hud {
namespace {

// vtable slots (d3d12.h declaration order)
constexpr int kObjSetPrivateData = 4;
constexpr int kObjSetName = 6;
constexpr int kClResourceBarrier = 26;

const GUID kNameA = {0x429b8c22, 0x9188, 0x4b0c, {0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00}};
const GUID kNameW = {0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}};
const char kHudName[] = "textureOverlayTexture";

using PFN_SetPrivateData = HRESULT(STDMETHODCALLTYPE*)(ID3D12Object*, REFGUID, UINT, const void*);
using PFN_SetName = HRESULT(STDMETHODCALLTYPE*)(ID3D12Object*, LPCWSTR);
using PFN_Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);

PFN_SetPrivateData o_SetPrivateData = nullptr;
PFN_SetName        o_SetName = nullptr;
PFN_Barrier        o_Barrier = nullptr;

// The engine creates the target anew now and then (transient memory): the newest few named ones.
// Checked lock-free on every barrier of the game.
constexpr int kTracked = 4;
std::atomic<ID3D12Resource*> g_targets[kTracked];
std::atomic<int>             g_nextTarget{0};

std::atomic<bool>     g_active{false};
std::atomic<uint64_t> g_captures{0};
uint64_t              g_seen = 0;

std::mutex            g_mtx;  // the copy and its views
ID3D12Device*         g_device = nullptr;
ID3D12Resource*       g_copy = nullptr;
std::vector<std::pair<ID3D12Resource*, uint64_t>> g_retired;  // old copies, released a few captures later
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
bool                  g_installed = false;

bool Patch(void** slot, void* value, void** original)
{
    if (*slot == value) return true;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (original && !*original) *original = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

bool IsTarget(ID3D12Resource* r)
{
    if (!r) return false;
    for (auto& t : g_targets)
        if (t.load(std::memory_order_relaxed) == r) return true;
    return false;
}

void Named(ID3D12Object* obj, bool isHud)
{
    ID3D12Resource* r = (ID3D12Resource*)obj;
    if (isHud) {
        if (IsTarget(r)) return;
        g_targets[g_nextTarget++ % kTracked] = r;
        D3D12_RESOURCE_DESC d = r->GetDesc();
        Log("hud: game HUD target %p %llux%u fmt %d", r, d.Width, d.Height, d.Format);
    } else {
        for (auto& t : g_targets) {  // the address now names something else
            ID3D12Resource* expected = r;
            t.compare_exchange_strong(expected, nullptr);
        }
    }
}

HRESULT STDMETHODCALLTYPE Hook_SetPrivateData(ID3D12Object* self, REFGUID g, UINT size, const void* data)
{
    HRESULT hr = o_SetPrivateData(self, g, size, data);
    if (data && size && IsEqualGUID(g, kNameA)) {
        Named(self, strnlen((const char*)data, size) == sizeof(kHudName) - 1 &&
                        !memcmp(data, kHudName, sizeof(kHudName) - 1));
    } else if (data && size && IsEqualGUID(g, kNameW)) {
        const wchar_t* w = (const wchar_t*)data;
        UINT n = 0;
        while (n < size / 2 && w[n]) n++;
        bool same = n == sizeof(kHudName) - 1;
        for (UINT i = 0; same && i < n; i++) same = w[i] == (wchar_t)kHudName[i];
        Named(self, same);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_SetName(ID3D12Object* self, LPCWSTR name)
{
    HRESULT hr = o_SetName(self, name);
    if (name) {
        size_t n = wcslen(name);
        bool same = n == sizeof(kHudName) - 1;
        for (size_t i = 0; same && i < n; i++) same = name[i] == (wchar_t)kHudName[i];
        Named(self, same);
    }
    return hr;
}

// Copies the game's HUD target (in RENDER_TARGET) into g_copy and clears it, on the game's list.
void Capture(ID3D12GraphicsCommandList* cl, ID3D12Resource* src)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    D3D12_RESOURCE_DESC sd = src->GetDesc();
    if (g_copy) {
        D3D12_RESOURCE_DESC cd = g_copy->GetDesc();
        if (cd.Width != sd.Width || cd.Height != sd.Height || cd.Format != sd.Format) {
            g_retired.push_back({g_copy, g_captures.load() + 8});
            g_copy = nullptr;
        }
    }
    if (!g_copy) {
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd = sd;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        rd.Alignment = 0;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&g_copy)))) {
            g_copy = nullptr;
            Log("hud: copy texture %llux%u fmt %d could not be created", sd.Width, sd.Height, sd.Format);
            return;
        }
        Log("hud: copy texture %llux%u fmt %d", sd.Width, sd.Height, sd.Format);
    }
    while (!g_retired.empty() && g_retired.front().second <= g_captures.load()) {
        g_retired.front().first->Release();
        g_retired.erase(g_retired.begin());
    }

    D3D12_RESOURCE_BARRIER b[2] = {};
    for (auto& x : b) {
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    b[0].Transition.pResource = src;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = g_copy;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    o_Barrier(cl, 2, b);
    cl->CopyResource(g_copy, src);
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    o_Barrier(cl, 2, b);

    // Empty, the target holds alpha 1: its alpha is how much of the scene shows through (colours
    // premultiplied). Cleared to that, the game's final pass leaves the eyes' images without the HUD.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_device->CreateRenderTargetView(src, nullptr, rtv);
    const float empty[4] = {0, 0, 0, 1};
    cl->ClearRenderTargetView(rtv, empty, 0, nullptr);
    g_captures++;
}

void STDMETHODCALLTYPE Hook_Barrier(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RESOURCE_BARRIER* b)
{
    if (g_active.load(std::memory_order_relaxed) && b) {
        for (UINT i = 0; i < n; i++) {
            const D3D12_RESOURCE_BARRIER& x = b[i];
            if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && x.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                x.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET &&
                (x.Transition.StateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
                IsTarget(x.Transition.pResource)) {
                Capture(self, x.Transition.pResource);
                break;
            }
        }
    }
    o_Barrier(self, n, b);
}

} // namespace

void Install(ID3D12Device* device)
{
    if (g_installed || !device) return;
    g_installed = true;
    g_device = device;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)))) {
        Log("hud: RTV heap could not be created");
        return;
    }

    // a resource and a command list of our own give the vtables shared by all of the game's
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource* res = nullptr;
    if (SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&res)))) {
        void** rvt = *(void***)res;
        Patch(&rvt[kObjSetPrivateData], (void*)Hook_SetPrivateData, (void**)&o_SetPrivateData);
        Patch(&rvt[kObjSetName], (void*)Hook_SetName, (void**)&o_SetName);
        res->Release();
    }
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)))) {
        void** cvt = *(void***)cl;
        Patch(&cvt[kClResourceBarrier], (void*)Hook_Barrier, (void**)&o_Barrier);
        cl->Close();
    }
    if (cl) cl->Release();
    if (alloc) alloc->Release();
    Log("hud: hooks installed (names %d/%d, ResourceBarrier %d)", o_SetPrivateData != nullptr, o_SetName != nullptr,
        o_Barrier != nullptr);
}

void SetActive(bool on)
{
    if (g_active.exchange(on) != on) Log("hud: %s", on ? "taken out of the game's image" : "left to the game");
}

ID3D12Resource* FrameImage()
{
    uint64_t n = g_captures.load();
    bool fresh = n != g_seen;
    g_seen = n;
    std::lock_guard<std::mutex> lock(g_mtx);
    return fresh ? g_copy : nullptr;
}

} // namespace hud
