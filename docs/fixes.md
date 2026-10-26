# C4 correctness fixes — 2026-10-26

The reviewed baseline is `031df45`. Its root `d3d9.dll` is deliberately unchanged; rebuild to obtain the fixed implementation.

## Finding disposition

| Review | Change | Regression coverage |
|---|---|---|
| F-001 MSVC build/exports | CMake Win32, static runtime, narrow x86 .def; removed SDK `small` collision | /W4 /WX Release/Debug builds, actual PE export/import inspection and DLL load |
| F-002 COM escape | IUnknown/base QI returns wrapper; own refs; wrapped parent; unknown/Ex rejected | QI identity, null output, parent identity, device held by state block |
| F-003 depth lifetime | Removed unused readable-depth allocations and retained depth surface | Explicit depth create/bind/unbind/release leaves native device count at 1, final proxy release=0 |
| F-004 disable/OFF | Desired/effective states; preserve point/mip filters; cap AF; restore exact game state | AF 0/1, AF caps/unsupported path, POINT/NONE, nonzero game LOD, stock fog, toggles |
| F-005 state blocks | Wrap blocks; restore desired state before Create/Capture/Begin/Apply; refresh after Apply; defer overrides until draw | ALL/PIXEL/VERTEX, custom Begin/End, Capture, partial masks, OFF Apply, recording live-state isolation |
| F-006 fog order | Pre-draw policy; mode 1 color lock; mode 2 game color; both preserve game fog enable | Game writes after enable, color/distance/disable, restoration and no fog work in Present |
| F-007 latency claim | Default off; capability and result diagnostics; restore original latency on OFF; removed fictional FPS fallback | Native unsupported path exercised; documentation corrected. Supported Ex path still needs an Ex-capable backend test |
| F-008 global hotkeys | Per-device key edges gated by foreground viewport; ignore held key on focus gain; ScrollLock default | Deterministic background/focus/hold/press transitions; no synthetic desktop input |

## Additional changes

- Unicode module/config/log paths, dynamic module path buffer, dedicated log filename. Existing config creation uses CREATE_NEW, never truncating on a read failure.
- Strict finite numeric parsing, bounded AF/FPS/LOD/fog/latency/key settings, fallback on malformed or overflowing values, valid fog distance order.
- Config logging is available during loading. Detector output occurs on device release or configured key, not under loader lock.
- DllMain only records the module handle. Lazy runtime loading uses InitOnce and publishes the function pointer only after complete success. No loader-lock FreeLibrary.
- System runtime module reference deliberately retained until process exit, to cover native resources surviving wrapper release. No global timer-resolution modification.
- Per-device QPC/waitable-timer limiter, no busy-spin or <=1 ms gap; timing resets on cap changes/Reset/toggle. Exact scheduling accuracy is not promised.
- Swap chains wrapped, identity preserved while referenced; GetDevice and Present stay on the proxy path. DONOTWAIT is not delayed.
- Float state packing uses memcpy rather than strict-aliasing violations.
- D3DPERF functions forward to the native runtime rather than stub success.
- No window-title rewriting; no game config/launcher edits; no speculative rendering/depth features.
- Conservative C4 profile, CI and explicit installation/rollback documentation.

## Implementation choices

The runtime is the source of truth for state-block masks. Before capture or recording, only modified effective states are restored to their desired values. Recording writes pass through unchanged and do not alter the live desired cache. Apply first restores live desired states, invokes native Apply, then refreshes the cache; the next draw reapplies policy. This avoids an incomplete hardcoded list of D3DSBT_PIXELSTATE/VERTEXSTATE masks. Overrides are not written while recording.

The texture mip filter is never forced to LINEAR. AF changes only LINEAR/ANISOTROPIC minification. LOD override excludes point/no-mip samplers. Fog override preserves FOGENABLE, intentionally revising the original force-enable behavior to avoid applying fog to explicitly disabled passes.

PUREDEVICE is removed on creation because runtime state getters are required. The target is a classic single-render-thread C4 client, not arbitrary multi-threaded D3D9 software.

## Validation and remaining limits

Local native Windows tests passed in Release and Debug with MSVC 14.44 and Windows SDK 10.0.26100.0. Latest test suite contains 50 unit/config checks, 151 native-device checks, plus exported-DLL smoke checks. Native checks execute an actual draw, device and swap-chain Present, Reset and resource teardown in a disposable hidden window. No game process was started.

Not established by these tests:

- Actual C4 visual correctness, protection acceptance, long sessions, fullscreen lost-device recovery, physical multi-window hotkeys and siege frametimes.
- Supported Ex latency behavior, Wine/DXVK or third-party overlay compatibility.
- Full resource-interface transparency: textures/surfaces/buffers/shaders/queries are still native objects, and their GetDevice may expose the native device. Calls made through that pointer bypass policy. This is explicitly documented, not represented as a full-COM interception fix.
- Concurrent state mutation on one device. Wrapper state caches assume the target's render-thread serialization.

Build outputs are in `build/Release`; the installation package is generated with `cmake --install build --config Release --prefix out/c4`. `D:/Games/L2Siege` remains untouched. The old review harness in ignored `tmp/` describes the baseline and is superseded by `tests/`.
