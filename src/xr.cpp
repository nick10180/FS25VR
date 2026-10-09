// OpenXR compositor for FS25.
//
// Stereo scheme: alternate-eye rendering with true per-eye geometry. Every game frame the Lua
// mod asks GetView() for the eye to draw; it places the game camera at that eye's predicted pose
// with that eye's asymmetric frustum. When the frame is presented we copy the backbuffer into that
// eye's OpenXR swapchain and submit a projection layer holding the newest image of each eye, each
// tagged with the exact pose it was rendered from, so the runtime reprojects both correctly.
// Frames rendered without a VR view (menus, loading screens) are shown on a flat quad instead.

#include "xr.h"
#include "blit.h"
#include "config.h"
#include "overlay.h"
#include "planes.h"
#include "log.h"
#include "profile.h"
#include "window.h"

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kRing = 16;
constexpr int kAllocs = 12;  // copies of three frames in flight (quad views: 4 per frame)

struct FrameRecord {
    uint64_t frame = ~0ull;
    int      eye = 0;
    XrPosef  pose{};   // raw LOCAL-space pose the image is rendered from
    XrFovf   fov{};    // frustum actually rendered (may be wider than the eye's)
    bool     hasOther = false; // plane stereo: the right eye, rendered as the engine's second view
    XrPosef  otherPose{};
    XrFovf   otherFov{};
    bool     hasFocus = false; // quad views: both eyes' focus views as the engine's third and fourth view
    XrFovf   focusFov[2]{};
};

struct Chain {
    XrSwapchain                              handle = XR_NULL_HANDLE;
    std::vector<ID3D12Resource*>             images;
    D3D12_CPU_DESCRIPTOR_HANDLE              rtvBase{};
    uint32_t                                 w = 0, h = 0;

    // Async mode: the game thread draws into a staging texture (never touching the OpenXR
    // swapchain); the compositor thread copies it into a swapchain image. Eye chains have two, so
    // the game can draw the next stereo pair while the compositor still has the last one.
    ID3D12Resource*                          staging[2] = {};     // rest in RENDER_TARGET
    D3D12_CPU_DESCRIPTOR_HANDLE              stagingRtv[2]{};
    int                                      writeSet = 0;        // staging texture the game draws into
    bool                                     staged = false;      // menu screen: new image not yet copied
};

struct State {
    std::recursive_mutex mtx;
    std::mutex           chainMtx;  // swapchain acquire/release (game thread) vs xrEndFrame (compositor)
    uint64_t             chainGen = 0;  // bumped (under chainMtx) whenever swapchains are destroyed
    bool                 compUsingChains = false;  // compositor is between acquire and xrEndFrame

    // compositor thread GPU objects (staging -> swapchain copies)
    ID3D12CommandAllocator*    cAlloc[3] = {};
    UINT64                     cAllocFence[3] = {};
    ID3D12GraphicsCommandList* cList = nullptr;
    ID3D12Fence*               cFence = nullptr;
    UINT64                     cFenceValue = 0;
    HANDLE                     cEvent = nullptr;
    int                        cIndex = 0;

    // compositor timing (seconds, summed over the stats window)
    double               cWait = 0, cBegin = 0, cAcquire = 0, cEnd = 0;
    uint64_t             cEyeImages = 0;
    uint64_t             compTotal = 0;
    FILE*                compCsv = nullptr;

    ID3D12Device*        device = nullptr;
    ID3D12CommandQueue*  queue = nullptr;

    XrInstance     instance = XR_NULL_HANDLE;
    XrSystemId     system = XR_NULL_SYSTEM_ID;
    XrSession      session = XR_NULL_HANDLE;
    XrSpace        local = XR_NULL_HANDLE;
    XrSpace        view = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool           running = false;
    bool           frameBegun = false;
    bool           shouldRender = false;
    XrTime         predictedTime = 0;
    XrDuration     predictedPeriod = 11111111;
    bool           fatal = false;
    uint64_t       nextInitAttempt = 0;

    DXGI_FORMAT    chainFormat = DXGI_FORMAT_UNKNOWN;
    Chain          eyes[2];
    Chain          quad;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    UINT           rtvInc = 0;

    bool           eyeValid[2] = {false, false};
    XrPosef        eyePose[2]{};
    XrFovf         eyeFov[2]{};
    bool           lastWasStereo = false;
    bool           showStereo = false;   // newest game frame was a 3D view (else flat menu screen)
    bool           quadValid = false;

    // Async mode hands eyes to the compositor in complete stereo pairs (the two frames between
    // which the simulation is frozen), so both eyes always change on the same headset refresh.
    // Latching each eye on its own lets one eye's updates land irregularly on the refreshes
    // whenever the game runs close to the headset's rate: that eye stutters while the other doesn't.
    int            pairMask = 0;         // eyes of the pair being drawn (bit per eye)
    XrPosef        pairPose[2]{};
    XrFovf         pairFov[2]{};
    bool           pairReady = false;    // a complete pair waits for the compositor
    XrView         pairViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};  // the pair's view query
    XrTime         pairTime = 0;         // display time it was made for
    uint64_t       pairViewFrame = ~0ull;
    // Plane stereo: the left eye is the game camera every frame, the right eye a second view of the
    // engine's main render path into a render overlay's texture, so every frame is a stereo pair
    bool           planeStereo = false;
    // Quad views: two more engine views, a narrower field of view at a higher pixel density per eye,
    // laid over the eyes' images (at the headset's recommended density) before they are submitted
    int            ovCount = 0;              // render overlays: 1 = right eye, 3 = + both focus views
    UINT           ovW[3] = {}, ovH[3] = {};  // their sizes
    int            readySet = 0;         // staging textures holding it
    XrPosef        readyPose[2]{};
    XrFovf         readyFov[2]{};

    // async submission: a compositor thread owns the OpenXR frame loop
    bool           async = true;
    std::atomic<bool> compRun{false};
    bool           compAlive = false;
    uint64_t       compFrames = 0;

    uint64_t       presentCount = 0;
    FrameRecord    ring[kRing];
    EyeView        cached{};
    uint64_t       cachedFrame = ~0ull;

    // Latency calibration: the Lua mod stamps (frame & 3) into a corner pixel; we read it back to
    // learn how many Presents pass between a Lua camera update and the image that shows it.
    int            lag = 0;
    bool           calibrating = true;
    int            votes[4] = {};
    int            calibSamples = 0;
    int            calibRounds = 0;
    ID3D12Resource* readback = nullptr;
    struct Sample_t { uint64_t present; UINT64 fence; int slot; DXGI_FORMAT fmt; };
    Sample_t       samples[8];
    int            sampleCount = 0;
    int            nextSlot = 0;

    XrPosef        recenter{{0, 0, 0, 1}, {0, 0, 0}};
    bool           recenterPending = true;

    UINT           bbW = 0, bbH = 0;
    UINT           recW = 0, recH = 0;   // runtime's recommended per-eye resolution

    // Desktop mirror: alternate-eye frames would flicker between the eyes on the monitor, so the
    // last left-eye image is kept and copied back over right-eye frames before they are shown.
    ID3D12Resource* mirror = nullptr;
    bool           mirrorValid = false;

    // frame rate statistics
    uint64_t       statPresents = 0, statStereo = 0;
    uint64_t       statPairsAligned = 0;  // pairs that straddled a headset frame (shared view query)
    ULONGLONG      statStart = 0;
    // where frame time goes (seconds, summed over the stats window)
    double         tGame = 0, tWait = 0, tSubmit = 0, tPresent = 0;
    double         tLockWait = 0;   // game thread time spent waiting for this state lock
    double         tChainWait = 0;  // game thread time waiting for a free swapchain image
    LARGE_INTEGER  tFrameStart{}, tPresentStart{};
    prof::CpuFrame cur;            // this frame's numbers for the profiler
    uint64_t       curFrame = 0;

    // frame-pair dump (debug): create x64\fs25vr_dump.txt to save the next stereo frames as BMPs
    int            dumpRemaining = 0;

    ID3D12CommandAllocator*    alloc[kAllocs] = {};
    UINT64                     allocFence[kAllocs] = {};
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence*               fence = nullptr;
    UINT64                     fenceValue = 0;
    HANDLE                     fenceEvent = nullptr;
    int                        allocIndex = 0;
    Blitter                    blit;

    char status[256] = "not started";
};

State S;

// Frames alternate between the eyes, and the simulation is frozen on the second frame of each pair.
int  EyeOfFrame(uint64_t f) { return (int)((f + (uint64_t)g_config.eyeOrder) & 1); }
bool IsSecondOfPair(uint64_t f) { return ((f + (uint64_t)g_config.syncPhase) & 1) == 1; }

double Seconds(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    return (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart;
}

LARGE_INTEGER Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t;
}

// ---------------------------------------------------------------------------------------------
// math

XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrQuaternionf QConj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v)
{
    XrQuaternionf p{v.x, v.y, v.z, 0};
    XrQuaternionf r = QMul(QMul(q, p), QConj(q));
    return {r.x, r.y, r.z};
}

XrPosef PoseCompose(const XrPosef& a, const XrPosef& b)  // a * b
{
    XrPosef r;
    r.orientation = QMul(a.orientation, b.orientation);
    XrVector3f t = QRot(a.orientation, b.position);
    r.position = {a.position.x + t.x, a.position.y + t.y, a.position.z + t.z};
    return r;
}

XrPosef PoseInverse(const XrPosef& p)
{
    XrPosef r;
    r.orientation = QConj(p.orientation);
    XrVector3f t = QRot(r.orientation, p.position);
    r.position = {-t.x, -t.y, -t.z};
    return r;
}

// Keeps only the heading of a head pose so recentring never tilts the world.
XrPosef YawOnly(const XrPosef& p)
{
    XrVector3f f = QRot(p.orientation, {0, 0, -1});
    float yaw = atan2f(-f.x, -f.z);
    XrPosef r;
    r.orientation = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    r.position = p.position;
    return r;
}

// ---------------------------------------------------------------------------------------------

#define XR_CHECK(call)                                                                    \
    do {                                                                                  \
        XrResult _r = (call);                                                             \
        if (XR_FAILED(_r)) {                                                              \
            char _b[64];                                                                  \
            if (S.instance) xrResultToString(S.instance, _r, _b); else sprintf(_b, "%d", _r); \
            Log("OpenXR: %s failed: %s", #call, _b);                                      \
            snprintf(S.status, sizeof(S.status), "%s failed: %s", #call, _b);             \
            return false;                                                                 \
        }                                                                                 \
    } while (0)

void DestroyChain(Chain& c)
{
    if (c.handle) xrDestroySwapchain(c.handle);
    for (auto* t : c.staging)
        if (t) t->Release();
    c = Chain{};
}

void WaitGpuIdle()
{
    if (!S.fence || !S.queue) return;
    UINT64 v = ++S.fenceValue;
    S.queue->Signal(S.fence, v);
    if (S.fence->GetCompletedValue() < v) {
        S.fence->SetEventOnCompletion(v, S.fenceEvent);
        WaitForSingleObject(S.fenceEvent, 2000);
    }
}

void DestroySwapchains()
{
    WaitGpuIdle();
    std::lock_guard<std::mutex> chains(S.chainMtx);  // never while the compositor submits them
    S.chainGen++;
    DestroyChain(S.eyes[0]);
    DestroyChain(S.eyes[1]);
    DestroyChain(S.quad);
    if (S.rtvHeap) S.rtvHeap->Release(), S.rtvHeap = nullptr;
    S.eyeValid[0] = S.eyeValid[1] = false;
    S.pairReady = false;
    S.pairMask = 0;
}

void DestroySession()
{
    DestroySwapchains();
    if (S.local) xrDestroySpace(S.local), S.local = XR_NULL_HANDLE;
    if (S.view) xrDestroySpace(S.view), S.view = XR_NULL_HANDLE;
    if (S.session) xrDestroySession(S.session), S.session = XR_NULL_HANDLE;
    S.running = false;
    S.frameBegun = false;
    S.sessionState = XR_SESSION_STATE_UNKNOWN;
}

bool InitGpu()
{
    if (S.cl) return true;
    for (int i = 0; i < kAllocs; i++)
        if (FAILED(S.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&S.alloc[i]))))
            return false;
    if (FAILED(S.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, S.alloc[0], nullptr,
                                           IID_PPV_ARGS(&S.cl))))
        return false;
    S.cl->Close();
    if (FAILED(S.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&S.fence)))) return false;
    S.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256 * 8;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(S.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&S.readback))))
        S.readback = nullptr;
    S.lag = std::max(0, g_config.presentLag);
    S.async = g_config.asyncSubmit;
    return true;
}

bool IsCalibFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM;
}

void FinishCalibration(const char* why)
{
    S.calibrating = false;
    Log("latency calibration: %s; presentLag = %d", why, S.lag);
}

// Reads back finished marker samples and votes on the Lua-to-Present offset.
void ProcessSamples()
{
    if (!S.calibrating || !S.readback) return;
    UINT64 done = S.fence->GetCompletedValue();
    int keep = 0;
    for (int i = 0; i < S.sampleCount; i++) {
        auto& sm = S.samples[i];
        if (sm.fence > done) {
            S.samples[keep++] = sm;
            continue;
        }
        D3D12_RANGE range = {(SIZE_T)sm.slot * 256, (SIZE_T)sm.slot * 256 + 4};
        BYTE* p = nullptr;
        if (FAILED(S.readback->Map(0, &range, (void**)&p))) continue;
        BYTE px[4];
        memcpy(px, p + sm.slot * 256, 4);
        D3D12_RANGE none = {0, 0};
        S.readback->Unmap(0, &none);
        BYTE r = px[0], g = px[1], b = px[2];
        if (sm.fmt == DXGI_FORMAT_B8G8R8A8_UNORM) std::swap(r, b);
        S.calibSamples++;
        if (b > 128) {
            int frameMod = (r > 128 ? 1 : 0) | (g > 128 ? 2 : 0);
            S.votes[(int)((sm.present - frameMod) & 3)]++;
        }
    }
    S.sampleCount = keep;

    int total = S.votes[0] + S.votes[1] + S.votes[2] + S.votes[3];
    if (total >= 40) {
        int best = 0;
        for (int d = 1; d < 4; d++)
            if (S.votes[d] > S.votes[best]) best = d;
        Log("latency calibration votes (offset 0..3): %d %d %d %d", S.votes[0], S.votes[1], S.votes[2], S.votes[3]);
        bool clear = S.votes[best] * 10 >= total * 8;
        memset(S.votes, 0, sizeof(S.votes));
        S.calibSamples = 0;
        if (clear && best == 0) {
            FinishCalibration("confirmed");
        } else if (++S.calibRounds > 4) {
            FinishCalibration("did not converge, keeping current value");
        } else if (clear) {
            S.lag += best;
            Log("latency calibration: adjusting presentLag by %d", best);
        }
    } else if (S.calibSamples > 600 && total < 10) {
        FinishCalibration("marker not visible, keeping configured value");
    }
}

bool InitInstance()
{
    const char* exts[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "Farming Simulator 25 VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "GIANTS Engine 10 + fs25vr");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    XR_CHECK(xrCreateInstance(&ci, &S.instance));

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(S.instance, &ip);
    Log("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    return true;
}

bool InitSession()
{
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(S.instance, &sgi, &S.system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        snprintf(S.status, sizeof(S.status), "headset not available");
        return false;
    }
    XR_CHECK(r);

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(S.instance, S.system, &sp);
    Log("OpenXR system: %s", sp.systemName);

    PFN_xrGetD3D12GraphicsRequirementsKHR getReq = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(S.instance, "xrGetD3D12GraphicsRequirementsKHR",
                                   (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    XR_CHECK(getReq(S.instance, S.system, &req));
    LUID devLuid = S.device->GetAdapterLuid();
    if (memcmp(&devLuid, &req.adapterLuid, sizeof(LUID)) != 0)
        Log("WARNING: the game renders on a different GPU than the headset is attached to");

    uint32_t n = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(S.instance, S.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               0, &n, nullptr));
    std::vector<XrViewConfigurationView> views(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(S.instance, S.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               n, &n, views.data()));
    if (n >= 1) {
        S.recW = views[0].recommendedImageRectWidth;
        S.recH = views[0].recommendedImageRectHeight;
        Log("OpenXR recommended eye resolution: %ux%u", S.recW, S.recH);
    }

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = S.device;
    binding.queue = S.queue;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = S.system;
    XR_CHECK(xrCreateSession(S.instance, &sci, &S.session));

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(S.session, &rs, &S.local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(S.session, &rs, &S.view));

    uint32_t fc = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(S.session, 0, &fc, nullptr));
    std::vector<int64_t> formats(fc);
    XR_CHECK(xrEnumerateSwapchainFormats(S.session, fc, &fc, formats.data()));
    const DXGI_FORMAT preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                     DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    S.chainFormat = DXGI_FORMAT_UNKNOWN;
    for (DXGI_FORMAT p : preferred) {
        for (int64_t f : formats)
            if (f == p) { S.chainFormat = p; break; }
        if (S.chainFormat != DXGI_FORMAT_UNKNOWN) break;
    }
    if (S.chainFormat == DXGI_FORMAT_UNKNOWN) {
        Log("OpenXR: no usable swapchain format");
        snprintf(S.status, sizeof(S.status), "no usable swapchain format");
        return false;
    }
    if (!S.blit.Init(S.device, S.chainFormat)) {
        snprintf(S.status, sizeof(S.status), "blit init failed");
        return false;
    }
    snprintf(S.status, sizeof(S.status), "session created, waiting for headset");
    Log("OpenXR session created (swapchain format %d)", S.chainFormat);
    return true;
}

bool CreateChain(Chain& c, uint32_t w, uint32_t h, UINT rtvOffset, UINT stagingRtvIndex, int stagingCount)
{
    // staging textures for async submission (same format and size as the swapchain images)
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = S.chainFormat;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (int i = 0; i < stagingCount; i++) {
        if (FAILED(S.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                     IID_PPV_ARGS(&c.staging[i])))) {
            Log("could not create staging texture %ux%u", w, h);
            return false;
        }
        c.stagingRtv[i] = S.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        c.stagingRtv[i].ptr += (SIZE_T)(stagingRtvIndex + i) * S.rtvInc;
        D3D12_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = S.chainFormat;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        S.device->CreateRenderTargetView(c.staging[i], &rd, c.stagingRtv[i]);
    }

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = S.chainFormat;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(S.session, &ci, &c.handle));
    uint32_t n = 0;
    XR_CHECK(xrEnumerateSwapchainImages(c.handle, 0, &n, nullptr));
    std::vector<XrSwapchainImageD3D12KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(c.handle, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()));
    if (n > 8) n = 8;
    c.w = w;
    c.h = h;
    c.rtvBase = S.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    c.rtvBase.ptr += (SIZE_T)rtvOffset * S.rtvInc;
    for (uint32_t i = 0; i < n; i++) {
        c.images.push_back(imgs[i].texture);
        D3D12_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = S.chainFormat;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h2 = c.rtvBase;
        h2.ptr += (SIZE_T)i * S.rtvInc;
        S.device->CreateRenderTargetView(imgs[i].texture, &rd, h2);
    }
    return true;
}

// the eyes' swapchains at w x h, the menu screen's at menuW x menuH
bool EnsureSwapchains(UINT w, UINT h, UINT menuW, UINT menuH)
{
    if (S.eyes[0].handle && S.eyes[0].w == w && S.eyes[0].h == h && S.quad.w == menuW && S.quad.h == menuH)
        return true;
    if (S.compUsingChains) return false;  // compositor is submitting them; rebuild on a later frame
    DestroySwapchains();
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 29;  // 3 chains x 8 swapchain images + 2+2+1 staging textures
    if (FAILED(S.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&S.rtvHeap)))) return false;
    S.rtvInc = S.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    int sets = S.async ? 2 : 0;
    if (!CreateChain(S.eyes[0], w, h, 0, 24, sets) || !CreateChain(S.eyes[1], w, h, 8, 26, sets) ||
        !CreateChain(S.quad, menuW, menuH, 16, 28, S.async ? 1 : 0))
        return false;
    Log("OpenXR swapchains created %ux%u (menu %ux%u)", w, h, menuW, menuH);
    return true;
}

// Quad views: the focus view of an eye, as the quad views layer does it without eye tracking: a
// section of the eye's field of view (fractions of its half extents) around straight ahead. Kept
// centred (screen-space effects assume it), so clamped on the narrower side.
XrFovf FocusFov(const XrFovf& eye)
{
    float l = tanf(eye.angleLeft), r = tanf(eye.angleRight);
    float d = tanf(eye.angleDown), u = tanf(eye.angleUp);
    float h = std::min({g_config.quadFocusWidth * 0.5f * (r - l), r, -l});
    float v = std::min({g_config.quadFocusHeight * 0.5f * (u - d), u, -d});
    return {atanf(-h), atanf(h), atanf(v), atanf(-v)};
}

// Where a focus view lies in its eye's image (w x h, rendered with the field of view `eye` from the same
// camera), with the configured blended edge.
BlitRect FocusRect(const XrFovf& eye, const XrFovf& focus, UINT w, UINT h)
{
    float l = tanf(eye.angleLeft), r = tanf(eye.angleRight), d = tanf(eye.angleDown), u = tanf(eye.angleUp);
    BlitRect rc;
    rc.x = (tanf(focus.angleLeft) - l) / (r - l) * (float)w;
    rc.w = (tanf(focus.angleRight) - tanf(focus.angleLeft)) / (r - l) * (float)w;
    rc.y = (u - tanf(focus.angleUp)) / (u - d) * (float)h;
    rc.h = (tanf(focus.angleUp) - tanf(focus.angleDown)) / (u - d) * (float)h;
    rc.smoothing = g_config.quadFocusSmoothing;
    return rc;
}

void BeginFrame()
{
    if (!S.running || S.frameBegun) return;
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrSession session = S.session;
    // Let the Lua thread keep running while we block on the compositor.
    S.mtx.unlock();
    LARGE_INTEGER w0 = Now();
    XrResult r = xrWaitFrame(session, &wi, &fs);
    LARGE_INTEGER w1 = Now();
    S.mtx.lock();
    S.tWait += Seconds(w0, w1);
    S.cur.wait = Seconds(w0, w1);
    if (XR_FAILED(r)) {
        Log("xrWaitFrame failed %d", r);
        return;
    }
    S.predictedTime = fs.predictedDisplayTime;
    S.predictedPeriod = fs.predictedDisplayPeriod;
    S.shouldRender = fs.shouldRender == XR_TRUE;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    r = xrBeginFrame(S.session, &bi);
    if (XR_FAILED(r)) {
        Log("xrBeginFrame failed %d", r);
        return;
    }
    S.frameBegun = true;

    if (S.recenterPending) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(S.view, S.local, S.predictedTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            S.recenter = YawOnly(loc.pose);
            S.recenterPending = false;
            Log("recentred at (%.2f %.2f %.2f)", S.recenter.position.x, S.recenter.position.y,
                S.recenter.position.z);
        }
    }
}

void PollEvents()
{
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (S.instance && xrPollEvent(S.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* e = (XrEventDataSessionStateChanged*)&ev;
            S.sessionState = e->state;
            Log("OpenXR session state %d", e->state);
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(S.session, &bi))) {
                    S.running = true;
                    S.recenterPending = true;
                    snprintf(S.status, sizeof(S.status), "running");
                }
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                if (S.frameBegun) {
                    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
                    fe.displayTime = S.predictedTime;
                    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    xrEndFrame(S.session, &fe);
                    S.frameBegun = false;
                }
                xrEndSession(S.session);
                S.running = false;
                snprintf(S.status, sizeof(S.status), "headset idle");
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                DestroySession();
                snprintf(S.status, sizeof(S.status), "session ended");
                S.nextInitAttempt = S.presentCount + 600;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            DestroySession();
            xrDestroyInstance(S.instance);
            S.instance = XR_NULL_HANDLE;
            S.nextInitAttempt = S.presentCount + 600;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            S.recenterPending = true;
            break;
        default:
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Copies the backbuffer into one image of 'chain'. Returns false if nothing was submitted.
enum class Mirror { None, Save, Restore };

bool EnsureMirror(ID3D12Resource* bb)
{
    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    if (S.mirror) {
        D3D12_RESOURCE_DESC md = S.mirror->GetDesc();
        if (md.Width == bd.Width && md.Height == bd.Height && md.Format == bd.Format) return true;
        WaitGpuIdle();
        S.mirror->Release();
        S.mirror = nullptr;
        S.mirrorValid = false;
    }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = bd;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    rd.MipLevels = 1;
    rd.DepthOrArraySize = 1;
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(S.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&S.mirror)))) {
        S.mirror = nullptr;
        return false;
    }
    S.mirrorValid = false;
    return true;
}

void RecordMirror(ID3D12Resource* bb, Mirror mode)
{
    if (mode == Mirror::None || !S.mirror) return;
    if (mode == Mirror::Restore && !S.mirrorValid) return;
    bool save = mode == Mirror::Save;
    D3D12_RESOURCE_BARRIER b[2] = {};
    for (auto& x : b) {
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    // the mirror rests in COPY_DEST between uses
    b[0].Transition.pResource = bb;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[0].Transition.StateAfter = save ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.pResource = S.mirror;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    S.cl->ResourceBarrier(save ? 1 : 2, b);
    if (save)
        S.cl->CopyResource(S.mirror, bb);
    else
        S.cl->CopyResource(bb, S.mirror);
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    S.cl->ResourceBarrier(save ? 1 : 2, b);
    if (save) S.mirrorValid = true;
}

bool CopyToChain(Chain& chain, ID3D12Resource* bb, DXGI_FORMAT bbFormat, bool sampleMarker = false,
                 Mirror mirror = Mirror::None, const CursorDraw* cursor = nullptr,
                 D3D12_RESOURCE_STATES srcState = D3D12_RESOURCE_STATE_PRESENT, const BlitRect* over = nullptr)
{
    // Async mode draws into the chain's staging texture and never calls OpenXR here: on some
    // runtimes (Quest via Steam Link) swapchain calls block until the next headset refresh, which
    // would pace the game to the headset. The compositor thread copies staging -> swapchain.
    // Sync mode writes the swapchain image directly.
    uint32_t idx = 0;
    if (!S.async) {
        LARGE_INTEGER a0 = Now();
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(chain.handle, &ai, &idx))) return false;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;  // 100 ms
        XrResult waited = xrWaitSwapchainImage(chain.handle, &wi);
        S.tChainWait += Seconds(a0, Now());
        if (XR_FAILED(waited)) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(chain.handle, &ri);
            return false;
        }
    }

    int a = S.allocIndex;
    S.allocIndex = (S.allocIndex + 1) % kAllocs;
    if (S.fence->GetCompletedValue() < S.allocFence[a]) {
        S.fence->SetEventOnCompletion(S.allocFence[a], S.fenceEvent);
        WaitForSingleObject(S.fenceEvent, 1000);
    }
    S.alloc[a]->Reset();
    S.cl->Reset(S.alloc[a], nullptr);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = srcState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const bool transition = srcState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    int markerSlot = -1;
    if (sampleMarker && S.readback && S.sampleCount < 8 && IsCalibFormat(bbFormat)) {
        markerSlot = S.nextSlot;
        S.nextSlot = (S.nextSlot + 1) % 8;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        S.cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = S.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = (UINT64)markerSlot * 256;
        dst.PlacedFootprint.Footprint = {bbFormat, 1, 1, 1, 256};
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box = {3, 3, 0, 4, 4, 1};
        S.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (transition || markerSlot >= 0) S.cl->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = chain.rtvBase;
    rtv.ptr += (SIZE_T)idx * S.rtvInc;
    if (S.async) rtv = chain.stagingRtv[chain.writeSet];
    S.blit.Record(S.cl, bb, bbFormat, rtv, chain.w, chain.h, cursor, over);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = srcState;
    if (transition) S.cl->ResourceBarrier(1, &b);
    RecordMirror(bb, mirror);
    S.cl->Close();
    ID3D12CommandList* lists[] = {S.cl};
    S.queue->ExecuteCommandLists(1, lists);
    S.allocFence[a] = ++S.fenceValue;
    S.queue->Signal(S.fence, S.allocFence[a]);
    if (markerSlot >= 0) S.samples[S.sampleCount++] = {S.presentCount, S.allocFence[a], markerSlot, bbFormat};

    if (!S.async) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(chain.handle, &ri);
    }
    return true;
}

// Frustum that the game renders for an eye given the backbuffer aspect, plus the matching
// setFovY / setProjectionOffset parameters. GIANTS builds its projection as
//   t = tan(fovY/2), w = t*aspect, left/right = 2w*offX -/+ w, bottom/top = 2t*offY -/+ t
void ComputeProjection(const XrFovf& eye, float aspect, EyeView& out, XrFovf& rendered)
{
    float l = tanf(eye.angleLeft), r = tanf(eye.angleRight);
    float d = tanf(eye.angleDown), u = tanf(eye.angleUp);
    static ULONGLONG lastLog = 0;
    if (GetTickCount64() - lastLog > 10000) {
        lastLog = GetTickCount64();
        Log("eye fov (deg): left %.1f right %.1f up %.1f down %.1f; symmetric=%d", eye.angleLeft * 57.2958f,
            eye.angleRight * 57.2958f, eye.angleUp * 57.2958f, eye.angleDown * 57.2958f, g_config.symmetricFrustum);
        if (g_config.symmetricFrustum && S.recW && S.recH) {
            // window size giving the runtime's recommended pixel density over the centred frustum
            float hw = 2.0f * std::max(r, -l) / (r - l), hh = 2.0f * std::max(u, -d) / (u - d);
            UINT w = ((UINT)(S.recW * hw) + 7) & ~7u, h = ((UINT)(S.recH * hh) + 7) & ~7u;
            Log("  recommended render size for the centred frustum: %ux%u (current %ux%u)", w, h, S.bbW, S.bbH);
        }
    }
    if (g_config.symmetricFrustum) {
        // Centred frustum enclosing the eye's field of view. Some screen-space effects rebuild
        // positions assuming a centred projection; costs some extra pixels at the edges.
        float h = std::max(r, -l), v = std::max(u, -d);
        l = -h, r = h, d = -v, u = v;
    }
    float cx = 0.5f * (r + l), cy = 0.5f * (u + d);
    float halfH = std::max(0.5f * (u - d), 0.5f * (r - l) / aspect);
    float halfW = halfH * aspect;
    out.fovY = 2.0f * atanf(halfH);
    out.offX = cx / (2.0f * halfW);
    out.offY = cy / (2.0f * halfH);
    rendered.angleLeft = atanf(cx - halfW);
    rendered.angleRight = atanf(cx + halfW);
    rendered.angleDown = atanf(cy - halfH);
    rendered.angleUp = atanf(cy + halfH);
}

// Size of the eyes' images handed to the runtime: the headset's recommended pixel density over the
// frustum rendered for a w x h image. Streaming runtimes scale every image to their stream size
// without averaging, so extra pixels would be skipped (no anti-aliasing from them); the bridge
// averages them itself instead. Smaller images stay as they are, except with quad views (the eye's
// image is scaled up there, for the focus view laid over it).
void EyeImageSize(UINT w, UINT h, bool scaleUp, UINT& outW, UINT& outH)
{
    outW = w, outH = h;
    if (!S.recW || S.pairViewFrame == ~0ull) return;
    const XrFovf& eye = S.pairViews[0].fov;
    EyeView unused;
    XrFovf rendered;
    ComputeProjection(eye, (float)w / (float)h, unused, rendered);
    float tw = (tanf(rendered.angleRight) - tanf(rendered.angleLeft)) / (tanf(eye.angleRight) - tanf(eye.angleLeft));
    UINT rw = ((UINT)((float)S.recW * tw) + 7) & ~7u;
    if (rw >= w && !scaleUp) return;
    outW = rw;
    outH = ((UINT)((float)rw * (float)h / (float)w) + 7) & ~7u;
}

// Debug: writes the backbuffer (half resolution) to x64\fs25vr_dump_<frame>_eye<n>.bmp. Stalls the GPU.
void DumpBackbuffer(ID3D12Resource* bb, int eye)
{
    D3D12_RESOURCE_DESC d = bb->GetDesc();
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return;
    UINT w = (UINT)d.Width, h = d.Height;
    UINT pitch = (w * 4 + 255) & ~255u;
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (UINT64)pitch * h;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    if (FAILED(S.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&buf))))
        return;
    int a = S.allocIndex;
    S.allocIndex = (S.allocIndex + 1) % kAllocs;
    if (S.fence->GetCompletedValue() < S.allocFence[a]) {
        S.fence->SetEventOnCompletion(S.allocFence[a], S.fenceEvent);
        WaitForSingleObject(S.fenceEvent, 1000);
    }
    S.alloc[a]->Reset();
    S.cl->Reset(S.alloc[a], nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    S.cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {d.Format, w, h, 1, pitch};
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    S.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    S.cl->ResourceBarrier(1, &b);
    S.cl->Close();
    ID3D12CommandList* lists[] = {S.cl};
    S.queue->ExecuteCommandLists(1, lists);
    S.allocFence[a] = ++S.fenceValue;
    S.queue->Signal(S.fence, S.allocFence[a]);
    WaitGpuIdle();

    BYTE* p = nullptr;
    if (SUCCEEDED(buf->Map(0, nullptr, (void**)&p))) {
        UINT ow = w / 2, oh = h / 2;
        wchar_t name[MAX_PATH];
        swprintf(name, MAX_PATH, L"%sfs25vr_dump_%llu_eye%d.bmp", ModuleDir().c_str(),
                 (unsigned long long)S.presentCount, eye);
        if (FILE* f = _wfopen(name, L"wb")) {
            BITMAPFILEHEADER fh = {0x4D42, 0, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
            BITMAPINFOHEADER ih = {sizeof(ih), (LONG)ow, -(LONG)oh, 1, 32, BI_RGB};
            fh.bfSize = fh.bfOffBits + ow * oh * 4;
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            std::vector<BYTE> row(ow * 4);
            bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
            for (UINT y = 0; y < oh; y++) {
                const BYTE* srow = p + (size_t)(y * 2) * pitch;
                for (UINT x = 0; x < ow; x++) {
                    const BYTE* px = srow + x * 8;
                    row[x * 4 + 0] = rgba ? px[2] : px[0];
                    row[x * 4 + 1] = px[1];
                    row[x * 4 + 2] = rgba ? px[0] : px[2];
                    row[x * 4 + 3] = 255;
                }
                fwrite(row.data(), row.size(), 1, f);
            }
            fclose(f);
            Log("dump: wrote frame %llu eye %d", (unsigned long long)S.presentCount, eye);
        }
        buf->Unmap(0, nullptr);
    }
    buf->Release();
}

bool TryInit()
{
    if (S.fatal || !S.device || !S.queue) return false;
    if (S.presentCount < S.nextInitAttempt) return false;
    S.nextInitAttempt = S.presentCount + 300;
    if (!InitGpu()) {
        Log("GPU objects could not be created");
        S.fatal = true;
        return false;
    }
    if (!S.instance && !InitInstance()) return false;
    if (!S.session && !InitSession()) {
        if (S.session) DestroySession();
        return false;
    }
    return true;
}

} // namespace

namespace vr {

void OnSwapChainCreated(ID3D12CommandQueue* queue, IDXGISwapChain* swap)
{
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    if (!queue) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev)))) return;
    if (S.device && S.device != dev) {
        Log("swap chain created on a new device; restarting VR");
        DestroySession();
    }
    S.device = dev;  // keep the reference for the lifetime of the process
    if (S.queue != queue) {
        queue->AddRef();
        S.queue = queue;
    }
    Log("swap chain created (queue %p, device %p)", queue, dev);
    overlay::Install(dev);
    prof::Init(dev, queue);
    prof::HookQueue(queue);
}

void OnPresentImpl(IDXGISwapChain* swap, LARGE_INTEGER onPresentStart);

void OnPresent(IDXGISwapChain* swap)
{
    LARGE_INTEGER lockStart = Now();
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    LARGE_INTEGER onPresentStart = Now();
    S.tLockWait += Seconds(lockStart, onPresentStart);
    S.cur = prof::CpuFrame{};
    S.curFrame = S.presentCount;
    UpdateMouseClip();
    if (S.tFrameStart.QuadPart) {
        S.cur.game = Seconds(S.tFrameStart, onPresentStart);
        S.tGame += S.cur.game;
    }
    prof::GameWorkDone(S.curFrame);
    overlay::OnFrameEnd();
    OnPresentImpl(swap, onPresentStart);
    prof::VrWorkDone(S.curFrame);
    S.tPresentStart = Now();
    S.cur.submit = Seconds(onPresentStart, S.tPresentStart);
    S.tSubmit += S.cur.submit;
}

// Game thread: copies the finished backbuffer into the swapchain of the eye it was rendered for
// (or onto the flat menu screen) and records the pose/frustum it was rendered with.
void CopyFrame(IDXGISwapChain* swap)
{
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(swap->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    UINT bbIndex = sc3->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    sc3->GetBuffer(bbIndex, IID_PPV_ARGS(&bb));
    sc3->Release();
    if (!bb) return;
    D3D12_RESOURCE_DESC desc = bb->GetDesc();
    S.bbW = (UINT)desc.Width;
    S.bbH = desc.Height;

    const bool quad = S.planeStereo && S.ovCount == 3;
    UINT eyeW, eyeH;
    EyeImageSize(S.bbW, S.bbH, quad, eyeW, eyeH);
    if ((S.shouldRender || g_config.forceRender || S.async) && EnsureSwapchains(eyeW, eyeH, S.bbW, S.bbH)) {
        // the game draws the OS cursor, which is not part of the backbuffer; draw one into the copy
        CursorDraw cursor;
        if (g_config.showCursor && CursorInBackbuffer(S.bbW, S.bbH, cursor.x, cursor.y)) {
            cursor.visible = true;
            cursor.unit = std::max(1.0f, (float)S.bbH / 1080.0f * 1.25f);
        }
        const FrameRecord& rec = S.ring[S.presentCount % kRing];
        bool stereo = rec.frame == S.presentCount;
        S.cur.shouldRender = S.shouldRender;
        S.cur.eye = stereo ? rec.eye : -1;
        if (stereo && S.dumpRemaining > 0) {
            DumpBackbuffer(bb, rec.eye);
            S.dumpRemaining--;
        }
        if (stereo) {
            if (S.calibrating && !IsCalibFormat(desc.Format))
                FinishCalibration("backbuffer format cannot be sampled, keeping configured value");
            Mirror m = EnsureMirror(bb) ? (rec.eye == 0 ? Mirror::Save : Mirror::Restore) : Mirror::None;
            if (!S.lastWasStereo) {  // stale images from before a menu
                S.eyeValid[rec.eye ^ 1] = false;
                if (S.async) S.eyeValid[rec.eye] = false;  // replaced only when a whole pair is ready
                S.pairMask = 0;
            }
            const bool closer = IsSecondOfPair(rec.frame);  // completes a pair
            if (S.async && !closer) S.pairMask = 0;  // a new pair starts
            DXGI_FORMAT ovFormat = DXGI_FORMAT_UNKNOWN;
            D3D12_RESOURCE_STATES ovState = D3D12_RESOURCE_STATE_COMMON;
            ID3D12Resource* ovImage = rec.hasOther && S.async && !S.calibrating
                                          ? overlay::Image(0, ovFormat, ovState)
                                          : nullptr;
            // quad views: the focus views' images, laid over the eyes' images
            ID3D12Resource* fImage[2] = {};
            DXGI_FORMAT fFormat[2] = {};
            D3D12_RESOURCE_STATES fState[2] = {};
            if (ovImage && rec.hasFocus && quad) {
                for (int e = 0; e < 2; e++) fImage[e] = overlay::Image(1 + e, fFormat[e], fState[e]);
                if (!fImage[0] || !fImage[1]) ovImage = nullptr;
            }
            if (rec.hasOther && !ovImage) {
                // plane stereo, but no right-eye image (yet): keep showing the last pair
                S.pairMask = 0;
            } else if (ovImage) {
                // both eyes of this frame: the window's image and the overlay texture's
                // The main pipeline writes its already encoded output through the texture's sRGB
                // view, encoding twice: read through the sRGB view.
                auto readable = [](DXGI_FORMAT& f) {
                    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) f = DXGI_FORMAT_R8G8B8A8_UNORM;
                    if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) f = DXGI_FORMAT_B8G8R8A8_UNORM;
                    if (f == DXGI_FORMAT_R10G10B10A2_TYPELESS) f = DXGI_FORMAT_R10G10B10A2_UNORM;
                };
                readable(ovFormat);
                bool okL = CopyToChain(S.eyes[0], bb, desc.Format, false, m, &cursor);
                // Plane stereo renders the right eye from the left eye's camera moved by the eye
                // offset, without the (tiny) rotation between the eyes: shown with that pose.
                XrPosef otherPose = rec.otherPose;
                otherPose.orientation = rec.pose.orientation;
                bool okR = okL && CopyToChain(S.eyes[1], ovImage, ovFormat, false, Mirror::None, nullptr, ovState);
                // quad views: each eye's focus view over its image
                bool okF = true;
                for (int e = 0; okR && fImage[0] && e < 2; e++) {
                    readable(fFormat[e]);
                    BlitRect rc = FocusRect(e ? rec.otherFov : rec.fov, rec.focusFov[e], S.eyes[e].w, S.eyes[e].h);
                    okF = okF && CopyToChain(S.eyes[e], fImage[e], fFormat[e], false, Mirror::None, nullptr,
                                             fState[e], &rc);
                }
                if (okL && okR && okF) {
                    int set = S.eyes[0].writeSet;
                    S.readySet = set;
                    S.readyPose[0] = rec.pose;
                    S.readyFov[0] = rec.fov;
                    S.readyPose[1] = otherPose;
                    S.readyFov[1] = rec.otherFov;
                    S.eyes[0].writeSet = S.eyes[1].writeSet = set ^ 1;
                    S.pairReady = true;
                }
                S.pairMask = 0;
            } else if (CopyToChain(S.eyes[rec.eye], bb, desc.Format, S.calibrating, m, &cursor)) {
                if (S.async) {  // the compositor submits complete pairs (and sets eyeValid/eyePose)
                    S.pairMask |= 1 << rec.eye;
                    S.pairPose[rec.eye] = rec.pose;
                    S.pairFov[rec.eye] = rec.fov;
                    if (closer) {
                        if (S.pairMask == 3) {
                            int set = S.eyes[rec.eye].writeSet;
                            S.readySet = set;
                            for (int e = 0; e < 2; e++) {
                                S.readyPose[e] = S.pairPose[e];
                                S.readyFov[e] = S.pairFov[e];
                                S.eyes[e].writeSet = set ^ 1;  // the next pair goes into the other textures
                            }
                            S.pairReady = true;
                        }
                        S.pairMask = 0;
                    }
                } else {
                    S.eyeValid[rec.eye] = true;
                    S.eyePose[rec.eye] = rec.pose;
                    S.eyeFov[rec.eye] = rec.fov;
                }
            }
        } else if (CopyToChain(S.quad, bb, desc.Format, false, Mirror::None, &cursor)) {
            if (S.async)
                S.quad.staged = true;
            else
                S.quadValid = true;
        }
        S.showStereo = stereo;
        S.lastWasStereo = stereo;
        if (!stereo) S.mirrorValid = false;
        if (g_config.debugLog)
            Log("present %llu: %s eye %d", S.presentCount, stereo ? "stereo" : "flat", stereo ? rec.eye : -1);
    }
    bb->Release();
}

// Ends the current OpenXR frame with the newest images: both eyes (each with its own render pose)
// while the game shows the 3D view, otherwise the flat menu screen.
void SubmitFrame()
{
    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (S.shouldRender || g_config.forceRender) {
        if (S.showStereo && S.eyeValid[0] && S.eyeValid[1]) {
            for (int e = 0; e < 2; e++) {
                pv[e].pose = S.eyePose[e];
                pv[e].fov = S.eyeFov[e];
                pv[e].subImage.swapchain = S.eyes[e].handle;
                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)S.eyes[e].w, (int32_t)S.eyes[e].h}};
            }
            proj.space = S.local;
            proj.viewCount = 2;
            proj.views = pv;
            layers.push_back((XrCompositionLayerBaseHeader*)&proj);
        } else if (!S.showStereo && S.quadValid && S.quad.handle) {
            XrPosef offset{{0, 0, 0, 1}, {0, 0, -g_config.menuDistance}};
            quad.space = S.local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = S.quad.handle;
            quad.subImage.imageRect = {{0, 0}, {(int32_t)S.quad.w, (int32_t)S.quad.h}};
            quad.pose = PoseCompose(S.recenter, offset);
            quad.size = {g_config.menuWidth, g_config.menuWidth * (float)S.quad.h / (float)S.quad.w};
            layers.push_back((XrCompositionLayerBaseHeader*)&quad);
        }
    }
    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
    fe.displayTime = S.predictedTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = (uint32_t)layers.size();
    fe.layers = layers.data();
    XrResult r = xrEndFrame(S.session, &fe);
    if (XR_FAILED(r)) Log("xrEndFrame failed %d", r);
    S.frameBegun = false;
    S.compFrames++;
}

// Compositor thread: copies each marked staging texture into its acquired swapchain image.
// Called with the state lock held, so the game thread cannot draw into a staging texture until
// the copy has been submitted (the GPU executes the queue in submission order).
int RecordStagingCopies(Chain* chains[3], const bool copy[3], const uint32_t idx[3], const int set[3])
{
    if (!S.cList) {
        for (auto& a : S.cAlloc)
            if (FAILED(S.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return 0;
        if (FAILED(S.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, S.cAlloc[0], nullptr,
                                               IID_PPV_ARGS(&S.cList))))
            return 0;
        S.cList->Close();
        if (FAILED(S.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&S.cFence)))) return 0;
        S.cEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    int a = S.cIndex;
    S.cIndex = (S.cIndex + 1) % 3;
    if (S.cFence->GetCompletedValue() < S.cAllocFence[a]) {
        S.cFence->SetEventOnCompletion(S.cAllocFence[a], S.cEvent);
        WaitForSingleObject(S.cEvent, 1000);
    }
    S.cAlloc[a]->Reset();
    S.cList->Reset(S.cAlloc[a], nullptr);
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (!copy[i]) continue;
        ID3D12Resource* img = chains[i]->images[idx[i]];
        ID3D12Resource* src = chains[i]->staging[set[i]];
        D3D12_RESOURCE_BARRIER b[2] = {};
        for (auto& x : b) {
            x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        b[0].Transition.pResource = src;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1].Transition.pResource = img;  // swapchain images are handed out in RENDER_TARGET
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        S.cList->ResourceBarrier(2, b);
        S.cList->CopyResource(img, src);
        std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
        std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
        S.cList->ResourceBarrier(2, b);
        n++;
    }
    S.cList->Close();
    ID3D12CommandList* lists[] = {S.cList};
    S.queue->ExecuteCommandLists(1, lists);
    S.cAllocFence[a] = ++S.cFenceValue;
    S.queue->Signal(S.cFence, S.cAllocFence[a]);
    return n;
}

// Async mode, one headset frame. The OpenXR frame calls (which can block for most of a headset
// frame inside the runtime) are made WITHOUT the state lock: the game thread takes that lock
// several times per frame (Present, eye-pose queries from Lua) and must never queue behind the
// runtime. Only the frame data is copied under the lock; swapchain access is serialised with the
// game thread's copies by the small chain lock.
void CompositorFrame()
{
    XrSession session;
    XrSpace local, view;
    bool wantRecenter;
    {
        std::lock_guard<std::recursive_mutex> lock(S.mtx);
        if (!S.running) return;
        session = S.session;
        local = S.local;
        view = S.view;
        wantRecenter = S.recenterPending;
    }

    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    LARGE_INTEGER w0 = Now();
    XrResult r = xrWaitFrame(session, &wi, &fs);
    LARGE_INTEGER w1 = Now();
    if (XR_FAILED(r)) {
        Log("xrWaitFrame failed %d", r);
        Sleep(5);
        return;
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    r = xrBeginFrame(session, &bi);
    if (XR_FAILED(r)) {
        Log("xrBeginFrame failed %d", r);
        return;
    }
    LARGE_INTEGER b1 = Now();
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    bool recentred = wantRecenter && XR_SUCCEEDED(xrLocateSpace(view, local, fs.predictedDisplayTime, &loc)) &&
                     (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                     (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);

    // 1. which chains have a new image from the game? Mark the chains in use so the game thread
    //    does not rebuild them until this frame has been submitted.
    Chain* chains[3] = {&S.eyes[0], &S.eyes[1], &S.quad};
    bool copy[3] = {false, false, false};
    {
        std::lock_guard<std::recursive_mutex> lock(S.mtx);
        S.compUsingChains = true;
        for (int i = 0; i < 2; i++) copy[i] = S.pairReady && chains[i]->handle && chains[i]->staging[1];
        copy[2] = chains[2]->staged && chains[2]->handle && chains[2]->staging[0];
    }

    // 2. get free swapchain images (may block on some runtimes - this thread only, never the game)
    LARGE_INTEGER a0 = Now();
    uint32_t idx[3] = {};
    for (int i = 0; i < 3; i++) {
        if (!copy[i]) continue;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(chains[i]->handle, &ai, &idx[i]))) { copy[i] = false; continue; }
        XrSwapchainImageWaitInfo wi2{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi2.timeout = 100000000;  // 100 ms
        if (XR_FAILED(xrWaitSwapchainImage(chains[i]->handle, &wi2))) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(chains[i]->handle, &ri);
            copy[i] = false;
        }
    }
    LARGE_INTEGER a1 = Now();

    // 3. under the lock (so the game cannot overwrite a staging texture before the copy executes):
    //    copy staging -> swapchain image, take over the pose it was rendered with, build the layers
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerBaseHeader* layers[1];
    uint32_t layerCount = 0;
    uint64_t gen;
    int copied = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(S.mtx);
        // the newest pair: the game may have published another since step 1 (into the other
        // staging textures), which is read together with its poses here under the lock
        const int set[3] = {S.readySet, S.readySet, 0};
        if (copy[0] || copy[1] || copy[2]) copied = RecordStagingCopies(chains, copy, idx, set);
        for (int e = 0; e < 2; e++)
            if (copy[e]) {
                S.eyePose[e] = S.readyPose[e];
                S.eyeFov[e] = S.readyFov[e];
                S.eyeValid[e] = true;
            }
        if (copy[0] || copy[1]) S.pairReady = false;
        if (copy[2]) {
            S.quadValid = true;
            chains[2]->staged = false;
        }
        gen = S.chainGen;
        S.predictedTime = fs.predictedDisplayTime;
        S.predictedPeriod = fs.predictedDisplayPeriod;
        S.shouldRender = fs.shouldRender == XR_TRUE;
        S.tWait += Seconds(w0, w1);
        S.compFrames++;
        if (recentred) {
            S.recenter = YawOnly(loc.pose);
            S.recenterPending = false;
            Log("recentred at (%.2f %.2f %.2f)", S.recenter.position.x, S.recenter.position.y, S.recenter.position.z);
        }
        if (S.shouldRender || g_config.forceRender) {
            if (S.showStereo && S.eyeValid[0] && S.eyeValid[1]) {
                for (int e = 0; e < 2; e++) {
                    pv[e].pose = S.eyePose[e];
                    pv[e].fov = S.eyeFov[e];
                    pv[e].subImage.swapchain = S.eyes[e].handle;
                    pv[e].subImage.imageRect = {{0, 0}, {(int32_t)S.eyes[e].w, (int32_t)S.eyes[e].h}};
                }
                proj.space = local;
                proj.viewCount = 2;
                proj.views = pv;
                layers[layerCount++] = (XrCompositionLayerBaseHeader*)&proj;
            } else if (!S.showStereo && S.quadValid && S.quad.handle) {
                XrPosef offset{{0, 0, 0, 1}, {0, 0, -g_config.menuDistance}};
                quad.space = local;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = S.quad.handle;
                quad.subImage.imageRect = {{0, 0}, {(int32_t)S.quad.w, (int32_t)S.quad.h}};
                quad.pose = PoseCompose(S.recenter, offset);
                quad.size = {g_config.menuWidth, g_config.menuWidth * (float)S.quad.h / (float)S.quad.w};
                layers[layerCount++] = (XrCompositionLayerBaseHeader*)&quad;
            }
        }
    }

    // 4. hand the images back (the runtime waits on the queue for the copies) and end the frame
    for (int i = 0; i < 3; i++)
        if (copy[i]) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(chains[i]->handle, &ri);
        }
    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
    fe.displayTime = fs.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    LARGE_INTEGER e0 = Now();
    {
        std::lock_guard<std::mutex> chainLock(S.chainMtx);
        fe.layerCount = gen == S.chainGen ? layerCount : 0;  // swapchains rebuilt since the snapshot
        fe.layers = layers;
        r = xrEndFrame(session, &fe);
    }
    LARGE_INTEGER e1 = Now();
    if (XR_FAILED(r)) Log("xrEndFrame failed %d", r);

    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    S.compUsingChains = false;
    // compositor timing (log summary every 10 s; per-frame CSV with profile=1)
    double tw = Seconds(w0, w1), tb = Seconds(w1, b1), ta = Seconds(a0, a1), te = Seconds(e0, e1);
    S.cWait += tw;
    S.cBegin += tb;
    S.cAcquire += ta;
    S.cEnd += te;
    S.cEyeImages += (copy[0] ? 1 : 0) + (copy[1] ? 1 : 0);
    if (g_config.profile) {
        if (!S.compCsv) {
            S.compCsv = _wfopen((ModuleDir() + L"fs25vr_compositor.csv").c_str(), L"w");
            if (S.compCsv)
                fprintf(S.compCsv, "headset_frame,wait_frame_ms,begin_frame_ms,acquire_images_ms,end_frame_ms,"
                                   "left_new,right_new,menu_new,layers,should_render\n");
        }
        if (S.compCsv) {
            fprintf(S.compCsv, "%llu,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%u,%d\n", (unsigned long long)S.compTotal, tw * 1000,
                    tb * 1000, ta * 1000, te * 1000, copy[0], copy[1], copy[2], (unsigned)fe.layerCount,
                    S.shouldRender);
            if ((S.compTotal % 90) == 0) fflush(S.compCsv);
        }
    }
    S.compTotal++;
    (void)copied;
}

// Async mode: the OpenXR frame loop runs here at the headset's rate, independent of the game.
void CompositorLoop()
{
    Log("compositor thread started");
    while (S.compRun) {
        bool running;
        {
            std::lock_guard<std::recursive_mutex> lock(S.mtx);
            PollEvents();
            if (!S.session) break;
            running = S.running;
        }
        if (!running) {
            Sleep(10);
            continue;
        }
        CompositorFrame();
    }
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    S.compAlive = false;
    Log("compositor thread stopped");
}

void StartCompositor()
{
    if (!S.async || S.compAlive) return;
    S.compRun = true;
    S.compAlive = true;
    std::thread(CompositorLoop).detach();  // lives until the session ends (or the process exits)
}

void OnPresentImpl(IDXGISwapChain* swap, LARGE_INTEGER onPresentStart)
{
    if (!S.session) {
        if (TryInit()) StartCompositor();
    }
    if (S.async) {
        if (S.session && !S.compAlive) StartCompositor();
        if (!S.running) return;
        ProcessSamples();
        CopyFrame(swap);
        return;
    }
    PollEvents();
    if (S.running && !S.frameBegun) BeginFrame();
    if (!S.running || !S.frameBegun) return;
    ProcessSamples();
    CopyFrame(swap);
    SubmitFrame();
}

void LogStats()
{
    S.statPresents++;
    if (S.cur.eye >= 0) S.statStereo++;
    ULONGLONG now = GetTickCount64();
    if (!S.statStart) S.statStart = now;
    if (now - S.statStart < 10000) return;
    double sec = (now - S.statStart) / 1000.0;
    double n = (double)std::max<uint64_t>(1, S.statPresents);
    Log("frame rate: %.1f fps (%.1f stereo frames/s = %.1f updates per eye/s), render %ux%u%s", S.statPresents / sec,
        S.statStereo / sec, S.statStereo / sec / 2, S.bbW, S.bbH, S.shouldRender ? "" : " [headset not rendering]");
    Log("  per frame CPU: game %.1f ms, VR submit %.1f ms, Present %.1f ms, waiting for headset %.1f ms%s",
        S.tGame / n * 1000, S.tSubmit / n * 1000, S.tPresent / n * 1000, S.async ? 0.0 : S.tWait / n * 1000,
        S.async ? " (async: headset paced separately)" : "");
    Log("  game thread waits: state lock %.2f ms, free headset image %.2f ms per frame", S.tLockWait / n * 1000,
        S.tChainWait / n * 1000);
    if (S.async) {
        double cn = (double)std::max<uint64_t>(1, S.compFrames);
        Log("  compositor: %.1f headset frames/s, %.1f new eye images/s; per headset frame: wait %.1f ms, "
            "begin %.2f ms, image acquire %.2f ms, end %.2f ms",
            S.compFrames / sec, S.cEyeImages / sec, S.cWait / cn * 1000, S.cBegin / cn * 1000,
            S.cAcquire / cn * 1000, S.cEnd / cn * 1000);
        if (S.statStereo)
            Log("  stereo pairs straddling a headset frame (both eyes from one head pose): %.0f%%",
                100.0 * S.statPairsAligned / std::max(1.0, S.statStereo / 2.0));
        S.statPairsAligned = 0;
        S.compFrames = 0;
        S.cWait = S.cBegin = S.cAcquire = S.cEnd = 0;
        S.cEyeImages = 0;
    }
    if (prof::Enabled()) {
        prof::Summary g = prof::TakeSummary();
        Log("  per frame GPU: game %.2f ms (left eye %.2f, right eye %.2f), VR copy %.2f ms; main loop CPU left %.2f / right %.2f ms",
            g.gpuFrame, g.gpuFrameL, g.gpuFrameR, g.gpuVr, g.runFrameL, g.runFrameR);
    }
    S.statPresents = S.statStereo = 0;
    S.tGame = S.tWait = S.tSubmit = S.tPresent = S.tLockWait = S.tChainWait = 0;
    S.statStart = now;
}

void OnPostPresent()
{
    LARGE_INTEGER lockStart = Now();
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    S.tLockWait += Seconds(lockStart, Now());
    if (S.tPresentStart.QuadPart) {
        S.cur.present = Seconds(S.tPresentStart, Now());
        S.tPresent += S.cur.present;
    }
    S.tPresentStart.QuadPart = 0;
    S.presentCount++;
    S.cur.wait = 0;
    if (S.running && !S.async) BeginFrame();
    prof::EndFrame(S.curFrame, S.cur);
    LogStats();
    if ((S.presentCount % 60) == 0) {
        std::wstring trigger = ModuleDir() + L"fs25vr_dump.txt";
        if (GetFileAttributesW(trigger.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(trigger.c_str());
            S.dumpRemaining = 4;
            Log("dump: saving the next 4 stereo frames");
        }
    }
    S.tFrameStart = Now();
}

void OnResizeBuffers()
{
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    WaitGpuIdle();
}

bool IsRunning()
{
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    return S.running;
}

bool GetView(EyeView& out)
{
    LARGE_INTEGER lockStart = Now();
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    S.tLockWait += Seconds(lockStart, Now());
    if (!S.running || S.bbW == 0) return false;
    uint64_t target = S.presentCount + (uint64_t)S.lag;
    if (S.cachedFrame == target) {
        out = S.cached;
        return true;
    }

    const bool ovStereo = S.planeStereo && S.ovCount;
    int eye = ovStereo ? 0 : EyeOfFrame(target);
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = S.predictedTime + (XrTime)(S.lag + (S.async ? 1 : 0)) * S.predictedPeriod;
    // Both eyes of a pair are shown together, so both come from ONE view query, like a stereo
    // camera: the same display time and the same tracking sample, i.e. exactly one head pose.
    // Querying per eye gives two slightly different head poses (and, when a headset frame starts
    // between the two renders, display times a whole headset frame apart); streaming runtimes that
    // reproject with a single head pose then show the other eye jittering.
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (S.async && !ovStereo && IsSecondOfPair(target) && S.pairViewFrame == target - 1) {
        views[0] = S.pairViews[0];
        views[1] = S.pairViews[1];
        if (li.displayTime != S.pairTime) S.statPairsAligned++;
    } else {
        li.space = S.local;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t n = 0;
        if (XR_FAILED(xrLocateViews(S.session, &li, &vs, 2, &n, views)) || n != 2) return false;
        if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;
        S.pairViews[0] = views[0];
        S.pairViews[1] = views[1];
        S.pairTime = li.displayTime;
        S.pairViewFrame = target;
    }

    FrameRecord& rec = S.ring[target % kRing];
    rec.frame = target;
    rec.eye = eye;
    rec.pose = views[eye].pose;
    ComputeProjection(views[eye].fov, (float)S.bbW / (float)S.bbH, out, rec.fov);
    rec.hasOther = ovStereo;
    if (ovStereo) {
        // the right eye as the engine's second view (the overlay texture's aspect), offset from the
        // left eye's camera
        EyeView o{};
        rec.otherPose = views[1].pose;
        ComputeProjection(views[1].fov, (float)S.ovW[0] / (float)S.ovH[0], o, rec.otherFov);
        float sc = g_config.worldScale;
        XrPosef rel = PoseCompose(PoseInverse(views[0].pose), views[1].pose);
        float off[3] = {rel.position.x * sc, rel.position.y * sc, rel.position.z * sc};
        auto tans = [](const XrFovf& f, float t[4]) {
            t[0] = tanf(f.angleLeft), t[1] = tanf(f.angleRight), t[2] = tanf(f.angleDown), t[3] = tanf(f.angleUp);
        };
        float t[4];
        tans(rec.otherFov, t);
        planes::SetPlaneView(1, off, t);
        rec.hasFocus = S.ovCount == 3;
        if (rec.hasFocus) {
            // each eye's focus view, from that eye's camera
            const float none[3] = {};
            for (int e = 0; e < 2; e++) {
                rec.focusFov[e] = FocusFov(views[e].fov);
                tans(rec.focusFov[e], t);
                planes::SetPlaneView(2 + e, e ? off : none, t);
            }
        }
    }

    XrPosef p = PoseCompose(PoseInverse(S.recenter), views[eye].pose);
    float s = g_config.worldScale;
    out.eye = eye;
    out.pos[0] = p.position.x * s;
    out.pos[1] = p.position.y * s;
    out.pos[2] = p.position.z * s;
    out.quat[0] = p.orientation.x;
    out.quat[1] = p.orientation.y;
    out.quat[2] = p.orientation.z;
    out.quat[3] = p.orientation.w;
    out.frame = target;
    out.second = !ovStereo && IsSecondOfPair(target);
    S.cached = out;
    S.cachedFrame = target;
    return true;
}

void SetSymmetricFrustum(bool on)
{
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    g_config.symmetricFrustum = on;
    S.cachedFrame = ~0ull;
    Log("symmetric frustum %s", on ? "on" : "off");
}

int PrepareOverlay(bool on, bool quad, float renderScale, uint32_t w[3], uint32_t h[3])
{
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    if (S.planeStereo) {
        planes::SetStereo(false);
        S.planeStereo = false;
    }
    overlay::LockImage(false);
    S.ovCount = 0;
    if (on && S.bbW && S.bbH) {
        // sizes no game render target has, all different (the bridge tells the overlays apart by size)
        S.ovCount = 1;
        S.ovW[0] = S.bbW + 32;
        S.ovH[0] = S.bbH;
        if (quad) {
            // The engine renders every view at its render resolution (the window's times the 3D
            // resolution scaling) and scales the result to the view's size: the focus views get that
            // resolution, so their narrower field of view has the higher density
            UINT rw = (UINT)((float)S.bbW * renderScale + 0.5f), rh = (UINT)((float)S.bbH * renderScale + 0.5f);
            S.ovW[1] = rw + 16;
            S.ovW[2] = rw + 48;
            S.ovH[1] = S.ovH[2] = rh;
            for (int e = 1; e < 3; e++)
                while (S.ovW[e] == S.ovW[0] && S.ovH[e] == S.ovH[0]) S.ovW[e] += 8;
            S.ovCount = 3;
        }
    }
    for (int i = 0; i < 3; i++) {
        w[i] = i < S.ovCount ? S.ovW[i] : 0;
        h[i] = i < S.ovCount ? S.ovH[i] : 0;
    }
    S.cachedFrame = ~0ull;
    overlay::SetSizes(S.ovCount, S.ovW, S.ovH);
    if (S.ovCount == 3)
        Log("stereo overlays prepared: right eye %ux%u, focus views %ux%u / %ux%u (%.2f x %.2f of the field of view, "
            "render scale %.2f)", S.ovW[0], S.ovH[0], S.ovW[1], S.ovH[1], S.ovW[2], S.ovH[2], g_config.quadFocusWidth,
            g_config.quadFocusHeight, renderScale);
    else
        Log("stereo overlay %s (%ux%u)", S.ovCount ? "prepared" : "off", w[0], h[0]);
    return S.ovCount;
}

bool SetPlaneStereo(bool on)
{
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    if (on && !S.ovCount) return false;
    bool ok = planes::SetStereo(on, S.ovCount);
    // the overlays' own output textures (they rendered by themselves until now) stay the views'
    // images; the further views' pipelines create more targets of those sizes
    overlay::LockImage(on && ok);
    S.planeStereo = on && ok;
    S.cachedFrame = ~0ull;
    return ok;
}

void HeadsetInfo(uint32_t& recW, uint32_t& recH, float& focusW, float& focusH)
{
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    recW = S.recW;
    recH = S.recH;
    focusW = g_config.quadFocusWidth;
    focusH = g_config.quadFocusHeight;
}

void RequestRecenter()
{
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    S.recenterPending = true;
}

bool NextFrameIsSecondEye()
{
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    if (!S.running || !S.lastWasStereo) return false;
    uint64_t next = S.presentCount + (uint64_t)S.lag;
    return !S.planeStereo && IsSecondOfPair(next);
}

bool IsCalibrating()
{
    std::lock_guard<std::recursive_mutex> lock(S.mtx);
    return S.running && S.calibrating && S.readback != nullptr;
}

const char* Status()
{
    return S.status;
}

} // namespace vr
