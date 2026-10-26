#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <vector>

struct Config {
    int anisotropy = 16;
    float lod_bias = 0.0f;
    int fps_limit = 60;
    int min_fps = 8;
    int fog_mode = 0;
    float fog_start = 4096.0f;
    float fog_end = 16384.0f;
    DWORD fog_color = 0x00C8D4E8;
    int frame_latency = 0;
    bool logging = false;
    bool detector = false;
    int toggle_key = VK_SCROLL;
    int detect_key = 0;
};
static Config g_cfg;
static HINSTANCE g_module = nullptr;

// Open only for a log write: no FILE, lock or heap cleanup in DllMain.
static std::wstring g_base_path;
static SRWLOCK g_log_lock = SRWLOCK_INIT;
static void log_write(const char* message) {
    if (!g_cfg.logging || g_base_path.empty()) return;
    AcquireSRWLockExclusive(&g_log_lock);
    FILE* file = _wfopen((g_base_path + L"d3d9_proxy.log").c_str(), L"ab");
    if (file) { fprintf(file, "%s\n", message); fclose(file); }
    ReleaseSRWLockExclusive(&g_log_lock);
}

static std::wstring module_directory(HMODULE module) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        DWORD count = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!count) return {};
        if (count < buffer.size()) {
            std::wstring path(buffer.data(), count);
            auto slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash + 1);
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

static DWORD float_bits(float value) {
    DWORD bits; memcpy(&bits, &value, sizeof(bits)); return bits;
}
static float bits_float(DWORD bits) {
    float value; memcpy(&value, &bits, sizeof(value)); return value;
}
static bool trailing_space_only(const wchar_t* text) {
    while (*text == L' ' || *text == L'\t' || *text == L'\r' || *text == L'\n') ++text;
    return *text == 0;
}
static long parse_integer(const wchar_t* text, long fallback, long low, long high, int base = 10) {
    wchar_t* end = nullptr;
    errno = 0;
    long value = wcstol(text, &end, base);
    if (end == text || !trailing_space_only(end) || errno == ERANGE) return fallback;
    return std::clamp(value, low, high);
}
static float parse_float(const wchar_t* text, float fallback, float low, float high) {
    wchar_t* end = nullptr;
    errno = 0;
    float value = wcstof(text, &end);
    if (end == text || !trailing_space_only(end) || errno == ERANGE || !std::isfinite(value)) return fallback;
    return std::clamp(value, low, high);
}
static int key_name_to_vk(const wchar_t* name) {
    const struct { const wchar_t* name; int code; } keys[] = {
        {L"RightAlt", VK_RMENU}, {L"RightCtrl", VK_RCONTROL},
        {L"Home", VK_HOME}, {L"End", VK_END}, {L"PageUp", VK_PRIOR},
        {L"PageDown", VK_NEXT}, {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE},
        {L"ScrollLock", VK_SCROLL}, {L"Pause", VK_PAUSE}
    };
    for (const auto& key : keys) if (_wcsicmp(name, key.name) == 0) return key.code;
    if (wcslen(name) > 2 && name[0] == L'0' && (name[1] == L'x' || name[1] == L'X')) {
        long value = parse_integer(name, -1, -1, 256, 16);
        if (value > 0 && value < 256) return static_cast<int>(value);
    }
    return 0;
}
static std::wstring ini_string(const std::wstring& path, const wchar_t* section,
                               const wchar_t* key, const wchar_t* fallback) {
    wchar_t value[256] = {};
    GetPrivateProfileStringW(section, key, fallback, value, 256, path.c_str());
    return value;
}
static int ini_int(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                   int fallback, int low, int high) {
    return static_cast<int>(parse_integer(ini_string(path, section, key, L"").c_str(), fallback, low, high));
}
static float ini_float(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                       float fallback, float low, float high) {
    return parse_float(ini_string(path, section, key, L"").c_str(), fallback, low, high);
}
static void config_load(const std::wstring& path) {
    // CREATE_NEW never truncates an existing configuration, including when read access fails.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const char defaults[] =
            "; C4-safe starting profile. Restart after editing.\r\n"
            "[general]\r\nlogging=0\r\n"
            "[fps]\r\n; 0 disables the corresponding cap.\r\nlimit=60\r\nmin_fps=8\r\n"
            "[graphics]\r\n; 0/1: preserve game filtering; 2-16: AF, limited by device caps.\r\n"
            "anisotropy=16\r\n; 0 preserves game LOD. Point/no-mip samplers remain untouched.\r\n"
            "lod_bias=0.0\r\n"
            "[fog]\r\n; 0: game; 1: color+linear distances; 2: game color+linear distances.\r\n"
            "; Both overrides respect game FOGENABLE (UI/sky passes can disable fog).\r\n"
            "mode=0\r\nstart=4096.0\r\nend=16384.0\r\ncolor=0xC8D4E8\r\n"
            "[latency]\r\n; Optional Device9Ex capability only. 0 preserves runtime default.\r\n"
            "; Classic native D3D9 normally does not support this. No GPU-sync fallback.\r\n"
            "frame_latency=0\r\n"
            "[detector]\r\nenabled=0\r\n"
            "[hotkeys]\r\n; Foreground viewport only; empty disables.\r\ntoggle=ScrollLock\r\ndetect=\r\n";
        DWORD written = 0;
        WriteFile(file, defaults, sizeof(defaults) - 1, &written, nullptr);
        CloseHandle(file);
    }
    g_cfg.logging = ini_int(path, L"general", L"logging", 0, 0, 1) != 0;
    g_cfg.detector = ini_int(path, L"detector", L"enabled", 0, 0, 1) != 0;
    g_cfg.fps_limit = ini_int(path, L"fps", L"limit", 60, 0, 1000);
    g_cfg.min_fps = ini_int(path, L"fps", L"min_fps", 8, 0, 120);
    g_cfg.anisotropy = ini_int(path, L"graphics", L"anisotropy", 16, 0, 16);
    g_cfg.lod_bias = ini_float(path, L"graphics", L"lod_bias", 0, -4, 4);
    g_cfg.fog_mode = ini_int(path, L"fog", L"mode", 0, 0, 2);
    g_cfg.fog_start = ini_float(path, L"fog", L"start", 4096, 0, 1000000);
    g_cfg.fog_end = ini_float(path, L"fog", L"end", 16384, 0, 1000000);
    if (g_cfg.fog_end <= g_cfg.fog_start) { g_cfg.fog_start = 4096; g_cfg.fog_end = 16384; }
    g_cfg.fog_color = static_cast<DWORD>(parse_integer(
        ini_string(path, L"fog", L"color", L"0xC8D4E8").c_str(), 0xC8D4E8, 0, 0xFFFFFF, 16));
    g_cfg.frame_latency = ini_int(path, L"latency", L"frame_latency", 0, 0, 16);
    g_cfg.toggle_key = key_name_to_vk(ini_string(path, L"hotkeys", L"toggle", L"ScrollLock").c_str());
    g_cfg.detect_key = key_name_to_vk(ini_string(path, L"hotkeys", L"detect", L"").c_str());
    char text[256];
    snprintf(text, sizeof(text), "Config: fps=%d background=%d AF=%d LOD=%.2f fog=%d latency=%d detector=%d",
        g_cfg.fps_limit, g_cfg.min_fps, g_cfg.anisotropy, g_cfg.lod_bias, g_cfg.fog_mode,
        g_cfg.frame_latency, static_cast<int>(g_cfg.detector));
    log_write(text);
}

static bool window_focused(HWND window) {
    if (!window || !IsWindow(window) || IsIconic(window)) return false;
    HWND foreground = GetForegroundWindow();
    return foreground && GetAncestor(window, GA_ROOT) == GetAncestor(foreground, GA_ROOT);
}
struct KeyEdge {
    bool was_down = false;
    bool was_focused = false;
    bool poll(bool focused, bool down) {
        // A held key on focus gain is not a new press in this viewport.
        bool pressed = focused && was_focused && down && !was_down;
        was_down = down;
        was_focused = focused;
        return pressed;
    }
};

// Per-device QPC timing. A waitable timer sleeps even the final fraction of a
// millisecond; no busy loop and no process-wide timer-resolution modification.
struct FrameLimiter {
    LARGE_INTEGER frequency{}, last{};
    int previous_limit = -1;
    HANDLE timer = nullptr;
    FrameLimiter() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&last);
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~FrameLimiter() { if (timer) CloseHandle(timer); }
    void reset() { QueryPerformanceCounter(&last); previous_limit = -1; }
    void wait(int limit) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (limit <= 0 || previous_limit != limit || frequency.QuadPart <= 0) {
            last = now; previous_limit = limit; return;
        }
        const double budget = static_cast<double>(frequency.QuadPart) / limit;
        double remaining = budget - static_cast<double>(now.QuadPart - last.QuadPart);
        while (remaining > 0) {
            double seconds = remaining / static_cast<double>(frequency.QuadPart);
            LARGE_INTEGER due;
            due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(std::ceil(seconds * 10000000.0)));
            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(timer, INFINITE);
            else Sleep(std::max<DWORD>(1, static_cast<DWORD>(std::ceil(seconds * 1000.0))));
            QueryPerformanceCounter(&now);
            remaining = budget - static_cast<double>(now.QuadPart - last.QuadPart);
        }
        last = now;
    }
};

// Cache requested and effective values separately. Native state blocks always
// see requested values: restore before capture/begin/apply, refresh after apply.
// No shadow state-block mask: the runtime owns the exact D3DSBT_* semantics.
struct GraphicsPolicy {
    struct Value { DWORD desired = 0, effective = 0; bool valid = false; };
    static constexpr D3DSAMPLERSTATETYPE sampler_keys[] = {
        D3DSAMP_MINFILTER, D3DSAMP_MAXANISOTROPY, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MIPFILTER
    };
    static constexpr D3DRENDERSTATETYPE fog_keys[] = {
        D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGVERTEXMODE,
        D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_RANGEFOGENABLE
    };
    Value samplers[16][4]{};
    Value fog[7]{};
    D3DCAPS9 caps{};
    bool dirty = true;
    bool enabled = true;
    bool recording = false;
    static int sampler_index(D3DSAMPLERSTATETYPE type) {
        for (int i = 0; i < 4; ++i) if (sampler_keys[i] == type) return i;
        return -1;
    }
    static int fog_index(D3DRENDERSTATETYPE type) {
        for (int i = 0; i < 7; ++i) if (fog_keys[i] == type) return i;
        return -1;
    }
    void refresh(IDirect3DDevice9* real) {
        real->GetDeviceCaps(&caps);
        for (DWORD s = 0; s < 16; ++s) for (int i = 0; i < 4; ++i) {
            auto& v = samplers[s][i];
            v.valid = SUCCEEDED(real->GetSamplerState(s, sampler_keys[i], &v.desired));
            v.effective = v.desired;
        }
        for (int i = 0; i < 7; ++i) {
            auto& v = fog[i];
            v.valid = SUCCEEDED(real->GetRenderState(fog_keys[i], &v.desired));
            v.effective = v.desired;
        }
        dirty = true;
    }
    HRESULT sync(IDirect3DDevice9* real, bool override_states) {
        if (recording) return D3D_OK;
        HRESULT result = D3D_OK;
        bool active = override_states && enabled;
        const DWORD af = std::min<DWORD>(static_cast<DWORD>(g_cfg.anisotropy), caps.MaxAnisotropy);
        for (DWORD s = 0; s < 16; ++s) {
            auto* values = samplers[s];
            bool filtered = values[0].valid && (values[0].desired == D3DTEXF_LINEAR || values[0].desired == D3DTEXF_ANISOTROPIC);
            bool use_af = active && filtered && af > 1 && (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC);
            for (int i = 0; i < 4; ++i) {
                auto& v = values[i]; if (!v.valid) continue;
                DWORD target = v.desired;
                if (use_af && i == 0) target = D3DTEXF_ANISOTROPIC;
                if (use_af && i == 1) target = af;
                if (active && filtered && values[3].desired != D3DTEXF_NONE && i == 2 &&
                    g_cfg.lod_bias != 0 && (caps.RasterCaps & D3DPRASTERCAPS_MIPMAPLODBIAS)) target = float_bits(g_cfg.lod_bias);
                if (target != v.effective) {
                    HRESULT hr = real->SetSamplerState(s, sampler_keys[i], target);
                    if (SUCCEEDED(hr)) v.effective = target; else result = hr;
                }
            }
        }
        bool use_fog = active && g_cfg.fog_mode > 0 && fog[0].valid && fog[0].desired != FALSE &&
            (caps.RasterCaps & (D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_FOGVERTEX));
        for (int i = 0; i < 7; ++i) {
            auto& v = fog[i]; if (!v.valid) continue;
            DWORD target = v.desired;
            if (use_fog) {
                if (i == 1 && g_cfg.fog_mode == 1) target = g_cfg.fog_color;
                if (i == 2) target = (caps.RasterCaps & D3DPRASTERCAPS_FOGTABLE) ? D3DFOG_LINEAR : D3DFOG_NONE;
                if (i == 3) target = (caps.RasterCaps & D3DPRASTERCAPS_FOGTABLE) ? D3DFOG_NONE : D3DFOG_LINEAR;
                if (i == 4) target = float_bits(g_cfg.fog_start);
                if (i == 5) target = float_bits(g_cfg.fog_end);
                if (i == 6) target = FALSE;
            }
            if (target != v.effective) {
                HRESULT hr = real->SetRenderState(fog_keys[i], target);
                if (SUCCEEDED(hr)) v.effective = target; else result = hr;
            }
        }
        dirty = !override_states || FAILED(result);
        return result;
    }
    void before_draw(IDirect3DDevice9* real) { if (dirty && !recording) sync(real, true); }
    HRESULT restore(IDirect3DDevice9* real) { return sync(real, false); }
};
