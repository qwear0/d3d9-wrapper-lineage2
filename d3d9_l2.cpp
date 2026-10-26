#include "proxy_support.h"

struct DeviceProxy;
struct StateBlockProxy;
struct SwapChainProxy;

// Forward-only methods keep the SDK signatures visible; policy lives in the
// small set of explicitly implemented methods below.
#define HR(name, args, call) HRESULT STDMETHODCALLTYPE name args override { return real->name call; }
#define RET(type, name, args, call) type STDMETHODCALLTYPE name args override { return real->name call; }

struct DeviceProxy final : IDirect3DDevice9 {
    IDirect3DDevice9* real;
    IDirect3D9* parent;
    std::atomic<ULONG> refs{1};
    GraphicsPolicy policy;
    FrameLimiter limiter;
    HWND window;
    KeyEdge toggle_edge, detect_edge;
    UINT original_latency = 0;
    bool latency_supported = false;
    unsigned long long light_calls = 0, draw_calls = 0;
    std::vector<SwapChainProxy*> swap_chains; // weak: each chain owns a device reference

    DeviceProxy(IDirect3DDevice9* device, IDirect3D9* owner, HWND hwnd)
        : real(device), parent(owner), window(hwnd) {
        parent->AddRef();
        policy.refresh(real);
        IDirect3DDevice9Ex* ex = nullptr;
        HRESULT hr = real->QueryInterface(IID_IDirect3DDevice9Ex, reinterpret_cast<void**>(&ex));
        if (SUCCEEDED(hr)) {
            latency_supported = SUCCEEDED(ex->GetMaximumFrameLatency(&original_latency));
            ex->Release();
        }
        char text[160];
        snprintf(text, sizeof(text), "Device: MaxAnisotropy=%lu requestedAF=%d ExLatency=%s QI=0x%08lX",
            policy.caps.MaxAnisotropy, g_cfg.anisotropy, latency_supported ? "supported" : "unsupported", hr);
        log_write(text);
        apply_latency();
    }
    ~DeviceProxy() {
        detector_dump();
        real->Release();
        parent->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDirect3DDevice9) return E_NOINTERFACE;
        *out = static_cast<IDirect3DDevice9*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --refs; if (!count) delete this; return count;
    }
    void detector_dump() {
        if (!g_cfg.detector) return;
        char text[256];
        snprintf(text, sizeof(text), "Detector: draws=%llu SetLight=%llu requestedFog=%lu color=0x%06lX start=%.0f end=%.0f",
            draw_calls, light_calls, policy.fog[0].desired, policy.fog[1].desired,
            bits_float(policy.fog[4].desired), bits_float(policy.fog[5].desired));
        log_write(text);
    }
    void apply_latency() {
        if (!latency_supported || g_cfg.frame_latency == 0) return;
        IDirect3DDevice9Ex* ex = nullptr;
        if (SUCCEEDED(real->QueryInterface(IID_IDirect3DDevice9Ex, reinterpret_cast<void**>(&ex)))) {
            UINT target = policy.enabled ? static_cast<UINT>(g_cfg.frame_latency) : original_latency;
            HRESULT hr = ex->SetMaximumFrameLatency(target);
            ex->Release();
            char text[96]; snprintf(text, sizeof(text), "SetMaximumFrameLatency(%u): 0x%08lX", target, hr);
            log_write(text);
        }
    }
    void set_enabled(bool enabled) {
        if (policy.recording || policy.enabled == enabled) return;
        policy.enabled = enabled;
        policy.dirty = true;
        policy.sync(real, true);
        apply_latency();
        limiter.reset();
        log_write(enabled ? "Enhancements ON" : "Enhancements OFF");
    }
    void poll_keys(HWND target) {
        bool focused = window_focused(target);
        bool toggle = g_cfg.toggle_key && (GetAsyncKeyState(g_cfg.toggle_key) & 0x8000);
        bool detect = g_cfg.detect_key && (GetAsyncKeyState(g_cfg.detect_key) & 0x8000);
        if (toggle_edge.poll(focused, toggle)) set_enabled(!policy.enabled);
        if (detect_edge.poll(focused, detect)) detector_dump();
    }
    void after_present(HWND target) {
        int cap = window_focused(target) ? g_cfg.fps_limit : g_cfg.min_fps;
        limiter.wait(policy.enabled ? cap : 0);
    }
    void before_draw() {
        policy.before_draw(real);
        if (g_cfg.detector) ++draw_calls;
    }
    HRESULT wrap_chain(IDirect3DSwapChain9* chain, IDirect3DSwapChain9** out);

    HR(TestCooperativeLevel, (), ())
    RET(UINT, GetAvailableTextureMem, (), ())
    HR(EvictManagedResources, (), ())
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** out) override {
        if (!out) return D3DERR_INVALIDCALL;
        *out = parent; parent->AddRef(); return S_OK;
    }
    HR(GetDeviceCaps, (D3DCAPS9* p), (p))
    HR(GetDisplayMode, (UINT i, D3DDISPLAYMODE* p), (i,p))
    HR(GetCreationParameters, (D3DDEVICE_CREATION_PARAMETERS* p), (p))
    HR(SetCursorProperties, (UINT x, UINT y, IDirect3DSurface9* s), (x,y,s))
    RET(void, SetCursorPosition, (int x, int y, DWORD f), (x,y,f))
    RET(BOOL, ShowCursor, (BOOL b), (b))
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* p, IDirect3DSwapChain9** out) override {
        if (!out) return D3DERR_INVALIDCALL;
        *out = nullptr;
        IDirect3DSwapChain9* chain = nullptr;
        HRESULT hr = real->CreateAdditionalSwapChain(p, &chain);
        return FAILED(hr) ? hr : wrap_chain(chain, out);
    }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index, IDirect3DSwapChain9** out) override {
        if (!out) return D3DERR_INVALIDCALL;
        *out = nullptr;
        IDirect3DSwapChain9* chain = nullptr;
        HRESULT hr = real->GetSwapChain(index, &chain);
        return FAILED(hr) ? hr : wrap_chain(chain, out);
    }
    RET(UINT, GetNumberOfSwapChains, (), ())
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* p) override {
        HRESULT hr = real->Reset(p);
        if (SUCCEEDED(hr)) {
            policy.recording = false;
            policy.refresh(real);
            if (p && p->hDeviceWindow) window = p->hDeviceWindow;
            limiter.reset();
            toggle_edge = {}; detect_edge = {};
            apply_latency();
        }
        log_write(SUCCEEDED(hr) ? "Reset succeeded" : "Reset failed");
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Present(const RECT* a, const RECT* b, HWND c, const RGNDATA* d) override {
        HWND target = c ? c : window;
        poll_keys(target);
        HRESULT hr = real->Present(a,b,c,d);
        after_present(target);
        return hr;
    }
    HR(GetBackBuffer, (UINT a, UINT b, D3DBACKBUFFER_TYPE c, IDirect3DSurface9** d), (a,b,c,d))
    HR(GetRasterStatus, (UINT a, D3DRASTER_STATUS* b), (a,b))
    HR(SetDialogBoxMode, (BOOL b), (b))
    RET(void, SetGammaRamp, (UINT a, DWORD b, const D3DGAMMARAMP* c), (a,b,c))
    RET(void, GetGammaRamp, (UINT a, D3DGAMMARAMP* b), (a,b))
    HR(CreateTexture, (UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* s), (w,h,l,u,f,p,t,s))
    HR(CreateVolumeTexture, (UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** t, HANDLE* s), (w,h,d,l,u,f,p,t,s))
    HR(CreateCubeTexture, (UINT s, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** t, HANDLE* shared), (s,l,u,f,p,t,shared))
    HR(CreateVertexBuffer, (UINT l, DWORD u, DWORD f, D3DPOOL p, IDirect3DVertexBuffer9** v, HANDLE* s), (l,u,f,p,v,s))
    HR(CreateIndexBuffer, (UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** i, HANDLE* s), (l,u,f,p,i,s))
    HR(CreateRenderTarget, (UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL l, IDirect3DSurface9** s, HANDLE* shared), (w,h,f,m,q,l,s,shared))
    HR(CreateDepthStencilSurface, (UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL d, IDirect3DSurface9** s, HANDLE* shared), (w,h,f,m,q,d,s,shared))
    HR(UpdateSurface, (IDirect3DSurface9* a, const RECT* b, IDirect3DSurface9* c, const POINT* d), (a,b,c,d))
    HR(UpdateTexture, (IDirect3DBaseTexture9* a, IDirect3DBaseTexture9* b), (a,b))
    HR(GetRenderTargetData, (IDirect3DSurface9* a, IDirect3DSurface9* b), (a,b))
    HR(GetFrontBufferData, (UINT a, IDirect3DSurface9* b), (a,b))
    HR(StretchRect, (IDirect3DSurface9* a, const RECT* b, IDirect3DSurface9* c, const RECT* d, D3DTEXTUREFILTERTYPE f), (a,b,c,d,f))
    HR(ColorFill, (IDirect3DSurface9* a, const RECT* b, D3DCOLOR c), (a,b,c))
    HR(CreateOffscreenPlainSurface, (UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** s, HANDLE* shared), (w,h,f,p,s,shared))
    HR(SetRenderTarget, (DWORD i, IDirect3DSurface9* s), (i,s))
    HR(GetRenderTarget, (DWORD i, IDirect3DSurface9** s), (i,s))
    HR(SetDepthStencilSurface, (IDirect3DSurface9* s), (s))
    HR(GetDepthStencilSurface, (IDirect3DSurface9** s), (s))
    HR(BeginScene, (), ())
    HR(EndScene, (), ())
    HR(Clear, (DWORD a, const D3DRECT* b, DWORD c, D3DCOLOR d, float e, DWORD f), (a,b,c,d,e,f))
    HR(SetTransform, (D3DTRANSFORMSTATETYPE a, const D3DMATRIX* b), (a,b))
    HR(GetTransform, (D3DTRANSFORMSTATETYPE a, D3DMATRIX* b), (a,b))
    HR(MultiplyTransform, (D3DTRANSFORMSTATETYPE a, const D3DMATRIX* b), (a,b))
    HR(SetViewport, (const D3DVIEWPORT9* p), (p))
    HR(GetViewport, (D3DVIEWPORT9* p), (p))
    HR(SetMaterial, (const D3DMATERIAL9* m), (m))
    HR(GetMaterial, (D3DMATERIAL9* m), (m))
    HRESULT STDMETHODCALLTYPE SetLight(DWORD i, const D3DLIGHT9* light) override {
        if (g_cfg.detector) ++light_calls;
        return real->SetLight(i,light);
    }
    HR(GetLight, (DWORD i, D3DLIGHT9* l), (i,l))
    HR(LightEnable, (DWORD i, BOOL b), (i,b))
    HR(GetLightEnable, (DWORD i, BOOL* b), (i,b))
    HR(SetClipPlane, (DWORD i, const float* p), (i,p))
    HR(GetClipPlane, (DWORD i, float* p), (i,p))
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE type, DWORD value) override {
        HRESULT hr = real->SetRenderState(type,value);
        int i = GraphicsPolicy::fog_index(type);
        if (SUCCEEDED(hr) && !policy.recording && i >= 0) {
            policy.fog[i] = {value, value, true}; policy.dirty = true;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE type, DWORD* value) override {
        HRESULT hr = real->GetRenderState(type,value);
        int i = GraphicsPolicy::fog_index(type);
        if (SUCCEEDED(hr) && i >= 0 && policy.fog[i].valid) *value = policy.fog[i].desired;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** out) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override {
        if (policy.recording) return D3DERR_INVALIDCALL;
        HRESULT hr = policy.restore(real);
        if (FAILED(hr)) return hr;
        hr = real->BeginStateBlock();
        if (SUCCEEDED(hr)) policy.recording = true;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** out) override;
    HR(SetClipStatus, (const D3DCLIPSTATUS9* c), (c))
    HR(GetClipStatus, (D3DCLIPSTATUS9* c), (c))
    HR(GetTexture, (DWORD s, IDirect3DBaseTexture9** t), (s,t))
    HR(SetTexture, (DWORD s, IDirect3DBaseTexture9* t), (s,t))
    HR(GetTextureStageState, (DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD* v), (s,t,v))
    HR(SetTextureStageState, (DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v), (s,t,v))
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE type, DWORD* value) override {
        HRESULT hr = real->GetSamplerState(s,type,value);
        int i = GraphicsPolicy::sampler_index(type);
        if (SUCCEEDED(hr) && s < 16 && i >= 0 && policy.samplers[s][i].valid) *value = policy.samplers[s][i].desired;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE type, DWORD value) override {
        HRESULT hr = real->SetSamplerState(s,type,value);
        int i = GraphicsPolicy::sampler_index(type);
        if (SUCCEEDED(hr) && !policy.recording && s < 16 && i >= 0) {
            policy.samplers[s][i] = {value, value, true}; policy.dirty = true;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override {
        policy.before_draw(real); return real->ValidateDevice(passes);
    }
    HR(SetPaletteEntries, (UINT p, const PALETTEENTRY* e), (p,e))
    HR(GetPaletteEntries, (UINT p, PALETTEENTRY* e), (p,e))
    HR(SetCurrentTexturePalette, (UINT p), (p))
    HR(GetCurrentTexturePalette, (UINT* p), (p))
    HR(SetScissorRect, (const RECT* r), (r))
    HR(GetScissorRect, (RECT* r), (r))
    HR(SetSoftwareVertexProcessing, (BOOL b), (b))
    RET(BOOL, GetSoftwareVertexProcessing, (), ())
    HR(SetNPatchMode, (float n), (n))
    RET(float, GetNPatchMode, (), ())
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE t, UINT a, UINT b) override { before_draw(); return real->DrawPrimitive(t,a,b); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT d, UINT e) override { before_draw(); return real->DrawIndexedPrimitive(t,a,b,c,d,e); }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT c, const void* d, UINT s) override { before_draw(); return real->DrawPrimitiveUP(t,c,d,s); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void* d, D3DFORMAT e, const void* f, UINT g) override { before_draw(); return real->DrawIndexedPrimitiveUP(t,a,b,c,d,e,f,g); }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT a, UINT b, UINT c, IDirect3DVertexBuffer9* d, IDirect3DVertexDeclaration9* e, DWORD f) override { before_draw(); return real->ProcessVertices(a,b,c,d,e,f); }
    HR(CreateVertexDeclaration, (const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** d), (e,d))
    HR(SetVertexDeclaration, (IDirect3DVertexDeclaration9* d), (d))
    HR(GetVertexDeclaration, (IDirect3DVertexDeclaration9** d), (d))
    HR(SetFVF, (DWORD f), (f))
    HR(GetFVF, (DWORD* f), (f))
    HR(CreateVertexShader, (const DWORD* f, IDirect3DVertexShader9** s), (f,s))
    HR(SetVertexShader, (IDirect3DVertexShader9* s), (s))
    HR(GetVertexShader, (IDirect3DVertexShader9** s), (s))
    HR(SetVertexShaderConstantF, (UINT r, const float* d, UINT c), (r,d,c))
    HR(GetVertexShaderConstantF, (UINT r, float* d, UINT c), (r,d,c))
    HR(SetVertexShaderConstantI, (UINT r, const int* d, UINT c), (r,d,c))
    HR(GetVertexShaderConstantI, (UINT r, int* d, UINT c), (r,d,c))
    HR(SetVertexShaderConstantB, (UINT r, const BOOL* d, UINT c), (r,d,c))
    HR(GetVertexShaderConstantB, (UINT r, BOOL* d, UINT c), (r,d,c))
    HR(SetStreamSource, (UINT n, IDirect3DVertexBuffer9* v, UINT o, UINT s), (n,v,o,s))
    HR(GetStreamSource, (UINT n, IDirect3DVertexBuffer9** v, UINT* o, UINT* s), (n,v,o,s))
    HR(SetStreamSourceFreq, (UINT n, UINT d), (n,d))
    HR(GetStreamSourceFreq, (UINT n, UINT* d), (n,d))
    HR(SetIndices, (IDirect3DIndexBuffer9* i), (i))
    HR(GetIndices, (IDirect3DIndexBuffer9** i), (i))
    HR(CreatePixelShader, (const DWORD* f, IDirect3DPixelShader9** s), (f,s))
    HR(SetPixelShader, (IDirect3DPixelShader9* s), (s))
    HR(GetPixelShader, (IDirect3DPixelShader9** s), (s))
    HR(SetPixelShaderConstantF, (UINT r, const float* d, UINT c), (r,d,c))
    HR(GetPixelShaderConstantF, (UINT r, float* d, UINT c), (r,d,c))
    HR(SetPixelShaderConstantI, (UINT r, const int* d, UINT c), (r,d,c))
    HR(GetPixelShaderConstantI, (UINT r, int* d, UINT c), (r,d,c))
    HR(SetPixelShaderConstantB, (UINT r, const BOOL* d, UINT c), (r,d,c))
    HR(GetPixelShaderConstantB, (UINT r, BOOL* d, UINT c), (r,d,c))
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT h, const float* s, const D3DRECTPATCH_INFO* i) override { before_draw(); return real->DrawRectPatch(h,s,i); }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT h, const float* s, const D3DTRIPATCH_INFO* i) override { before_draw(); return real->DrawTriPatch(h,s,i); }
    HR(DeletePatch, (UINT h), (h))
    HR(CreateQuery, (D3DQUERYTYPE t, IDirect3DQuery9** q), (t,q))
};

struct StateBlockProxy final : IDirect3DStateBlock9 {
    IDirect3DStateBlock9* real;
    DeviceProxy* owner;
    std::atomic<ULONG> refs{1};
    StateBlockProxy(IDirect3DStateBlock9* block, DeviceProxy* device) : real(block), owner(device) { owner->AddRef(); }
    ~StateBlockProxy() { real->Release(); owner->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDirect3DStateBlock9) return E_NOINTERFACE;
        *out = static_cast<IDirect3DStateBlock9*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG count = --refs; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** out) override {
        if (!out) return D3DERR_INVALIDCALL;
        *out = owner; owner->AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Capture() override {
        if (owner->policy.recording) return D3DERR_INVALIDCALL;
        HRESULT hr = owner->policy.restore(owner->real);
        return FAILED(hr) ? hr : real->Capture();
    }
    HRESULT STDMETHODCALLTYPE Apply() override {
        if (owner->policy.recording) return D3DERR_INVALIDCALL;
        HRESULT hr = owner->policy.restore(owner->real);
        if (FAILED(hr)) return hr;
        hr = real->Apply();
        // Read back even on a failed Apply: do not assume atomic driver failure.
        owner->policy.refresh(owner->real);
        return hr;
    }
};

HRESULT DeviceProxy::CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** out) {
    if (!out || policy.recording) return D3DERR_INVALIDCALL;
    *out = nullptr;
    HRESULT hr = policy.restore(real);
    if (FAILED(hr)) return hr;
    IDirect3DStateBlock9* block = nullptr;
    hr = real->CreateStateBlock(type, &block);
    if (FAILED(hr)) return hr;
    *out = new (std::nothrow) StateBlockProxy(block, this);
    if (!*out) { block->Release(); return E_OUTOFMEMORY; }
    return hr;
}
HRESULT DeviceProxy::EndStateBlock(IDirect3DStateBlock9** out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    IDirect3DStateBlock9* block = nullptr;
    HRESULT hr = real->EndStateBlock(&block);
    if (FAILED(hr)) return hr;
    policy.recording = false;
    policy.dirty = true;
    *out = new (std::nothrow) StateBlockProxy(block, this);
    if (!*out) { block->Release(); return E_OUTOFMEMORY; }
    return hr;
}

struct SwapChainProxy final : IDirect3DSwapChain9 {
    IDirect3DSwapChain9* real;
    DeviceProxy* owner;
    std::atomic<ULONG> refs{1};
    SwapChainProxy(IDirect3DSwapChain9* chain, DeviceProxy* device) : real(chain), owner(device) { owner->AddRef(); }
    ~SwapChainProxy() {
        auto& list = owner->swap_chains;
        list.erase(std::remove(list.begin(), list.end(), this), list.end());
        real->Release(); owner->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDirect3DSwapChain9) return E_NOINTERFACE;
        *out = static_cast<IDirect3DSwapChain9*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG count = --refs; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Present(const RECT* a, const RECT* b, HWND c, const RGNDATA* d, DWORD flags) override {
        D3DPRESENT_PARAMETERS pp{};
        real->GetPresentParameters(&pp);
        HWND target = c ? c : (pp.hDeviceWindow ? pp.hDeviceWindow : owner->window);
        owner->poll_keys(target);
        HRESULT hr = real->Present(a,b,c,d,flags);
        // Respect the explicit non-blocking Present contract.
        if (!(flags & D3DPRESENT_DONOTWAIT)) owner->after_present(target);
        return hr;
    }
    HR(GetFrontBufferData, (IDirect3DSurface9* surface), (surface))
    HR(GetBackBuffer, (UINT i, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** out), (i,type,out))
    HR(GetRasterStatus, (D3DRASTER_STATUS* status), (status))
    HR(GetDisplayMode, (D3DDISPLAYMODE* mode), (mode))
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** out) override {
        if (!out) return D3DERR_INVALIDCALL;
        *out = owner; owner->AddRef(); return S_OK;
    }
    HR(GetPresentParameters, (D3DPRESENT_PARAMETERS* p), (p))
};
HRESULT DeviceProxy::wrap_chain(IDirect3DSwapChain9* chain, IDirect3DSwapChain9** out) {
    for (auto* proxy : swap_chains) if (proxy->real == chain) {
        proxy->AddRef(); chain->Release(); *out = proxy; return S_OK;
    }
    auto* proxy = new (std::nothrow) SwapChainProxy(chain, this);
    if (!proxy) { chain->Release(); return E_OUTOFMEMORY; }
    try { swap_chains.push_back(proxy); }
    catch (...) { proxy->Release(); return E_OUTOFMEMORY; }
    *out = proxy; return S_OK;
}

struct D3DProxy final : IDirect3D9 {
    IDirect3D9* real;
    std::atomic<ULONG> refs{1};
    explicit D3DProxy(IDirect3D9* runtime) : real(runtime) {}
    ~D3DProxy() { real->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDirect3D9) return E_NOINTERFACE;
        *out = static_cast<IDirect3D9*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG count = --refs; if (!count) delete this; return count; }
    HR(RegisterSoftwareDevice, (void* p), (p))
    RET(UINT, GetAdapterCount, (), ())
    HR(GetAdapterIdentifier, (UINT a, DWORD b, D3DADAPTER_IDENTIFIER9* c), (a,b,c))
    RET(UINT, GetAdapterModeCount, (UINT a, D3DFORMAT b), (a,b))
    HR(EnumAdapterModes, (UINT a, D3DFORMAT b, UINT c, D3DDISPLAYMODE* d), (a,b,c,d))
    HR(GetAdapterDisplayMode, (UINT a, D3DDISPLAYMODE* b), (a,b))
    HR(CheckDeviceType, (UINT a, D3DDEVTYPE b, D3DFORMAT c, D3DFORMAT d, BOOL e), (a,b,c,d,e))
    HR(CheckDeviceFormat, (UINT a, D3DDEVTYPE b, D3DFORMAT c, DWORD d, D3DRESOURCETYPE e, D3DFORMAT f), (a,b,c,d,e,f))
    HR(CheckDeviceMultiSampleType, (UINT a, D3DDEVTYPE b, D3DFORMAT c, BOOL d, D3DMULTISAMPLE_TYPE e, DWORD* f), (a,b,c,d,e,f))
    HR(CheckDepthStencilMatch, (UINT a, D3DDEVTYPE b, D3DFORMAT c, D3DFORMAT d, D3DFORMAT e), (a,b,c,d,e))
    HR(CheckDeviceFormatConversion, (UINT a, D3DDEVTYPE b, D3DFORMAT c, D3DFORMAT d), (a,b,c,d))
    HR(GetDeviceCaps, (UINT a, D3DDEVTYPE b, D3DCAPS9* c), (a,b,c))
    RET(HMONITOR, GetAdapterMonitor, (UINT a), (a))
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                            D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) override {
        if (!out) return D3DERR_INVALIDCALL;
        *out = nullptr;
        IDirect3DDevice9* device = nullptr;
        // PUREDEVICE disables getters required to preserve requested state.
        HRESULT hr = real->CreateDevice(adapter, type, focus, flags & ~D3DCREATE_PUREDEVICE, pp, &device);
        if (FAILED(hr)) return hr;
        auto* proxy = new (std::nothrow) DeviceProxy(device, this, pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus);
        if (!proxy) { device->Release(); return E_OUTOFMEMORY; }
        *out = proxy;
        D3DADAPTER_IDENTIFIER9 identifier{};
        if (SUCCEEDED(real->GetAdapterIdentifier(adapter, 0, &identifier))) log_write(identifier.Description);
        return hr;
    }
};
#undef HR
#undef RET

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static HMODULE g_runtime = nullptr;
static IDirect3D9* (WINAPI* g_create9)(UINT) = nullptr;
static BOOL CALLBACK initialize(PINIT_ONCE, PVOID, PVOID*) {
    try {
        g_base_path = module_directory(g_module);
        if (!g_base_path.empty()) config_load(g_base_path + L"d3d9_proxy.ini");
        wchar_t directory[32768]{};
        UINT size = GetSystemDirectoryW(directory, 32768);
        if (!size || size >= 32768) return FALSE;
        std::wstring path = std::wstring(directory, size) + L"\\d3d9.dll";
        HMODULE runtime = LoadLibraryW(path.c_str());
        if (!runtime) { log_write("ERROR: cannot load system d3d9"); return FALSE; }
        auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(runtime, "Direct3DCreate9"));
        if (!create) { FreeLibrary(runtime); return FALSE; }
        // A single backend module reference is intentionally retained until process
        // exit: raw D3D resources may outlive wrappers. Never FreeLibrary in DllMain.
        g_runtime = runtime; g_create9 = create;
        log_write("Backend: system-directory d3d9.dll (no local DLL chaining)");
        return TRUE;
    } catch (...) { return FALSE; }
}
static bool ensure_initialized() { return InitOnceExecuteOnce(&g_once, initialize, nullptr, nullptr) != FALSE; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_module = instance;
    return TRUE;
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    if (!ensure_initialized()) return nullptr;
    IDirect3D9* real = g_create9(sdk);
    if (!real) return nullptr;
    auto* proxy = new (std::nothrow) D3DProxy(real);
    if (!proxy) real->Release();
    return proxy;
}

// Forward diagnostics to the native implementation rather than silently stubbing.
extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    if (!ensure_initialized()) return -1;
    auto fn = reinterpret_cast<int (WINAPI*)(D3DCOLOR,LPCWSTR)>(GetProcAddress(g_runtime,"D3DPERF_BeginEvent"));
    return fn ? fn(color,name) : -1;
}
extern "C" int WINAPI D3DPERF_EndEvent() {
    if (!ensure_initialized()) return -1;
    auto fn = reinterpret_cast<int (WINAPI*)()>(GetProcAddress(g_runtime,"D3DPERF_EndEvent"));
    return fn ? fn() : -1;
}
extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
    if (!ensure_initialized()) return;
    auto fn = reinterpret_cast<void (WINAPI*)(D3DCOLOR,LPCWSTR)>(GetProcAddress(g_runtime,"D3DPERF_SetMarker"));
    if (fn) fn(color,name);
}
extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
    if (!ensure_initialized()) return;
    auto fn = reinterpret_cast<void (WINAPI*)(D3DCOLOR,LPCWSTR)>(GetProcAddress(g_runtime,"D3DPERF_SetRegion"));
    if (fn) fn(color,name);
}
extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame() {
    if (!ensure_initialized()) return FALSE;
    auto fn = reinterpret_cast<BOOL (WINAPI*)()>(GetProcAddress(g_runtime,"D3DPERF_QueryRepeatFrame"));
    return fn ? fn() : FALSE;
}
extern "C" void WINAPI D3DPERF_SetOptions(DWORD options) {
    if (!ensure_initialized()) return;
    auto fn = reinterpret_cast<void (WINAPI*)(DWORD)>(GetProcAddress(g_runtime,"D3DPERF_SetOptions"));
    if (fn) fn(options);
}
extern "C" DWORD WINAPI D3DPERF_GetStatus() {
    if (!ensure_initialized()) return 0;
    auto fn = reinterpret_cast<DWORD (WINAPI*)()>(GetProcAddress(g_runtime,"D3DPERF_GetStatus"));
    return fn ? fn() : 0;
}
