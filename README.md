[Русская версия](README_RU.md)

# Direct3D 9 wrapper for Lineage II C4

A small **32-bit Windows** proxy for classic `IDirect3D9` clients. This fork prioritizes correct state restoration and a reproducible native Windows build.

> **Do not install the old `d3d9.dll` tracked in the repository root.** It is the unchanged upstream binary and does not contain these fixes. Build and use `out/c4/d3d9.dll` (or a CI artifact from the matching source revision).

The inspected `L2.exe` and `D3DDrv.dll` in `D:\Games\L2Siege\system` are x86 and import `Direct3DCreate9`. Native D3D9 regression tests pass on the development machine. **Actual C4 gameplay, server protection, Wine/DXVK and third-party overlays have not been validated.** Upstream testing on Kamael is not a C4 compatibility guarantee.

## Build and test

Requirements: Visual Studio 2022 with Desktop development with C++, an x86 Windows SDK toolchain and CMake >=3.21. The project deliberately rejects x64 and unvalidated toolchains.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix out/c4
```

- DLL: `out/c4/d3d9.dll`; conservative configuration: `out/c4/d3d9_proxy.ini`.
- Static MSVC runtime; explicit eight-symbol x86 export list; warnings treated as errors.
- Tests cover parsing, Unicode/dotted configuration paths, focus key edges, sub-millisecond-budget waiting, native COM identity/lifetime, state restoration, AF/LOD/fog policy, explicit/custom state blocks, reset, depth release, swap chains, drawing/Present and the actual DLL exports/load.
- Native device test reports **skipped**, not passed, when HAL is unavailable (e.g. a headless runner). GPU tests require an interactive desktop. No tests start L2 or write to its directory.
- GitHub Actions builds, tests and packages the same Win32 target.

## Installation / rollback

1. Close all processes using the target client. Check server rules concerning graphics wrappers; do not bypass protection if rejected.
2. Back up any existing `system\d3d9.dll` and `system\d3d9_proxy.ini` **before copying**. A DXVK/ReShade proxy with the same name cannot be silently replaced or chained.
3. Copy the two files from `out/c4` into `D:\Games\L2Siege\system`, next to `L2.exe`. Preserve an existing custom INI instead of overwriting it blindly.
4. For the first smoke test set `[general] logging=1`; launch normally, including through the existing borderless launcher. The append-only log is `system\d3d9_proxy.log`.
5. Test login, UI/chat, world/town, teleport, Alt-Tab, Alt-Enter, minimize/restore, toggle, background cap, resolution changes and clean exit. Turn logging off afterward.
6. Roll back with the game closed: restore the backed-up DLL/INI, or remove only the newly installed wrapper files if none existed. Do not touch the Windows runtime.

The build/install commands above write only to the repository's build/package folders, not to the game. `Option.ini`, encrypted `l2.ini`, player settings and launch scripts are never modified by the wrapper.

## Behavior and configuration

See [`config/c4/d3d9_proxy.ini`](config/c4/d3d9_proxy.ini). Missing config is created next to the loaded DLL without truncating an existing file. Changes require a restart. Invalid/nonfinite numbers fall back to defaults; numeric ranges are bounded.

| Setting | Default | Meaning |
|---|---:|---|
| `general.logging` | 0 | Append startup/capability and toggle diagnostics |
| `fps.limit` | 60 | Foreground cap, 0 disables, maximum 1000 |
| `fps.min_fps` | 8 | Background/minimized cap, 0 disables, maximum 120 |
| `graphics.anisotropy` | 16 | 0/1 preserves game AF; 2–16 clamped to device capabilities |
| `graphics.lod_bias` | 0.0 | 0 preserves requested game LOD; override range -4..4 |
| `fog.mode` | 0 | 0 game; 1 configured color/linear distances; 2 game color/linear distances |
| `latency.frame_latency` | 0 | Optional Device9Ex capability, 0 preserves default, maximum 16 |
| `detector.enabled` | 0 | Draw/light counters and requested fog dump; needs logging |
| `hotkeys.toggle` | ScrollLock | Foreground viewport only; empty disables |
| `hotkeys.detect` | empty | Dump detector; named key or hex VK |

### Filtering and OFF

AF changes linear/anisotropic minification only; deliberate **point filtering and the game's mip filter remain unchanged**. Nonzero LOD override is applied only to filtered, mipmapped samplers with capability support. Negative bias may cause shimmer, so the C4 preset uses zero.

Desired game states are kept separately from effective overrides. OFF restores requested values, not hardcoded defaults. Getter methods report requested values. State-block capture, recording and apply operate on game state; overrides are applied at the next draw, not baked into saved state blocks. Standard block masks are handled by the D3D runtime itself. There are no unused depth allocations.

### Fog

Both override modes **respect the game's FOGENABLE**: UI/sky passes that disable fog remain unfogged. This intentionally differs from upstream's forced-enable behavior. Mode 1 changes color and linear distances, mode 2 keeps the game color. Relevant states are applied before draws, never after rendering in Present. Table fog falls back to vertex fog where supported; unsupported fog devices keep game state. Leave mode=0 until verified in the actual client.

### FPS, latency and hotkeys

The cap uses per-device QPC timestamps and a waitable timer, including the final fractional millisecond. It does not busy-spin or change system-wide timer resolution. Scheduler delays still affect accuracy; **no 0.1 ms precision guarantee** is made. Foreground and background cap transitions reset timing. Swap-chain Present is covered; explicit DONOTWAIT is not delayed. OFF disables caps along with graphics overrides.

FPS limiting is **not GPU queue-depth synchronization**. `SetMaximumFrameLatency` is attempted only when requested and when the underlying device exposes Device9Ex and its original value can be read/restored. Classic native CreateDevice returned E_NOINTERFACE on the test machine. Unsupported requests are visible with logging=1; there is no hidden synchronization fallback or promise of reduced input lag. The proxy does not convert classic devices to Ex (managed-pool compatibility matters).

Hotkeys are per device, require the foreground viewport and ignore a key held during focus gain. They do not rewrite the game's window title. Named keys: RightAlt, RightCtrl, Home, End, PageUp, PageDown, Insert, Delete, ScrollLock, Pause; or hex VK such as `0x77`.

## Architecture and boundaries

- Lazy initialization outside DllMain; Unicode module-relative paths; absolute system-directory backend loading. A backend module reference is intentionally kept until process exit because raw resources may outlive proxies.
- Wrapped base interfaces preserve IUnknown identity and own reference counts. Device GetDirect3D, state-block/swap-chain GetDevice return proxies. Unknown/Ex interface requests return E_NOINTERFACE instead of exposing an unwrapped device.
- `D3DCREATE_PUREDEVICE` is cleared: requested-state preservation needs runtime getters.
- D3DPERF exports forward to the system runtime. No network access, injection into another process or game-memory patches.
- **Not a general-purpose full D3D9 interposer:** textures, surfaces, buffers, shaders and queries are native objects. Their GetDevice methods can expose a raw device; code rendering through that raw pointer bypasses policy. No resource-wrapper layer or arbitrary DLL chaining is claimed. Likewise concurrent state mutation of one device is not supported; the target is the legacy single-render-thread C4 client.
- AF improves texture filtering, not CPU-bound siege performance. Measure identical scenes/frametimes rather than assuming an FPS increase.

Based on [d3d9-depth-proxy](https://github.com/guglovich/d3d9-depth-proxy). The original repository review is retained locally under `tmp/reports/2026-10-26_Review/repository.md`; implementation notes are in [`docs/fixes.md`](docs/fixes.md).
