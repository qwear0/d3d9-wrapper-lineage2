// Tests include the production implementation so assertions can compare desired
// state with actual runtime state. No game process, input injection or client I/O.
#include "../d3d9_l2.cpp"

static int checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); return 1; } } while (0)
#define OK(expr) CHECK(SUCCEEDED(expr))

static int unit_tests() {
    CHECK(parse_integer(L"junk", 60, 0, 1000) == 60);
    CHECK(parse_integer(L"-4", 60, 0, 1000) == 0);
    CHECK(parse_integer(L"9999", 60, 0, 1000) == 1000);
    CHECK(parse_integer(L"99999999999999999999", 60, 0, 1000) == 60);
    CHECK(parse_integer(L"60junk", 8, 0, 1000) == 8);
    CHECK(parse_integer(L" 60 \t", 8, 0, 1000) == 60);
    CHECK(parse_float(L"nan", 0, -4, 4) == 0);
    CHECK(parse_float(L"inf", 0, -4, 4) == 0);
    CHECK(parse_float(L"-100", 0, -4, 4) == -4);
    CHECK(parse_float(L"1.25garbage", 0, -4, 4) == 0);
    CHECK(parse_float(L"1.25", 0, -4, 4) == 1.25f);
    CHECK(bits_float(float_bits(-1.25f)) == -1.25f);
    CHECK(key_name_to_vk(L"ScrollLock") == VK_SCROLL);
    CHECK(key_name_to_vk(L"rightalt") == VK_RMENU);
    CHECK(key_name_to_vk(L"0x77") == 0x77);
    CHECK(key_name_to_vk(L"0x100") == 0);
    CHECK(key_name_to_vk(L"0x77oops") == 0);
    CHECK(key_name_to_vk(L"") == 0);
    KeyEdge key;
    CHECK(!key.poll(false, true));
    CHECK(!key.poll(true, true)); // held on focus gain
    CHECK(!key.poll(true, false));
    CHECK(key.poll(true, true));
    CHECK(!key.poll(true, true));
    CHECK(!key.poll(false, false));
    CHECK(!key.poll(false, true));
    CHECK(!key.poll(true, true));
    CHECK(!window_focused(nullptr));

    wchar_t temp[MAX_PATH]{};
    CHECK(GetTempPathW(MAX_PATH, temp) > 0);
    std::wstring directory = std::wstring(temp) + L"l2.proxy.tests.\u0442\u0435\u0441\u0442." + std::to_wstring(GetCurrentProcessId());
    CHECK(CreateDirectoryW(directory.c_str(), nullptr) != FALSE);
    g_base_path = directory + L"\\";
    std::wstring path = g_base_path + L"d3d9_proxy.ini";
    config_load(path);
    CHECK(g_cfg.lod_bias == 0 && g_cfg.frame_latency == 0 && g_cfg.fog_mode == 0);
    CHECK(g_cfg.toggle_key == VK_SCROLL && g_cfg.fps_limit == 60);
    CHECK(WritePrivateProfileStringW(L"graphics",L"anisotropy",L"99",path.c_str()));
    CHECK(WritePrivateProfileStringW(L"graphics",L"lod_bias",L"nan",path.c_str()));
    CHECK(WritePrivateProfileStringW(L"fps",L"limit",L"-3",path.c_str()));
    CHECK(WritePrivateProfileStringW(L"fps",L"min_fps",L"999",path.c_str()));
    CHECK(WritePrivateProfileStringW(L"fog",L"start",L"1000",path.c_str()));
    CHECK(WritePrivateProfileStringW(L"fog",L"end",L"10",path.c_str()));
    CHECK(WritePrivateProfileStringW(L"latency",L"frame_latency",L"123",path.c_str()));
    CHECK(WritePrivateProfileStringW(L"general",L"logging",L"1",path.c_str()));
    CHECK(WritePrivateProfileStringW(L"custom",L"preserve",L"yes",path.c_str()));
    config_load(path);
    CHECK(g_cfg.anisotropy == 16 && g_cfg.lod_bias == 0);
    CHECK(g_cfg.fps_limit == 0 && g_cfg.min_fps == 120 && g_cfg.frame_latency == 16);
    CHECK(g_cfg.fog_start == 4096 && g_cfg.fog_end == 16384);
    CHECK(ini_string(path,L"custom",L"preserve",L"") == L"yes");
    CHECK(GetFileAttributesW((g_base_path + L"d3d9_proxy.log").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(DeleteFileW((g_base_path + L"d3d9_proxy.log").c_str()));
    CHECK(DeleteFileW(path.c_str()));
    CHECK(RemoveDirectoryW(directory.c_str()));
    g_cfg = Config{}; g_base_path.clear();

    FrameLimiter limiter;
    limiter.wait(1000); // prime
    LARGE_INTEGER start{}, end{}, frequency{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (int i = 0; i < 5; ++i) limiter.wait(1000);
    QueryPerformanceCounter(&end);
    double elapsed = static_cast<double>(end.QuadPart-start.QuadPart) / static_cast<double>(frequency.QuadPart);
    CHECK(elapsed >= 0.0045); // the old <=1ms gap skipped all waits
    limiter.wait(0);
    limiter.wait(8); // changed cap must not wait on a stale foreground timestamp
    CHECK(limiter.previous_limit == 8);
    printf("Unit/config checks passed: %d (five 1ms waits: %.3f ms)\n",checks,elapsed*1000);
    return 0;
}

static DWORD sampler(IDirect3DDevice9* d, D3DSAMPLERSTATETYPE type, DWORD stage = 0) {
    DWORD v = 0xFFFFFFFF; d->GetSamplerState(stage,type,&v); return v;
}
static DWORD fog(IDirect3DDevice9* d, D3DRENDERSTATETYPE type) {
    DWORD v = 0xFFFFFFFF; d->GetRenderState(type,&v); return v;
}

static int native_tests() {
    g_cfg = Config{};
    wchar_t system[MAX_PATH]{};
    CHECK(GetSystemDirectoryW(system, MAX_PATH) != 0);
    HMODULE module = LoadLibraryW((std::wstring(system) + L"\\d3d9.dll").c_str());
    CHECK(module != nullptr);
    auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(module,"Direct3DCreate9"));
    CHECK(create != nullptr);
    IDirect3D9* raw = create(D3D_SDK_VERSION);
    if (!raw) { puts("SKIP: native D3D9 unavailable"); FreeLibrary(module); return 77; }
    auto* parent = new D3DProxy(raw);
    void* queried = nullptr;
    OK(parent->QueryInterface(IID_IUnknown,&queried)); CHECK(queried == parent);
    static_cast<IUnknown*>(queried)->Release();
    OK(parent->QueryInterface(IID_IDirect3D9,&queried)); CHECK(queried == parent);
    static_cast<IUnknown*>(queried)->Release();
    CHECK(parent->QueryInterface(IID_IDirect3D9Ex,&queried) == E_NOINTERFACE && queried == nullptr);
    CHECK(parent->QueryInterface(IID_IUnknown,nullptr) == E_POINTER);

    HWND window = CreateWindowExW(0,L"STATIC",L"D3D9 proxy tests",WS_OVERLAPPEDWINDOW,
        0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window != nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window;
    pp.BackBufferWidth=640; pp.BackBufferHeight=480;
    IDirect3DDevice9* device = nullptr;
    HRESULT hr = parent->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device);
    if (FAILED(hr)) {
        printf("SKIP: native HAL CreateDevice 0x%08lX\n",hr);
        parent->Release(); DestroyWindow(window); FreeLibrary(module); return 77;
    }
    auto* proxy = static_cast<DeviceProxy*>(device);
    printf("Native caps: max AF=%lu; Ex latency=%d\n",proxy->policy.caps.MaxAnisotropy,proxy->latency_supported);
    OK(device->QueryInterface(IID_IUnknown,&queried)); CHECK(queried == device);
    static_cast<IUnknown*>(queried)->Release();
    OK(device->QueryInterface(IID_IDirect3DDevice9,&queried)); CHECK(queried == device);
    static_cast<IUnknown*>(queried)->Release();
    CHECK(device->QueryInterface(IID_IDirect3DDevice9Ex,&queried) == E_NOINTERFACE && queried == nullptr);
    IDirect3D9* got_parent = nullptr;
    OK(device->GetDirect3D(&got_parent)); CHECK(got_parent == parent); got_parent->Release();

    // Real filtering changes only at a draw boundary; getters preserve requested values.
    OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));
    OK(device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
    for (int level : {0,1}) {
        g_cfg.anisotropy = level; proxy->policy.dirty = true; proxy->before_draw();
        CHECK(sampler(proxy->real,D3DSAMP_MINFILTER) == D3DTEXF_LINEAR);
    }
    g_cfg.anisotropy = 16; proxy->policy.dirty = true; proxy->before_draw();
    bool af = proxy->policy.caps.MaxAnisotropy > 1 && (proxy->policy.caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC);
    CHECK(sampler(proxy->real,D3DSAMP_MINFILTER) == static_cast<DWORD>(af ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR));
    CHECK(sampler(device,D3DSAMP_MINFILTER) == D3DTEXF_LINEAR);
    CHECK(sampler(proxy->real,D3DSAMP_MIPFILTER) == D3DTEXF_NONE);
    if (af) CHECK(sampler(proxy->real,D3DSAMP_MAXANISOTROPY) == std::min<DWORD>(16,proxy->policy.caps.MaxAnisotropy));
    OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));
    proxy->before_draw(); CHECK(sampler(proxy->real,D3DSAMP_MINFILTER) == D3DTEXF_POINT);

    OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));
    OK(device->SetSamplerState(0,D3DSAMP_MIPMAPLODBIAS,float_bits(0.75f)));
    g_cfg.lod_bias = -1;
    proxy->before_draw(); // no mipmap request: leave requested bias alone
    CHECK(sampler(proxy->real,D3DSAMP_MIPMAPLODBIAS) == float_bits(0.75f));
    OK(device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_LINEAR));
    proxy->before_draw();
    if (proxy->policy.caps.RasterCaps & D3DPRASTERCAPS_MIPMAPLODBIAS)
        CHECK(sampler(proxy->real,D3DSAMP_MIPMAPLODBIAS) == float_bits(-1));
    OK(device->SetRenderState(D3DRS_FOGENABLE,TRUE));
    OK(device->SetRenderState(D3DRS_FOGCOLOR,0x112233));
    proxy->set_enabled(false);
    CHECK(fog(proxy->real,D3DRS_FOGENABLE) == TRUE);
    CHECK(sampler(proxy->real,D3DSAMP_MINFILTER) == D3DTEXF_LINEAR);
    CHECK(sampler(proxy->real,D3DSAMP_MIPMAPLODBIAS) == float_bits(0.75f));
    proxy->set_enabled(true);

    // Mode 1 locks color and mode at draw time, but preserves game fog enable.
    g_cfg.fog_mode=1;
    OK(device->SetRenderState(D3DRS_FOGCOLOR,0x445566));
    OK(device->SetRenderState(D3DRS_FOGTABLEMODE,D3DFOG_EXP));
    OK(device->SetRenderState(D3DRS_FOGSTART,float_bits(100)));
    proxy->before_draw();
    CHECK(fog(proxy->real,D3DRS_FOGCOLOR) == g_cfg.fog_color);
    CHECK(fog(device,D3DRS_FOGCOLOR) == 0x445566);
    CHECK(fog(proxy->real,D3DRS_FOGSTART) == float_bits(g_cfg.fog_start));
    g_cfg.fog_mode=2; proxy->policy.dirty=true; proxy->before_draw();
    CHECK(fog(proxy->real,D3DRS_FOGCOLOR) == 0x445566);
    OK(device->SetRenderState(D3DRS_FOGENABLE,FALSE)); proxy->before_draw();
    CHECK(fog(proxy->real,D3DRS_FOGENABLE) == FALSE);
    CHECK(fog(proxy->real,D3DRS_FOGSTART) == float_bits(100));
    OK(device->SetRenderState(D3DRS_FOGENABLE,TRUE)); proxy->before_draw();
    proxy->set_enabled(false);
    CHECK(fog(proxy->real,D3DRS_FOGSTART) == float_bits(100));
    CHECK(fog(proxy->real,D3DRS_FOGCOLOR) == 0x445566);
    CHECK(fog(proxy->real,D3DRS_FOGTABLEMODE) == D3DFOG_EXP);
    g_cfg.fog_mode=0; g_cfg.lod_bias=0;
    proxy->set_enabled(true);

    // Runtime owns exact ALL/PIXEL/VERTEX capture masks. None capture overrides.
    for (auto type : {D3DSBT_ALL,D3DSBT_PIXELSTATE,D3DSBT_VERTEXSTATE}) {
        OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR)); proxy->before_draw();
        IDirect3DStateBlock9* state=nullptr;
        OK(device->CreateStateBlock(type,&state));
        OK(state->QueryInterface(IID_IUnknown,&queried)); CHECK(queried==state);
        static_cast<IUnknown*>(queried)->Release();
        IDirect3DDevice9* got=nullptr; OK(state->GetDevice(&got)); CHECK(got==device); got->Release();
        proxy->set_enabled(false);
        OK(state->Apply()); proxy->before_draw();
        CHECK(sampler(proxy->real,D3DSAMP_MINFILTER) == D3DTEXF_LINEAR);
        proxy->set_enabled(true); proxy->before_draw();
        OK(state->Capture());
        proxy->set_enabled(false); OK(state->Apply()); proxy->before_draw();
        CHECK(sampler(proxy->real,D3DSAMP_MINFILTER) == D3DTEXF_LINEAR);
        state->Release(); proxy->set_enabled(true);
    }
    // Custom recording must not change live desired state or capture unrelated overrides.
    OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR)); proxy->before_draw();
    OK(device->BeginStateBlock());
    CHECK(device->BeginStateBlock()==D3DERR_INVALIDCALL);
    OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));
    CHECK(sampler(device,D3DSAMP_MINFILTER)==D3DTEXF_LINEAR);
    IDirect3DStateBlock9* custom=nullptr;
    OK(device->EndStateBlock(&custom));
    proxy->before_draw();
    CHECK(sampler(device,D3DSAMP_MINFILTER)==D3DTEXF_LINEAR);
    OK(custom->Apply()); proxy->before_draw();
    CHECK(sampler(device,D3DSAMP_MINFILTER)==D3DTEXF_POINT);
    CHECK(sampler(proxy->real,D3DSAMP_MINFILTER)==D3DTEXF_POINT);
    custom->Release();

    // Partial block applies only its own states, not a stale snapshot of all tracked states.
    OK(device->BeginStateBlock());
    OK(device->SetRenderState(D3DRS_FOGCOLOR,0xAABBCC));
    OK(device->EndStateBlock(&custom));
    OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));
    OK(custom->Apply()); proxy->before_draw();
    CHECK(sampler(device,D3DSAMP_MINFILTER)==D3DTEXF_LINEAR);
    CHECK(fog(device,D3DRS_FOGCOLOR)==0xAABBCC);
    custom->Release();

    // Capture refreshes only the runtime-defined custom mask; later Apply keeps
    // unrelated states and must not restore old effective AF.
    OK(device->BeginStateBlock());
    OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));
    OK(device->EndStateBlock(&custom));
    OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR)); proxy->before_draw();
    OK(custom->Capture());
    OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));
    OK(device->SetRenderState(D3DRS_FOGCOLOR,0x123456));
    proxy->set_enabled(false);
    OK(custom->Apply()); proxy->before_draw();
    CHECK(sampler(proxy->real,D3DSAMP_MINFILTER)==D3DTEXF_LINEAR);
    CHECK(fog(proxy->real,D3DRS_FOGCOLOR)==0x123456);
    custom->Release(); proxy->set_enabled(true);
    // Cap-limited and unsupported-AF policy paths, independent of installed GPU.
    DWORD original_af=proxy->policy.caps.MaxAnisotropy;
    DWORD original_filters=proxy->policy.caps.TextureFilterCaps;
    proxy->policy.caps.MaxAnisotropy=2;
    proxy->policy.dirty=true; proxy->before_draw();
    if (af) CHECK(sampler(proxy->real,D3DSAMP_MAXANISOTROPY)==2);
    proxy->policy.caps.TextureFilterCaps &= ~D3DPTFILTERCAPS_MINFANISOTROPIC;
    proxy->policy.dirty=true; proxy->before_draw();
    CHECK(sampler(proxy->real,D3DSAMP_MINFILTER)==D3DTEXF_LINEAR);
    proxy->policy.caps.MaxAnisotropy=original_af;
    proxy->policy.caps.TextureFilterCaps=original_filters;
    proxy->policy.dirty=true;

    IDirect3DSwapChain9 *chain=nullptr,*same=nullptr;
    OK(device->GetSwapChain(0,&chain)); OK(device->GetSwapChain(0,&same)); CHECK(chain==same);
    same->Release();
    IDirect3DDevice9* chain_device=nullptr;
    OK(chain->GetDevice(&chain_device)); CHECK(chain_device==device); chain_device->Release();
    OK(chain->QueryInterface(IID_IDirect3DSwapChain9,&queried)); CHECK(queried==chain);
    static_cast<IUnknown*>(queried)->Release();
    chain->Release();
    CHECK(proxy->swap_chains.empty());

    // Explicit depth creation now has no sidecar texture or retained depth pointer.
    IDirect3DSurface9* depth=nullptr;
    OK(device->CreateDepthStencilSurface(640,480,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,TRUE,&depth,nullptr));
    OK(device->SetDepthStencilSurface(depth));
    OK(device->SetDepthStencilSurface(nullptr)); depth->Release();
    proxy->real->AddRef(); CHECK(proxy->real->Release()==1);
    OK(device->Reset(&pp));
    CHECK(!proxy->policy.recording);
    CHECK(sampler(device,D3DSAMP_MINFILTER)==D3DTEXF_POINT);

    // Execute actual draw/Present, not just the policy helper.
    struct Vertex { float x,y,z,rhw; DWORD color; };
    const Vertex triangle[] = {{10,10,0,1,0xFFFFFFFF},{100,10,0,1,0xFFFFFFFF},{10,100,0,1,0xFFFFFFFF}};
    OK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    OK(device->SetRenderState(D3DRS_LIGHTING,FALSE));
    OK(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
    OK(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));
    OK(device->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0));
    OK(device->BeginScene());
    OK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,triangle,sizeof(Vertex)));
    OK(device->EndScene());
    CHECK(sampler(proxy->real,D3DSAMP_MINFILTER)==static_cast<DWORD>(af ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR));
    g_cfg.fps_limit=0; g_cfg.min_fps=0;
    OK(device->Present(nullptr,nullptr,nullptr,nullptr));
    OK(device->GetSwapChain(0,&chain));
    OK(chain->Present(nullptr,nullptr,nullptr,nullptr,0)); chain->Release();
    D3DPRESENT_PARAMETERS extra=pp;
    OK(device->CreateAdditionalSwapChain(&extra,&chain));
    OK(chain->GetDevice(&chain_device)); CHECK(chain_device==device); chain_device->Release();
    OK(chain->Present(nullptr,nullptr,nullptr,nullptr,0)); chain->Release();

    // Block lifetime keeps the proxy alive after the application's device release.
    OK(device->CreateStateBlock(D3DSBT_ALL,&custom));
    CHECK(device->Release()==1);
    IDirect3DDevice9* held=nullptr;
    OK(custom->GetDevice(&held)); CHECK(held==device);
    CHECK(custom->Release()==0);
    CHECK(held->Release()==0);
    CHECK(parent->Release()==0);
    DestroyWindow(window); FreeLibrary(module);
    printf("Native regression checks passed: %d\n",checks);
    return 0;
}
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    if (strcmp(argv[1],"--unit")==0) return unit_tests();
    if (strcmp(argv[1],"--native")==0) return native_tests();
    return 2;
}
