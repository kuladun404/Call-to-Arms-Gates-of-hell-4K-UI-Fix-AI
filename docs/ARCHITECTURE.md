# Architecture and findings

Technical notes on how the `d3d11.dll` proxy scales Call to Arms – Gates of Hell. Written while developing the mod; useful if the game is patched.

Source: `src/` — `proxy.c` (loading, exports, config, vtable patch helper), `scale_d3d.c` (rendering), `scale_input.c` (User32/window), `common.h`; `build.py` generates `out/gen/exports.inc` + `stubs.S`.

## What the exe uses
Imports `d3d11!D3D11CreateDeviceAndSwapChain`, `dxgi!CreateDXGIFactory`, `GFSDK_SSAO_D3D11.win64.dll` (NVIDIA HBAO+), `WaveWorks.win64.dll` (water), `d3dcompiler_47`. Input: window messages + RawInput (relative, untouched) + User32 calls listed below.

## Proxy mechanics
- Loads `%SystemRoot%\System32\d3d11.dll`; every export forwards via asm `jmp *g_real[i]`; `.drectve` `-export:Name=stub_i`. `D3D11CreateDevice(AndSwapChain)` are implemented in C.
- Note: two modules named `d3d11.dll` are loaded; `GetModuleHandle(L"d3d11.dll")` may return the system one — use the path saved in DllMain (`g_dir`).

## Logical vs physical
`g_lw/g_lh` = what the game sees (1920×1080); physical = ×`scale`. Game starts with a 1360×768 windowed swapchain, then `SetFullscreenState` + `ResizeTarget(1920×1080)` (scaled ×2) + `ResizeBuffers(0,0)` → logical recomputed from the real desc.
- Swapchain: desc ×2 on create, `ResizeBuffers/ResizeTarget` ×2, `GetDesc` ÷2, back buffer marked.
- `CreateTexture2D`: scale ×2 when (a) size matches screen ÷1..16 and bind RT/DS/UAV or STAGING, or (b) `scale_ui_rt`: RT **or DS**, DEFAULT, non-square, not pow2×pow2, ≤4096 (UI widget caches, 3D unit preview 858×348 + its depth, the fixed 1920×1080 pair created while still at 1360×768). Scaled resources carry private data GUID `MARK`; their `GetDesc` reports logical size. RT and DS bound together must be scaled together, or D3D11 refuses the binding and the pass silently disappears.
- Viewports/scissors: stored logical per context; applied ×2 while the bound target is marked; `RSGet*` return logical.

## Biggest non-obvious finding: wrap contexts, don't patch them
D3D11 objects here keep their vtable inline (`*(void**)obj == obj+8`) and the runtime **rewrites slots itself** (Draw/RS*/OM* entries switch with state). Patching context vtables works for a while and then silently stops; immediate and deferred contexts also have different implementations. Fix: return COM wrappers (`CtxWrap {vtbl, real, ref}`) from `D3D11CreateDevice*`, `GetImmediateContext[1-3]`, `CreateDeferredContext[1-3]`. The wrapper vtable has `ID3D11DeviceContext4Vtbl` slots: generated asm thunks (`mov 8(%rcx),%rcx; mov (%rcx),%rax; jmp *8i(%rax)`) for everything, C hooks for QI/AddRef/Release, RS*, OM*, ClearState, Copy*, Map/Unmap, UpdateSubresource, Draw*/Dispatch (dump), Execute/FinishCommandList. QI for context IIDs returns the wrapper; immediate wrapper is a singleton. Device/texture/swapchain slots are stable enough for `vt_patch` (per-object patch + slot map when originals differ).

The game records the 3D scene in **deferred contexts** and plays it with `ExecuteCommandList` — without wrapping them the scene lands in the top-left quarter.

## Screen constants in constant buffers
Post-effects compute UV as `SV_Position * (1/W,1/H)` from constants → with ×2 targets you get a shrunken semi-transparent "ghost" of the scene in the top-left quarter plus clamp streaks. `scan_cb=1` logs pairs (W,H), (1/W,1/H), (0.5/W), (W/2), (4/W)... found in CB writes. `cb_fix=<sizes in floats>` rewrites those pairs to physical values in CBs of the listed sizes (both `UpdateSubresource` copy and `Map`→`Unmap`):
- 156 floats — scene/post constants
- 84 floats — HBAO+ (`InvQuarterResolution`, `InvFullResolution`)
- **Do not** touch the UI projection CB (232 floats, W/2,H/2 at [8]) or the UI shrinks.
After a game patch, rerun with `scan_cb=1 log=1` and check sizes.

## Input (`scale_input.c`)
IAT hooks in the exe only: `GetCursorPos ÷2, SetCursorPos ×2, ClipCursor, ScreenToClient/ClientToScreen/MapWindowPoints` (×2 → real → ÷2), `GetClientRect/GetWindowRect/GetMonitorInfoW ÷2`, `GetSystemMetrics` (screen sizes ÷2), `WindowFromPoint/MonitorFromRect ×2`, `SetWindowPos/CreateWindowExW ×2` (all windows, including the GDI splash `GoH_SPLASH_WND`), `AdjustWindowRect`, `EnumDisplaySettingsW(current) ÷2`, `RegisterClassExW` (wraps WndProc), `Set/GetWindowLongPtrW(GWLP_WNDPROC)`, `DefWindowProcW` (gets original lParam back). The wndproc wrapper halves lParam of mouse/NC/size/move/displaychange messages.
- **Splash** (`GoH_SPLASH_WND`, 1200×612, drawn with GDI BitBlt/StretchBlt/FillRect/TextOutW at the size from `GetClientRect`): the window is ×2 and `BeginPaint`/`GetDC` on it set `GM_ADVANCED` + world transform ×2 (reset in `EndPaint`/`ReleaseDC`), `rcPaint` ÷2, `InvalidateRect` ×2. Leaving the splash unscaled while `GetClientRect` is ÷2 drew the picture in the top-left quarter with black elsewhere. Needs `-lgdi32`.
- **Cursor**: the game builds 32×32 hardware cursors with `CreateIconIndirect`; the hook scales the color (32-bit, nearest) and mask bitmaps and the hotspot ×2 → 64×64 (`LoadImageW` cursors ×2 too). `SetCursor` logs each new cursor's size. A hardware cursor never shows in screenshots — measure it with `GetCursorInfo`+`GetIconInfo`.

## Steam overlay
`GameOverlayRenderer64.dll` draws through the same device and wrapped context. Given logical sizes it rendered ×2 on top of its own DPI scaling (huge). `from_overlay(__builtin_return_address(0))` detects calls from its module: swapchain `GetDesc`, texture `GetDesc` and `RSSet/GetViewports`, `RSSet/GetScissorRects` pass through unscaled.

## Diagnostics
- `log=2` logs unscaled RT/DS creations ("keep") and unique viewport combos.
- Create `ui4k.dump` beside the DLL while the game renders: the next frame's OM/RS/Draw/Execute calls are logged between `dump begin/end`. The game must be in the foreground (minimised fullscreen doesn't Present).
- Symptoms → causes: scene in a quarter = unscaled viewport (unwrapped context); ghost/streaks = CB screen constants; a pass missing = RT/DS size mismatch; wrong mouse = an unhooked User32 path.

## Sharp fonts (`font_hd.c`, `tools/make_fonts.py`)
The game loads BMFont page TGAs as empty `B8G8R8A8` textures (mips 1, bind 0 or SRV) and fills them with `UpdateSubresource`; DDS UI images use BC formats with initial data instead. Textures of page sizes (256², 512×256, 512², 2048×1024 with `font_big=1`, default on) are created ×2 and marked. On upload, the FNV-1a 64 hash of the BGRA rows selects `ui4k_fonts\<hash>.raw`, a ×2 page with the same layout. If no file matches, the upload is upscaled bilinearly and `ui4k_fonts\<hash>.pat` is applied if present: a patch with only the sharp glyph rectangles (format in `font_hd.c`), used for the huge `main_*` pages (Arial Unicode, 32 MB each at ×2) so the folder stays ~95 MB. The log says `replaced`, `patched by` or `has no replacement (upscaled)` per page; to identify a hash, FNV-hash the TGAs in the paks (some hits are UI skins or fx textures, not fonts, which is harmless). UVs are normalised, so metrics stay the same and text stays the same size.

`tools/make_fonts.py` builds the pages from the game's `interface.pak` into `out/ui4k_fonts`; `build.py --install` copies them. Every glyph is redrawn as a vector outline at ×2, fitted to the 1080p glyph (`ttf_fit.fit`), which is the reference for cell, position, ink edges and ink mass; nothing is a bitmap upscale.
- **Outline sources**, all fitted the same way, best per glyph by `err + 0.5·(1−corr) + KIND_BIAS`: `vector` from a font file of the same face (`FONTS` map: Windows Arial/Impact, OFL replacements in `tools/fonts`: Oswald, Open Sans, PT Sans Narrow, Roboto Condensed/Mono, Source Sans 3 for Myriad Pro, Anka Coder C87, TeX Gyre Bonum for Bookman; variable-font weight chosen by minimal error); `trace` = outline traced from the largest game atlas of the same face, taken only when it beats the font by 0.012 (e.g. Anka's slashed `0` vs the game's plain one); `self` = traced from the glyph itself (rare symbols, Pobeda which has no free font), tried with several isoline thresholds. Mixing trace and vector freely made stroke weight uneven within a word, hence the bias.
- **Fit**: the outline is kept as a signed distance field (scipy EDT, downsampled to the target size); rendering is a 1-px ramp on the field averaged SS×SS (4, or 2 for glyphs >40 px). Coordinate descent on the 4 ink-box edges minimises pixel error + `EDGE_W`×edge shift (`_edges`, subpixel); stroke weight matches ink mass by **offsetting the contour** (SDF threshold), not gamma — gamma left a grey halo. Acceptance `(err≤0.075, corr≥0.90)` or `(err≤0.13, corr≥0.97)`.
- **Gotchas**: some .fnt share one page (`roboto_con_15/15b`, `9/9b`, `main_12/15`, `roboto_mono_10/12`) — write the best run once, or the second overwrites; cid `0xFFFFFFFF` is BMFont's missing-glyph box, not `?`. Builds run in a process pool (all cores, 2048×1024 jobs in a separate pool of 8 with `max_tasks_per_child=1`); keep `master`/SDF caches small or 24 workers run out of RAM. Full build ~2.6 min. GPU was considered and not worth it (tiny sequential per-glyph searches).
- **Check**: `tools/check_fonts.py [regex] [--dir folder]` compares every glyph downscaled with the original (error, ink-edge shift > 0.25 px, grey-edge share); `tools/preview_fonts.py out.png a.fnt …` renders before/after. State of v1.0: 56 047 glyphs, mean error 0.0424, >0.1: 1 113, edge shift >0.25 px: 1 431. Rerun after game patches (page hashes change).

## Known limits
Very small sizes (8–9 px) carry most residual error: 1080p hinting distorts them, the vector glyph matches size and mass but not every hinted pixel. Multiplayer should be safe by design (the exe and CRC-checked files are untouched), but it has not been verified.
