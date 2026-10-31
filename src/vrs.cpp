// Fixed foveated rendering (see vrs.h).
//
// The game's command lists are hooked through their vtables (found on the lists the game hands
// to ExecuteCommandLists). Per list we track whether a depth buffer is bound and whether the
// viewport covers the whole backbuffer; while both hold on a stereo frame, the eye's shading rate
// image is bound with a MAX combiner (the coarser of the game's own rate and ours wins). If the
// game binds a shading rate image of its own, that list is left alone.

#include "vrs.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vrs {
namespace {

// vtable slots (COM vtable layout is fixed by the interface definitions)
constexpr int kQueueExecuteCommandLists = 10;
constexpr int kReset = 10;                   // ID3D12GraphicsCommandList
constexpr int kRSSetViewports = 21;
constexpr int kOMSetRenderTargets = 46;
constexpr int kBeginRenderPass = 68;         // ID3D12GraphicsCommandList4
constexpr int kRSSetShadingRate = 77;        // ID3D12GraphicsCommandList5
constexpr int kRSSetShadingRateImage = 78;
constexpr int kClearDepthStencilView = 47;
constexpr int kDevCreateDepthStencilView = 21;  // ID3D12Device
constexpr int kDevCopyDescriptors = 23;
constexpr int kDevCopyDescriptorsSimple = 24;

using GCL = ID3D12GraphicsCommandList;
using PFN_ECL = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_Reset = HRESULT(STDMETHODCALLTYPE*)(GCL*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_RSSetViewports = void(STDMETHODCALLTYPE*)(GCL*, UINT, const D3D12_VIEWPORT*);
using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(GCL*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE*);
using PFN_BeginRenderPass = void(STDMETHODCALLTYPE*)(GCL*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                                     const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
                                                     D3D12_RENDER_PASS_FLAGS);
using PFN_RSSetShadingRate = void(STDMETHODCALLTYPE*)(GCL*, D3D12_SHADING_RATE, const D3D12_SHADING_RATE_COMBINER*);
using PFN_RSSetShadingRateImage = void(STDMETHODCALLTYPE*)(GCL*, ID3D12Resource*);
using PFN_ClearDepthStencilView = void(STDMETHODCALLTYPE*)(GCL*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT,
                                                           UINT8, UINT, const D3D12_RECT*);
using PFN_CreateDepthStencilView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
                                                            const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CopyDescriptors = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                                     const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                                     D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CopyDescriptorsSimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);

// Original functions of one command list vtable (several layers may each have their own).
struct Vt {
    void**                    vt = nullptr;
    PFN_Reset                 reset = nullptr;
    PFN_RSSetViewports        viewports = nullptr;
    PFN_OMSetRenderTargets    renderTargets = nullptr;
    PFN_BeginRenderPass       beginRenderPass = nullptr;
    PFN_RSSetShadingRate      shadingRate = nullptr;
    PFN_RSSetShadingRateImage shadingRateImage = nullptr;
    PFN_ClearDepthStencilView clearDepth = nullptr;
};
constexpr int kMaxVt = 8;
Vt               g_vts[kMaxVt];
std::atomic<int> g_vtCount{0};
std::mutex       g_vtMtx;

struct ListState {
    bool dsv = false;          // a depth buffer is bound
    bool fullViewport = false; // viewport 0 covers the backbuffer
    bool typeChecked = false, direct = false;
    bool gameImage = false;    // the game bound a shading rate image of its own
    bool gameCombiners = false;
    D3D12_SHADING_RATE gameRate = D3D12_SHADING_RATE_1X1;
    D3D12_SHADING_RATE_COMBINER gameComb[2] = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                               D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    ID3D12Resource* applied = nullptr;  // our image currently bound on this list
    SIZE_T dsvPtr = 0;         // the bound depth view
    bool captured = false;     // its depth was copied at the start of this pass
};
std::mutex                          g_mtx;  // list states, images, render size
std::unordered_map<void*, ListState> g_lists;

// A layer below us (a proxy list forwarding to the real one) calls back into hooked slots;
// those nested calls just pass through.
thread_local int t_in = 0;
struct Enter {
    Enter() { t_in++; }
    ~Enter() { t_in--; }
};

ID3D12Device*        g_device = nullptr;
ID3D12CommandQueue*  g_queue = nullptr;
PFN_ECL              o_ECL = nullptr;
bool                 g_supported = false;
UINT                 g_tile = 16;
D3D12_SHADING_RATE   g_coarse = D3D12_SHADING_RATE_4X4;
std::atomic<bool>    g_enabled{false};
std::atomic<int>     g_frameEye{-1};
std::atomic<bool>    g_presenting{false};
DWORD                g_presentThread = 0;

// images: 0 = left eye, 1 = right eye, 2 = both eyes (used when the eye is not known per frame)
ID3D12Resource*      g_img[3] = {};
UINT                 g_w = 0, g_h = 0;  // render size the images were built for
bool                 g_perEye = true;
bool                 g_overlap = false; // full-size passes were recorded while a frame was presented

struct EyeFov { float a[4], r[4]; bool valid = false; };
std::mutex           g_fovMtx;
EyeFov               g_fov[2];
unsigned             g_fovVersion = 0, g_builtVersion = ~0u;

// GPU upload of the images
ID3D12CommandAllocator*    g_alloc = nullptr;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence*               g_fence = nullptr;
UINT64                     g_fenceValue = 0;
HANDLE                     g_event = nullptr;
struct Retired { ID3D12Resource* res; UINT64 fence; uint64_t present; };
std::vector<Retired>       g_retired;
uint64_t                   g_presents = 0;

// statistics (10 s window)
std::atomic<uint64_t> g_statPasses{0}, g_statPassesInPresent{0}, g_statGameImage{0};
uint64_t              g_statFrames = 0;
ULONGLONG             g_statStart = 0;
bool                  g_loggedGameImage = false;

// Depth capture for the headset's depth layer. Depth views are mapped to their resources (the
// game binds views, not resources). At the start of each full-size 3D pass the bound depth buffer
// is known to be in DEPTH_WRITE (a writable view requires it), so a copy is recorded right there
// into the game's own command list; the last one of a frame holds the scene's depth.
struct DsvInfo { ID3D12Resource* res; bool writable; };
std::mutex                            g_dsvMtx;
std::unordered_map<SIZE_T, DsvInfo>   g_dsvs;
UINT                                  g_dsvInc = 0;
PFN_CreateDepthStencilView            o_CreateDsv = nullptr;
PFN_CopyDescriptors                   o_CopyDescriptors = nullptr;
PFN_CopyDescriptorsSimple             o_CopyDescriptorsSimple = nullptr;
std::atomic<bool>                     g_depthOn{false};
ID3D12Resource*                       g_cap[2] = {};          // per eye, rests in COPY_DEST (g_mtx)
uint64_t                              g_capSeq[2] = {~0ull, ~0ull};  // g_presents when last written
std::atomic<int>                      g_depthClear{-1};       // last clear value of the scene depth (0/1)
bool                                  g_loggedDepth = false;
std::atomic<uint64_t>                 g_statCaptures{0};
bool                                  g_dsvHooked = false;
bool                                  g_loggedDsvMiss = false;
std::mutex                            g_devHookMtx;

const Vt* FindVt(void* obj)
{
    void** vt = *(void***)obj;
    int n = g_vtCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_vts[i].vt == vt) return &g_vts[i];
    return nullptr;
}

bool IsR32Depth(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_R32_FLOAT;
}

// Records a copy of the bound scene depth into this eye's capture texture. g_mtx held.
void CaptureDepth(GCL* cl, ListState& s, int eye)
{
    s.captured = true;
    DsvInfo info{};
    {
        std::lock_guard<std::mutex> lock(g_dsvMtx);
        auto it = g_dsvs.find(s.dsvPtr);
        if (it == g_dsvs.end() || !it->second.writable) {
            if (!g_loggedDsvMiss) {
                g_loggedDsvMiss = true;
                Log("depth layer: the depth view of a 3D pass is %s (%zu depth views known)",
                    it == g_dsvs.end() ? "unknown" : "read-only or partial", g_dsvs.size());
            }
            return;
        }
        info = it->second;
    }
    D3D12_RESOURCE_DESC d = info.res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != g_w || d.Height != g_h ||
        d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1 || !IsR32Depth(d.Format)) {
        if (!g_loggedDepth) {
            g_loggedDepth = true;
            Log("depth layer: scene depth is %llux%u format %d (%u samples); only single-sample 32-bit depth "
                "at the render size is supported, depth layer off", d.Width, d.Height, (int)d.Format,
                d.SampleDesc.Count);
        }
        g_depthOn = false;
        return;
    }
    ID3D12Resource*& cap = g_cap[eye];
    if (cap) {
        D3D12_RESOURCE_DESC c = cap->GetDesc();
        if (c.Width != d.Width || c.Height != d.Height || c.Format != d.Format) {
            g_retired.push_back({cap, g_fenceValue, g_presents});
            cap = nullptr;
        }
    }
    if (!cap) {
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC cd = d;
        cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;  // depth may only be copied between depth resources
        cd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&cap)))) {
            cap = nullptr;
            Log("depth layer: could not create the capture texture; depth layer off");
            g_depthOn = false;
            return;
        }
        if (!g_loggedDepth) {
            g_loggedDepth = true;
            Log("depth layer: capturing scene depth %llux%u format %d", d.Width, d.Height, (int)d.Format);
        }
    }
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = info.res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;  // bound as a writable depth view
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    cl->CopyResource(cap, info.res);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
    g_capSeq[eye] = g_presents;
    g_statCaptures++;
}

// Binds or unbinds our image on a list according to its state, and captures the scene depth at
// the start of each full-size 3D pass. g_mtx held.
void Evaluate(GCL* cl, ListState& s, const Vt& v, bool newPass)
{
    int eye = g_frameEye.load(std::memory_order_relaxed);
    bool scenePass = eye >= 0 && s.dsv && s.fullViewport;
    if (scenePass && !s.typeChecked) {
        s.typeChecked = true;
        s.direct = cl->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;  // bundles cannot set shading rates
    }
    if (scenePass && s.direct && !s.captured && g_depthOn.load(std::memory_order_relaxed)) CaptureDepth(cl, s, eye);
    bool want = scenePass && g_enabled.load(std::memory_order_relaxed) && !s.gameImage;
    ID3D12Resource* img = (want && s.direct) ? g_img[g_perEye ? eye : 2] : nullptr;
    if (img) {
        if (s.applied != img) {
            D3D12_SHADING_RATE_COMBINER comb[2] = {
                s.gameCombiners ? s.gameComb[0] : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                D3D12_SHADING_RATE_COMBINER_MAX};
            v.shadingRate(cl, s.gameRate, comb);
            v.shadingRateImage(cl, img);
            s.applied = img;
        }
        if (newPass) {
            g_statPasses++;
            if (g_presenting.load(std::memory_order_relaxed) && GetCurrentThreadId() != g_presentThread)
                g_statPassesInPresent++;
        }
    } else if (s.applied) {
        v.shadingRateImage(cl, nullptr);
        v.shadingRate(cl, s.gameRate, s.gameCombiners ? s.gameComb : nullptr);
        s.applied = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE Hook_Reset(GCL* cl, ID3D12CommandAllocator* a, ID3D12PipelineState* p)
{
    const Vt* v = FindVt(cl);
    if (t_in) return v->reset(cl, a, p);
    Enter e;
    HRESULT hr = v->reset(cl, a, p);
    std::lock_guard<std::mutex> lock(g_mtx);
    g_lists[cl] = ListState{};  // Reset also resets the list's shading rate state
    return hr;
}

void STDMETHODCALLTYPE Hook_RSSetViewports(GCL* cl, UINT n, const D3D12_VIEWPORT* vp)
{
    const Vt* v = FindVt(cl);
    if (t_in) return v->viewports(cl, n, vp);
    Enter e;
    v->viewports(cl, n, vp);
    std::lock_guard<std::mutex> lock(g_mtx);
    ListState& s = g_lists[cl];
    s.fullViewport = n >= 1 && vp && g_w && fabsf(vp[0].TopLeftX) < 0.5f && fabsf(vp[0].TopLeftY) < 0.5f &&
                     fabsf(vp[0].Width - (float)g_w) < 0.5f && fabsf(vp[0].Height - (float)g_h) < 0.5f;
    Evaluate(cl, s, *v, false);
}

void STDMETHODCALLTYPE Hook_OMSetRenderTargets(GCL* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtv,
                                               BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    const Vt* v = FindVt(cl);
    if (t_in) return v->renderTargets(cl, n, rtv, single, dsv);
    Enter e;
    v->renderTargets(cl, n, rtv, single, dsv);
    std::lock_guard<std::mutex> lock(g_mtx);
    ListState& s = g_lists[cl];
    s.dsv = dsv != nullptr;
    s.dsvPtr = dsv ? dsv->ptr : 0;
    s.captured = false;
    Evaluate(cl, s, *v, true);
}

void STDMETHODCALLTYPE Hook_BeginRenderPass(GCL* cl, UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt,
                                            const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds,
                                            D3D12_RENDER_PASS_FLAGS flags)
{
    const Vt* v = FindVt(cl);
    if (t_in) return v->beginRenderPass(cl, n, rt, ds, flags);
    Enter e;
    {
        // shading rate state must be set before the pass begins
        std::lock_guard<std::mutex> lock(g_mtx);
        ListState& s = g_lists[cl];
        s.dsv = ds != nullptr;
        s.dsvPtr = ds ? ds->cpuDescriptor.ptr : 0;
        s.captured = false;
        Evaluate(cl, s, *v, true);
    }
    v->beginRenderPass(cl, n, rt, ds, flags);
}

void STDMETHODCALLTYPE Hook_RSSetShadingRate(GCL* cl, D3D12_SHADING_RATE rate, const D3D12_SHADING_RATE_COMBINER* comb)
{
    const Vt* v = FindVt(cl);
    if (t_in) return v->shadingRate(cl, rate, comb);
    Enter e;
    std::lock_guard<std::mutex> lock(g_mtx);
    ListState& s = g_lists[cl];
    s.gameRate = rate;
    s.gameCombiners = comb != nullptr;
    if (comb) s.gameComb[0] = comb[0], s.gameComb[1] = comb[1];
    if (s.applied) {  // keep our image combined in
        D3D12_SHADING_RATE_COMBINER c[2] = {comb ? comb[0] : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                            D3D12_SHADING_RATE_COMBINER_MAX};
        v->shadingRate(cl, rate, c);
    } else {
        v->shadingRate(cl, rate, comb);
    }
}

void STDMETHODCALLTYPE Hook_RSSetShadingRateImage(GCL* cl, ID3D12Resource* img)
{
    const Vt* v = FindVt(cl);
    if (t_in) return v->shadingRateImage(cl, img);
    Enter e;
    std::lock_guard<std::mutex> lock(g_mtx);
    ListState& s = g_lists[cl];
    s.gameImage = img != nullptr;
    if (img) {
        s.applied = nullptr;  // the game's image replaces ours
        g_statGameImage++;
    }
    v->shadingRateImage(cl, img);
    if (!img) Evaluate(cl, s, *v, false);
}

// The scene depth's clear value tells whether the engine uses reversed depth (cleared to 0).
void STDMETHODCALLTYPE Hook_ClearDepthStencilView(GCL* cl, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags,
                                                  FLOAT depth, UINT8 stencil, UINT n, const D3D12_RECT* rects)
{
    const Vt* v = FindVt(cl);
    v->clearDepth(cl, dsv, flags, depth, stencil, n, rects);
    if (t_in || !(flags & D3D12_CLEAR_FLAG_DEPTH) || !g_depthOn.load(std::memory_order_relaxed)) return;
    ID3D12Resource* res = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_dsvMtx);
        auto it = g_dsvs.find(dsv.ptr);
        if (it != g_dsvs.end()) res = it->second.res;
    }
    if (!res || n != 0) return;
    D3D12_RESOURCE_DESC d = res->GetDesc();
    UINT w, h;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        w = g_w, h = g_h;
    }
    if (d.Width == w && d.Height == h) g_depthClear = depth < 0.5f ? 0 : 1;
}

// Depth views -> resources. Views are written by CreateDepthStencilView and by descriptor copies.
void STDMETHODCALLTYPE Hook_CreateDepthStencilView(ID3D12Device* dev, ID3D12Resource* res,
                                                   const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateDsv(dev, res, desc, h);
    std::lock_guard<std::mutex> lock(g_dsvMtx);
    if (!res) {
        g_dsvs.erase(h.ptr);
        return;
    }
    // only whole, writable, plain 2D views: their resource is in DEPTH_WRITE while bound
    bool writable = !desc || (desc->Flags == D3D12_DSV_FLAG_NONE && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D &&
                              desc->Texture2D.MipSlice == 0);
    g_dsvs[h.ptr] = {res, writable};
}

void CopyDsvs(SIZE_T dst, SIZE_T src, UINT count)  // g_dsvMtx held
{
    for (UINT i = 0; i < count; i++) {
        auto it = g_dsvs.find(src + (SIZE_T)i * g_dsvInc);
        if (it != g_dsvs.end())
            g_dsvs[dst + (SIZE_T)i * g_dsvInc] = it->second;
        else
            g_dsvs.erase(dst + (SIZE_T)i * g_dsvInc);
    }
}

void STDMETHODCALLTYPE Hook_CopyDescriptorsSimple(ID3D12Device* dev, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    o_CopyDescriptorsSimple(dev, n, dst, src, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return;
    std::lock_guard<std::mutex> lock(g_dsvMtx);
    CopyDsvs(dst.ptr, src.ptr, n);
}

void STDMETHODCALLTYPE Hook_CopyDescriptors(ID3D12Device* dev, UINT nDst, const D3D12_CPU_DESCRIPTOR_HANDLE* dst,
                                            const UINT* dstSizes, UINT nSrc, const D3D12_CPU_DESCRIPTOR_HANDLE* src,
                                            const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    o_CopyDescriptors(dev, nDst, dst, dstSizes, nSrc, src, srcSizes, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return;
    // flatten both range lists into single descriptors, in order
    std::vector<SIZE_T> d, s;
    for (UINT i = 0; i < nDst; i++)
        for (UINT k = 0; k < (dstSizes ? dstSizes[i] : 1); k++) d.push_back(dst[i].ptr + (SIZE_T)k * g_dsvInc);
    for (UINT i = 0; i < nSrc; i++)
        for (UINT k = 0; k < (srcSizes ? srcSizes[i] : 1); k++) s.push_back(src[i].ptr + (SIZE_T)k * g_dsvInc);
    std::lock_guard<std::mutex> lock(g_dsvMtx);
    std::vector<std::pair<SIZE_T, DsvInfo>> copied;  // read all sources before writing (ranges may overlap)
    for (size_t i = 0; i < d.size() && i < s.size(); i++) {
        auto it = g_dsvs.find(s[i]);
        copied.push_back({d[i], it != g_dsvs.end() ? it->second : DsvInfo{nullptr, false}});
    }
    for (auto& c : copied) {
        if (c.second.res)
            g_dsvs[c.first] = c.second;
        else
            g_dsvs.erase(c.first);
    }
}

bool PatchSlot(void** slot, void* hook, void** original)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Hooks the vtable of a command list the game executes, the first time it is seen.
void HookList(ID3D12CommandList* list)
{
    if (FindVt(list)) return;
    std::lock_guard<std::mutex> lock(g_vtMtx);
    if (FindVt(list)) return;
    int n = g_vtCount.load();
    if (n >= kMaxVt) return;
    // our calls go straight through the vtable's slots, so the object must expose
    // ID3D12GraphicsCommandList5 through the same vtable (single inheritance)
    ID3D12GraphicsCommandList5* l5 = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&l5))) || !l5) return;
    bool same = (void*)l5 == (void*)list;
    l5->Release();
    void** vt = *(void***)list;
    Vt& v = g_vts[n];
    if (!same) {
        v.vt = vt;  // remember it so it is not checked again; no hooks
        g_vtCount.store(n + 1, std::memory_order_release);
        Log("foveated: a command list type without shading rate support is used; skipped");
        return;
    }
    v.vt = vt;
    v.reset = (PFN_Reset)vt[kReset];
    v.viewports = (PFN_RSSetViewports)vt[kRSSetViewports];
    v.renderTargets = (PFN_OMSetRenderTargets)vt[kOMSetRenderTargets];
    v.beginRenderPass = (PFN_BeginRenderPass)vt[kBeginRenderPass];
    v.shadingRate = (PFN_RSSetShadingRate)vt[kRSSetShadingRate];
    v.shadingRateImage = (PFN_RSSetShadingRateImage)vt[kRSSetShadingRateImage];
    v.clearDepth = (PFN_ClearDepthStencilView)vt[kClearDepthStencilView];
    g_vtCount.store(n + 1, std::memory_order_release);  // published before any hook can run
    void* dummy;
    PatchSlot(&vt[kReset], (void*)Hook_Reset, &dummy);
    PatchSlot(&vt[kRSSetViewports], (void*)Hook_RSSetViewports, &dummy);
    PatchSlot(&vt[kOMSetRenderTargets], (void*)Hook_OMSetRenderTargets, &dummy);
    PatchSlot(&vt[kBeginRenderPass], (void*)Hook_BeginRenderPass, &dummy);
    PatchSlot(&vt[kRSSetShadingRate], (void*)Hook_RSSetShadingRate, &dummy);
    PatchSlot(&vt[kRSSetShadingRateImage], (void*)Hook_RSSetShadingRateImage, &dummy);
    PatchSlot(&vt[kClearDepthStencilView], (void*)Hook_ClearDepthStencilView, &dummy);
    Log("foveated: command list vtable %p hooked", (void*)vt);
}

void STDMETHODCALLTYPE Hook_ECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (q == g_queue && g_vtCount.load(std::memory_order_relaxed) < kMaxVt) {
        for (UINT i = 0; i < n; i++)
            if (lists[i] && !FindVt(lists[i]) && lists[i]->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
                HookList(lists[i]);
    }
    o_ECL(q, n, lists);
}

// ---------------------------------------------------------------------------------------------
// Masks

D3D12_SHADING_RATE Coarser(D3D12_SHADING_RATE a, D3D12_SHADING_RATE b) { return a > b ? a : b; }  // 1X1 < 2X2 < 4X4

void BuildMask(const EyeFov& f, UINT w, UINT h, UINT tw, UINT th, std::vector<uint8_t>& out)
{
    const float inner = g_config.foveaInner, outer = g_config.foveaOuter;
    const float L = f.a[0], R = f.a[1], D = f.a[2], U = f.a[3];
    const float rl = f.r[0], rr = f.r[1], rd = f.r[2], ru = f.r[3];
    out.assign((size_t)tw * th, (uint8_t)D3D12_SHADING_RATE_1X1);
    for (UINT ty = 0; ty < th; ty++) {
        for (UINT tx = 0; tx < tw; tx++) {
            // tile extent in tangent space (image y runs downwards)
            float u0 = std::min(1.0f, (float)(tx * g_tile) / w), u1 = std::min(1.0f, (float)((tx + 1) * g_tile) / w);
            float v0 = std::min(1.0f, (float)(ty * g_tile) / h), v1 = std::min(1.0f, (float)((ty + 1) * g_tile) / h);
            float x0 = rl + u0 * (rr - rl), x1 = rl + u1 * (rr - rl);
            float y0 = ru - v1 * (ru - rd), y1 = ru - v0 * (ru - rd);
            D3D12_SHADING_RATE rate;
            if (x1 < L || x0 > R || y1 < D || y0 > U) {
                rate = g_coarse;  // the lens never shows this tile
            } else {
                // distance from the view axis in units of the field of view to each side,
                // measured at the tile point closest to the axis
                float x = std::clamp(0.0f, x0, x1), y = std::clamp(0.0f, y0, y1);
                float nx = x >= 0 ? x / std::max(R, 1e-3f) : x / std::max(-L, 1e-3f);
                float ny = y >= 0 ? y / std::max(U, 1e-3f) : y / std::max(-D, 1e-3f);
                float r = sqrtf(nx * nx + ny * ny);
                rate = r <= inner ? D3D12_SHADING_RATE_1X1 : r <= outer ? D3D12_SHADING_RATE_2X2 : g_coarse;
            }
            out[(size_t)ty * tw + tx] = (uint8_t)rate;
        }
    }
}

void WaitUpload()
{
    if (g_fence->GetCompletedValue() < g_fenceValue) {
        g_fence->SetEventOnCompletion(g_fenceValue, g_event);
        WaitForSingleObject(g_event, 1000);
    }
}

void ReleaseRetired(bool all)
{
    UINT64 done = g_fence->GetCompletedValue();
    for (size_t i = 0; i < g_retired.size();) {
        Retired& r = g_retired[i];
        if (all || (r.fence <= done && g_presents >= r.present + 3)) {  // lists recorded with it have run
            r.res->Release();
            g_retired[i] = g_retired.back();
            g_retired.pop_back();
        } else {
            i++;
        }
    }
}

// Creates the three images for the current size and field of view. Game thread.
bool Build(UINT w, UINT h)
{
    EyeFov fov[2];
    {
        std::lock_guard<std::mutex> lock(g_fovMtx);
        fov[0] = g_fov[0];
        fov[1] = g_fov[1];
        g_builtVersion = g_fovVersion;
    }
    if (!fov[0].valid && !fov[1].valid) return false;
    if (!fov[0].valid) fov[0] = fov[1];
    if (!fov[1].valid) fov[1] = fov[0];

    UINT tw = (w + g_tile - 1) / g_tile, th = (h + g_tile - 1) / g_tile;
    std::vector<uint8_t> mask[3];
    BuildMask(fov[0], w, h, tw, th, mask[0]);
    BuildMask(fov[1], w, h, tw, th, mask[1]);
    mask[2].resize(mask[0].size());
    for (size_t i = 0; i < mask[2].size(); i++)
        mask[2][i] = (uint8_t)std::min(mask[0][i], mask[1][i]);  // finer of the two: right for either eye

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = tw;
    td.Height = th;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 size = 0;
    g_device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &size);
    UINT64 stride = (size + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);

    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, up = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = stride * 3;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* img[3] = {};
    bool ok = SUCCEEDED(g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&upload)));
    for (int i = 0; ok && i < 3; i++)
        ok = SUCCEEDED(g_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&img[i])));
    BYTE* p = nullptr;
    if (ok) ok = SUCCEEDED(upload->Map(0, nullptr, (void**)&p));
    if (!ok) {
        Log("foveated: could not create the shading rate images; foveated rendering disabled");
        if (upload) upload->Release();
        for (auto* r : img)
            if (r) r->Release();
        g_supported = false;
        g_enabled = false;
        return false;
    }
    for (int i = 0; i < 3; i++)
        for (UINT y = 0; y < th; y++)
            memcpy(p + stride * i + (size_t)y * fp.Footprint.RowPitch, mask[i].data() + (size_t)y * tw, tw);
    upload->Unmap(0, nullptr);

    WaitUpload();
    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);
    D3D12_RESOURCE_BARRIER bar[3] = {};
    for (int i = 0; i < 3; i++) {
        D3D12_TEXTURE_COPY_LOCATION dst = {img[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = fp;
        src.PlacedFootprint.Offset = stride * i;
        g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        bar[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar[i].Transition.pResource = img[i];
        bar[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        bar[i].Transition.StateAfter = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    }
    g_list->ResourceBarrier(3, bar);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    o_ECL(g_queue, 1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);

    std::lock_guard<std::mutex> lock(g_mtx);
    g_retired.push_back({upload, g_fenceValue, g_presents});
    for (int i = 0; i < 3; i++) {
        if (g_img[i]) g_retired.push_back({g_img[i], g_fenceValue, g_presents});
        g_img[i] = img[i];
    }
    g_w = w, g_h = h;
    size_t full = 0, half = 0;
    for (uint8_t r : mask[0]) full += r == D3D12_SHADING_RATE_1X1, half += r == D3D12_SHADING_RATE_2X2;
    Log("foveated: shading rate images %ux%u (tile %u) for %ux%u: %.0f%% full rate, %.0f%% 2x2, %.0f%% %s",
        tw, th, g_tile, w, h, 100.0 * full / mask[0].size(), 100.0 * half / mask[0].size(),
        100.0 * (mask[0].size() - full - half) / mask[0].size(), g_coarse == D3D12_SHADING_RATE_4X4 ? "4x4" : "2x2");
    return true;
}

void LogStats()
{
    g_statFrames++;
    ULONGLONG now = GetTickCount64();
    if (!g_statStart) g_statStart = now;
    if (now - g_statStart < 10000) return;
    double frames = (double)std::max<uint64_t>(1, g_statFrames);
    uint64_t passes = g_statPasses.exchange(0), inPresent = g_statPassesInPresent.exchange(0);
    uint64_t gameImage = g_statGameImage.exchange(0);
    if (g_enabled)
        Log("  foveated rendering: %.1f full-size 3D passes per frame with the mask%s", passes / frames,
            g_perEye ? "" : " (shared mask for both eyes)");
    uint64_t captures = g_statCaptures.exchange(0);
    if (g_depthOn)
        Log("  depth layer: %.1f depth captures per frame, depth cleared to %d", captures / frames, (int)g_depthClear);
    if (gameImage && !g_loggedGameImage) {
        g_loggedGameImage = true;
        Log("foveated: the game binds shading rate images of its own; those passes keep the game's rates");
    }
    if (inPresent && !g_overlap) {
        // the eye of those passes is not known for sure: use the mask that is right for both eyes
        g_overlap = true;
        Log("foveated: %llu passes were recorded while a frame was presented; using one mask for both eyes",
            (unsigned long long)inPresent);
    }
    g_statFrames = 0;
    g_statStart = now;
}

// Depth views -> resources, for the depth layer. All D3D12 devices share one vtable, so hooking it
// once (with any device) covers the game's device too. Safe to call more than once.
void InstallDeviceHooks(ID3D12Device* device)
{
    std::lock_guard<std::mutex> lock(g_devHookMtx);
    if (g_dsvHooked || !device) return;
    g_dsvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    void** dvt = *(void***)device;
    // the originals are stored before each slot is switched (the game may call them right away)
    g_dsvHooked = PatchSlot(&dvt[kDevCreateDepthStencilView], (void*)Hook_CreateDepthStencilView, (void**)&o_CreateDsv) &&
                  PatchSlot(&dvt[kDevCopyDescriptors], (void*)Hook_CopyDescriptors, (void**)&o_CopyDescriptors) &&
                  PatchSlot(&dvt[kDevCopyDescriptorsSimple], (void*)Hook_CopyDescriptorsSimple,
                            (void**)&o_CopyDescriptorsSimple);
}

DWORD WINAPI EarlyDeviceThread(void*)
{
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr) || !dev) {
        Log("depth layer: could not create a device to hook (0x%08x)", hr);
        return 0;
    }
    InstallDeviceHooks(dev);
    dev->Release();
    Log("depth layer: device hooks installed at start-up");
    return 0;
}

} // namespace

void HookDeviceEarly()
{
    if (!g_config.depthLayer) return;
    // a worker thread: creating D3D12 objects inside DllMain is unsafe
    HANDLE t = CreateThread(nullptr, 0, EarlyDeviceThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void Init(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    static bool done = false;
    if (done || !device || !queue) return;
    done = true;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))) ||
        o6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2 || !o6.ShadingRateImageTileSize) {
        Log("foveated: not available (needs variable rate shading tier 2, GPU reports tier %d)",
            (int)o6.VariableShadingRateTier);
        return;
    }
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        Log("foveated: could not create GPU objects");
        return;
    }
    g_list->Close();
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_device = device;
    g_queue = queue;
    g_tile = o6.ShadingRateImageTileSize;
    g_coarse = o6.AdditionalShadingRatesSupported ? D3D12_SHADING_RATE_4X4 : D3D12_SHADING_RATE_2X2;

    void** vt = *(void***)queue;
    // the original is stored before the slot is switched (other threads may call it right away)
    if (!PatchSlot(&vt[kQueueExecuteCommandLists], (void*)Hook_ECL, (void**)&o_ECL)) return;
    g_supported = true;
    g_enabled = g_config.foveated;

    InstallDeviceHooks(device);  // normally done at start-up already (HookDeviceEarly)
    Log("foveated rendering: %s (VRS tier %d, tile %u, 4x4 %s; centre %.2f, ring %.2f)",
        g_enabled ? "on" : "off (F5 toggles)", (int)o6.VariableShadingRateTier, g_tile,
        o6.AdditionalShadingRatesSupported ? "yes" : "no", g_config.foveaInner, g_config.foveaOuter);
}

void SetEyeFov(int eye, const float actual[4], const float rendered[4])
{
    if (!g_supported || eye < 0 || eye > 1) return;
    std::lock_guard<std::mutex> lock(g_fovMtx);
    EyeFov& f = g_fov[eye];
    bool changed = !f.valid;
    for (int i = 0; i < 4; i++)
        changed |= fabsf(f.a[i] - actual[i]) > 1e-3f || fabsf(f.r[i] - rendered[i]) > 1e-3f;
    if (!changed) return;
    memcpy(f.a, actual, sizeof(f.a));
    memcpy(f.r, rendered, sizeof(f.r));
    f.valid = true;
    g_fovVersion++;
}

void SetFrameEye(int eye) { g_frameEye.store(eye, std::memory_order_relaxed); }

void SetPresenting(bool on)
{
    if (on) g_presentThread = GetCurrentThreadId();
    g_presenting.store(on, std::memory_order_relaxed);
}

void Update(UINT width, UINT height, bool perEye)
{
    if (!g_supported || !width || !height) return;
    {
        std::lock_guard<std::mutex> lock(g_mtx);  // depth capture retires textures from render threads
        g_presents++;
        ReleaseRetired(false);
    }
    unsigned version;
    {
        std::lock_guard<std::mutex> lock(g_fovMtx);
        version = g_fovVersion;
    }
    bool resized;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_perEye = perEye && !g_overlap;
        resized = width != g_w || height != g_h;
    }
    if (resized || version != g_builtVersion) Build(width, height);
    LogStats();
}

bool SetEnabled(bool on)
{
    if (!g_supported) return false;
    if (g_enabled != on) Log("foveated rendering %s", on ? "on" : "off");
    g_enabled = on;
    return true;
}

bool IsEnabled() { return g_supported && g_enabled; }

bool EnableDepthCapture(bool on)
{
    if (on && (!g_supported || !g_dsvHooked)) {
        Log("depth layer: the game's depth cannot be captured (needs the variable rate shading hooks)");
        return false;
    }
    g_depthOn = on;
    return true;
}

bool TakeDepth(int eye, ID3D12Resource** depth)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_depthOn || eye < 0 || eye > 1 || !g_cap[eye] || g_capSeq[eye] != g_presents) return false;
    *depth = g_cap[eye];
    return true;
}

int DepthClearValue() { return g_depthClear; }

} // namespace vrs
