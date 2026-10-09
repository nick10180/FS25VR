#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>

// Mouse pointer drawn into the copied image (the game uses the OS cursor, which is not in it).
struct CursorDraw {
    bool  visible = false;
    float x = 0, y = 0;   // tip position in source pixels
    float unit = 1;       // pixels per arrow unit (the arrow is ~12x19 units)
};

// Where a source is drawn over the target instead of filling it (quad views: the focus view over
// the eye's image), in target pixels, with a blended edge (fraction of the rect, 0 = hard).
struct BlitRect {
    float x = 0, y = 0, w = 0, h = 0;
    float smoothing = 0;
};

// A flat panel in space drawn over an eye's image: a part of the source texture on a rectangle given
// in the eye's space (metres, -z forward, +y up).
struct PanelDraw {
    float tanLeft = -1, tanRight = 1, tanUp = 1, tanDown = -1;  // the eye image's frustum
    float centre[3] = {0, 0, -1};     // the rectangle's centre
    float axisU[3] = {0.5f, 0, 0};    // from the centre to its right edge
    float axisV[3] = {0, 0.5f, 0};    // from the centre to its top edge
    float uv[4] = {0, 0, 1, 1};       // the part of the source it shows (u0, v0, u1, v1; v down)
    float mark[4] = {};               // outline colour (rgb) and strength; 0 = none
    float border[4] = {};             // outline width as a share of the half width / height, fill strength, plain area (1)
};

// Draws a game backbuffer into an OpenXR swapchain image with a fullscreen triangle.
// The source holds display-referred (gamma encoded) values; when the target RTV is an
// *_SRGB format the shader decodes them first so the hardware re-encode is lossless.
class Blitter {
public:
    bool Init(ID3D12Device* device, DXGI_FORMAT dstFormat);
    void Shutdown();

    // Records the draw. 'src' must be in PIXEL_SHADER_RESOURCE state, the RTV in RENDER_TARGET.
    void Record(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, DXGI_FORMAT srcFormat,
                D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT dstW, UINT dstH, const CursorDraw* cursor = nullptr,
                const BlitRect* over = nullptr);

    // Draws 'src' (PIXEL_SHADER_RESOURCE) as a panel over the target.
    void RecordPanel(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, DXGI_FORMAT srcFormat,
                     D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT dstW, UINT dstH, const PanelDraw& panel);

    bool Ready() const { return m_pso != nullptr; }

private:
    D3D12_GPU_DESCRIPTOR_HANDLE SrvFor(ID3D12Resource* src, DXGI_FORMAT srcFormat);

    ID3D12Device*         m_device = nullptr;
    ID3D12RootSignature*  m_root = nullptr;
    ID3D12PipelineState*  m_pso = nullptr;
    ID3D12PipelineState*  m_psoBlend = nullptr;
    ID3D12PipelineState*  m_psoPanel = nullptr;
    ID3D12DescriptorHeap* m_srvHeap = nullptr;
    UINT                  m_srvInc = 0;
    static constexpr UINT kSrvRing = 512;
    UINT                  m_srvNext = 0;
    ID3D12Resource*       m_srvLastRes = nullptr;   // the source of the newest view (reused for the same)
    DXGI_FORMAT           m_srvLastFormat = DXGI_FORMAT_UNKNOWN;
    bool                  m_decodeSrgb = false;
};

bool IsSrgbFormat(DXGI_FORMAT f);
