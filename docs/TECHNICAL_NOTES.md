# Technical notes

How the mod works, which engine addresses it uses, and which approaches were tried and failed,
so whoever continues does not repeat them. All addresses are RVAs into `LikeADragon8.exe`
(image base `0x140000000`). Everything here was established by runtime measurement unless it
is marked as a hypothesis.

## Target

- *Like a Dragon: Infinite Wealth*, Steam, internal codename `elvis`. Dragon Engine, D3D12 only
  (`ID3D12Device10`, Agility SDK in the game folder, feature level 12_1). SteamStub + Denuvo.
- The executable is `<install>\runtime\media\LikeADragon8.exe`, not in the install root.
- Build the addresses belong to: file size **291,158,048** bytes, SHA-256
  `C2AE08CA80C97B72723E1D3558B5A18636E3C0B02577512B5285F3F967C71FB6`, PE timestamp
  `0x67A1A56F`. Nothing in the add-on checks this. Adding a check (hash the PE header or the
  bytes at each breakpoint address before arming) is the first thing to do before anyone runs
  this on a patched game.
- Denuvo makes on-disk code analysis unreliable (code is unpacked at runtime), and PIX and
  RenderDoc do not attach. All reverse engineering below was done live: hardware breakpoints,
  real stack unwinding, and code ranges dumped from the running process (`tools/`).

## Architecture

```
LikeADragon8.exe
  └─ dxgi.dll  = ReShade 6.7.1 with full add-on support (installed by the user)
       └─ LAD8VR.addon64  (src/addon/lad8vr_stereo.cpp, this mod)
            ├─ F9: arm. Saves the engine eye transforms, then starts a worker that installs
            │      four execute breakpoints (Dr0-Dr3) on every game thread every 250 ms
            ├─ VEH: breakpoint 1 writes activation mode 1 before the dual-view initializer runs;
            │       breakpoints 2-4 open the identity-2 "skip" branches for the second eye
            ├─ F10: writes ±0.032 into the X translation of the two engine eye transforms
            └─ ReShade draw events: learn which pipelines are the HUD, re-issue each HUD draw
                   into both halves of the side-by-side frame
```

The add-on never modifies engine code bytes. It changes `RIP` in a trapped thread context,
and writes two engine data locations (the activation dword and the eye transforms). There is
no in-game disarm; when the add-on unloads it restores both locations.

### Key addresses

| Purpose | RVA |
|---|---|
| Activation mode dword (0 normal, 1 dual view, 2 dual view + extra built-in transform) | `0x48EB584` |
| Current engine eye index (0 or 1) | `0x48EB588` |
| Two 4×4 eye transforms, 64 bytes each, row-major (translation X at float index 12) | `0x48EB590` |
| Current view-group pointer (the gate input) | `0x48EB3B8` |
| Current group index | `0x48EB3C0` |
| Eight-group array base pointer, stride `0x13F0` | `0x48EB540` |
| Native dual-view initializer (breakpoint 1) | `0x27FDCB0` |
| Eye-0 / eye-1 engine callbacks | `0x27F5B90` / `0x27F5B50` |
| Mode-1 eye latch / view composition | `0x2806F30` |
| Two-viewport pass builder (calls a half-width helper twice) | `0x2868FA0` |
| Job dispatcher (parent of most producer chains) | `0x27E7AE9` |
| Linked-command insertion sequence (measurement anchor) | `0x2818325` |
| Pass table owner | `0x48EB380` |
| Per-pass coordinator / virtual pass-builder call | `0x27F6210` / `0x27F6303` |
| Shared per-pass cb12 update/copy | `0x2842B30` |
| Linked render-command playback | `0x27F7320`..`0x27F7430` |
| Indexed-draw emission | `0xDDB40`..`0xDDC0C` |

View group layout: `+0x00` leading identity (eye 0 → 1, eye 1 → 2), `+0x04` enable flags,
`+0x08` 64-bit pass mask, `+0x20` seven view slots of stride `0x2D0`.

### Activating the dormant dual view

A plain write of `1` to the activation dword does nothing: the engine clears it before the
initializer reads it. Instead, an execute breakpoint on the initializer's first instruction
(`0x27FDCB0`) writes `1` just before it runs. The unmodified engine then does all group setup,
eye-callback installation, view composition and viewport routing itself. A manual clone of view
group 0 into group 1 crashes, because it skips initializer work that lives outside the group.

The side-by-side split is not viewport-based: every scene draw is full width on the full-size
target, and the engine bakes the half-screen mapping into each eye's projection. The engine
composes the eye transforms at `0x48EB590` with the authored camera before draw submission, so
the F10 offset is true camera parallax, not a screen-space shift.

Mode 2 also activates cleanly but does not fix static submission, and its extra transform work
adds unwanted camera motion. Rejected.

### The identity gates

With the path active, eye 1 drew characters, NPCs, vehicles, trees and the HUD, but no roads,
floors, walls or building shells, and looked washed out. Counting insertions at `0x2818325` per
engine eye showed whole submission buckets at e.g. 5,721 for eye 0 and 0 for eye 1. The cause
is one instruction pattern repeated in several producers:

```asm
mov  rax, [rip + disp]       ; rax = *(exe+0x48EB3B8) = current view group
cmp  dword ptr [rax], 2      ; leading identity == 2, i.e. eye 1
je   skip                    ; eye 1 skips this whole submission loop
```

It is an unfinished "second view reuses the first view's frame-static work" optimisation:
nothing ever routes eye 0's output into eye 1. The fix is an execute breakpoint on each `je`;
the handler reads `[rax]` and, if it is `2`, sets `RIP` to the fall-through. Eye 1 then runs the
loop and builds its own command nodes in its own context.

| Name | `je` RVA | Fall-through | Restores |
|---|---|---|---|
| static | `0x165F34B` | `0x165F34D` | static world geometry |
| bucket16 | `0x165C40B` | `0x165C411` | shadow casters (this also fixed the "overexposed" eye) |
| bucket21_22 | `0x16767A3` | `0x16767A9` | remaining lighting / detail geometry |

With the initializer breakpoint that is four execute breakpoints, the x64 debug-register limit.
A correct gate is evaluated once per eye and opened only for eye 1, so its open count is exactly
half its hit count; anything else means it is firing on the wrong view. Measured: 3,600+
presents, no crash, all three gates at exactly half.

### HUD duplication

The engine draws the HUD once, last, during the eye-0 phase, full width across the side-by-side
target. So screen-left HUD elements land only in the left eye and centre/right elements only in
the right. For each HUD draw the add-on binds a half-width viewport and scissor, issues the draw
into the left half and then the right half, restores the game's viewport, and skips the
original. The UI's vertices already span full-screen NDC, so each half gets the whole HUD,
horizontally compressed, which is the anamorphic form a side-by-side frame expects.

HUD pipelines are identified at runtime, because pipeline handles change every launch:

1. `init_pipeline` hashes each pipeline's vertex **and** pixel shader into one pair identity.
2. While learning (dense for the first 45 presents, then one frame in four), each draw records
   its pipeline, the engine eye index read from `0x48EB588`, its order in the frame, its element
   count and its render-target width.
3. **Seed:** a pipeline whose draws are ≥95% eye-0, on a full-width target ≥2560 px wide, seen in
   ≥5 frames, ≥20 draws per frame, and never a draw over 4,096 elements, contributes its shader
   pair as a UI shader. Scene pipelines sit at about 50% eye-0; the HUD is exactly 100%.
4. **Select:** any pipeline using a UI shader pair, on the wide target, that never drew a mesh
   over 4,096 elements. Sticky. Hard cap of 24 pipelines (the busiest are kept).
5. **Per draw:** only draws of 512 elements or fewer are duplicated, even in a selected
   pipeline (menus draw 3D character previews through the same pipelines as UI quads).
6. Seeing a new pipeline triggers re-selection on the next present (minimum 8 presents apart,
   with a 15-present backstop), so battle and menu UI come good within a few frames.

How the rules got here (each step fixed a measured failure):

- Statistics alone (eye-0 fraction, volume, frame position) caught the field HUD but not
  battle UI, and every loosened threshold admitted something else. A lateness threshold based
  on "fraction of the largest frame seen" drifted upward and de-selected the HUD mid-session.
- Battle UI draws far fewer calls than the field HUD; a low volume floor would also admit
  full-screen composite passes, which are also eye-0-only and full width. Duplicating a
  composite into each half **tiles the whole screen**. That is the failure mode to fear.
- Keying on the vertex shader alone worked for menus and the minimap, then tiled the screen
  after browsing menus: the game shares one full-screen-quad vertex shader between sprites and
  composites. The vertex+pixel pair separates them (a composite samples the scene, a sprite
  samples an atlas).
- Menu detection from draw statistics is dead. A read-only probe reported "menu open" during
  ordinary gameplay with 1,440 "menu meshes". An earlier build that rescaled the whole frame
  on that signal flickered across the entire screen. Any menu-specific handling needs the
  game's own pause/menu state read from memory, with hysteresis on both edges, behind its own
  key, validated read-only first.

### The minimap (left eye only; read before attempting)

The minimap is not duplicated, so it shows in the left eye only. Five attempts to catch it all
tiled the screen or stopped working:

| Rule tried | Result |
|---|---|
| Companion volume floor lowered to 1 | tiled; 101 pipelines selected |
| Vertex-shader identity | worked, then tiled once menus seeded the shared vertex shader |
| Shared vertex shader + shared render target | tiled |
| High-volume anchors' render target + their draw-order band | tiled on arm |

Every property that could separate the minimap from a full-screen composite has been measured
and is shared: volume (both 1-6 small draws per frame), element count, eye (both eye-0 only),
position in the frame (inside the HUD's band), scissor (HUD, minimap and composites all use the
full-screen rect; only bloom mips and dialogue panels use small ones), and render target (a
composite draws into the same texture as the HUD). Any rule loose enough to admit the minimap
admits a pass that draws the whole frame.

What would actually work: classify draws by what they **sample**, not what they write. A
composite samples a full-screen texture; the minimap samples its own small render target. That
needs descriptor tracking (`bind_descriptor_tables` / `push_descriptors`) resolved back to source
textures, on an engine that is bindless with million-entry descriptor heaps, where an earlier
camera lane already failed on exactly that problem (see below). Until that exists, do not
change the duplication rules to chase the minimap.

`src/addon/experimental/lad8vr_ui_overlay_v41.cpp` is a sixth idea, built but never run: a frame
capture found that after the last eye-1 scene draw, the busiest HUD anchor's texture receives
only small primitives (479 draws, largest 60 elements), 468 of them already duplicated and the
other 11 being the minimap and some menu elements. v41 duplicates any small eye-0 draw into that
one texture after that point (measured per frame, applied on the next). It may work or may tile
the screen like its predecessors.

## Renderer facts

- `cb12` (register b12, 492 bytes, one layout across all 1,425 D3D12 shaders) is the per-view
  camera buffer: `c_matP` @0, `c_matV` @64, `c_matVP` @128, `c_matT[4]` @192, `c_rvtx_param`
  @448, `c_rt_size` @464, `c_location` @480. Column-vector convention, `VP = P * V` (measured).
  `c_matT[3]` behaves like the previous frame's VP.
- **Vertex shaders read `c_matP` and `c_matV` and multiply themselves; they do not read
  `c_matVP`.** Any detector that validates a camera by `VP == P * V` silently rejects the
  buffers that matter. Validate V's rotation for orthonormality and P for projection structure.
- Building draws bind cb10 (`c_matW`), cb11 (bones), cb12, cb13, cb5, cb9, cb2. Some passes use
  per-instance SRVs `c_ro_instance` (t16) and `c_ro_index` (t17).
- Bindless: three shader-visible descriptor heaps of 1,000,000 descriptors each. Descriptors
  are written into staging heaps and copied into the shader-visible heaps, so stores and loads
  use different heap handles.
- No indirect draws. Heavy compute (~27,000 dispatches per 20 s), every dispatch with a b12.
- The main pass's cb12 instances are `gpu_only` default-heap buffers; about a quarter are
  filled by `CopyBufferRegion` from an upload block, the rest by compute.
- The game creates four D3D12 devices; one renders.
- At the per-pass builder `0x27F6303`, `RCX` is the pass object (bucket at `+0x24`, selector at
  `+0x25`) and `RDX` its render context. 28 stable pass builders, buckets 2-31 except 9 and 19,
  about 111 calls per second each. Every builder calls `0x2842B30`, which copies a 496-byte
  camera block from `[context + 0x12CC0] + 0x480` (same layout as cb12). Bucket 4 carries a
  valid camera on a 32×32 target.
- Stable geometry stack: `0xDDBF7 > 0x27F73FB > 0x27F635A > 0x27E7AE9 > 0x4987AB`.

## Approaches that failed (do not repeat)

Native dual-view lane:

- Copying group 0's pass mask to group 1; changing the coordinator argument `EDX 2 → 1`, with
  or without the global group index: no visible change.
- Redirecting the current-group pointer, or setting group 1's identity to 1: swaps which eye is
  complete; sharing the identity globally eventually crashes at `0x27BA731`.
- Bypassing identity-1 setup gates in builders `0x2863B10`, `0x28643E0`, `0x2864A10`; copying
  view 1's frustum tail to view 2: no change.
- Calling the frame controller or pass builder twice inline: deadlocks the fork-join job system.
- Retaining eye 0's built command registry across the inter-eye reset and replaying it for
  eye 1: access violation in playback at `0x27F73E3`. Eye 1 must build, never copy.
- Identifying callers by scanning the stack for executable-range values: stale frames gave
  confident, wrong answers (the named "producers" got zero breakpoint hits). Use
  `RtlLookupFunctionEntry` + `RtlVirtualUnwind`; the binary ships `.pdata` even under DRM.

Camera-patching lanes (all before the native dual view was found; source in `src/proxy/`):

- Memory-scanning for a stable camera object: every candidate was a per-frame scratch slot.
- Patching cb12 in the upload ring at Present (too late), root CBVs (never used), descriptor
  tables (only ~5% of b12 binds could be resolved to a buffer, the vegetation/VFX/world-space UI
  that kept moving; solid geometry never moved), copy-source patching, compute-input patching,
  submission-time patching, blind `WriteBufferImmediate` writes: writes provably landed, but
  buildings never moved.
- `WriteBufferImmediate` inside the game's own command lists: device removed. Only on lists the
  mod submits itself.
- MinHook on `CreateCommittedResource`: crashes before the first present, even as a pure
  pass-through. Vtable-swapping devices: also crashes.
- AER at the per-pass builder: patching the `+0x480` camera block per eye at `0x27F6303` and
  restoring it at `0x27F6305` (59,155 patches with matching restores, no crash), with eyes
  chosen by Present parity and later by engine-produced eye tags (bucket 2 opens, bucket 31
  closes). Two OpenXR quad layers received the images. It never produced a coherent eye:
  scene objects moved, but black offset silhouettes appeared whenever the camera or player
  moved, in each eye on its own. This block is downstream of an earlier camera fan-out and
  visibility decision. The lane is retired.
- Depth reprojection (DIBR) side-by-side: worked, with the usual disocclusion ghosting. Not
  true stereo; superseded by the native dual view and not included here.

## The research dxgi.dll proxy (`src/proxy/`)

Built with `scripts\build_proxy.bat`, not shipped. It is a `dxgi.dll` proxy that forwards to
the real DLL (or to `dxgi_chain.dll` if present) and wraps the factory and swapchain objects.

- `dxgi_proxy.cpp`, `dxgi_wrappers.cpp`: export forwarding and COM wrappers. Never vtable-patch
  DXGI on this game: the Steam overlay patches the same slots and the two capture each other
  into infinite recursion. Wrappers never write into a vtable.
- `d3d_capture.cpp`: passive renderer census (swapchain size/format, colour space, queue,
  device nodes, `ViewInstancingTier`), and the per-present tick.
- `lad_xr.cpp`: OpenXR D3D12 session, sRGB swapchain selection (the backbuffer holds sRGB values
  in `R10G10B10A2_UNORM`; colour space `RGB_FULL_G22_NONE_P709`), a blit pipeline, resize and
  shutdown handling. It began as a working mono, head-tracked virtual screen. In its current
  form it submits the AER eye images as two eye-specific quad layers 2 m in front of the
  viewer, which is not a usable headset mode.
- `lad_engine_aer.cpp`: the failed AER camera experiment. Note that it installs its two execute
  breakpoints whenever the proxy is loaded, and only patches while an XR session runs.
- `lad_stereo.cpp`, `lad_camera.cpp`, `lad_cam_trace.cpp`: the failed camera lanes above, all
  gated off by `lad8vr.ini` (`src/proxy/lad8vr.example.ini`).
- ReShade and this proxy cannot both be loaded: chaining the proxy to ReShade through
  `dxgi_chain.dll` makes ReShade log "Unable to resolve hook" and never start its add-on system.
- Rules learned: never MinHook resource creation; never wait on threads from `DllMain`; never
  tear the XR session down from `DllMain`; rebuild the private copy on every swapchain resize
  (the game resizes from 1600×900 to 3840×2160 shortly after boot); OpenXR swapchain images
  are typeless and need an explicit RTV format, and are handed over in RENDER_TARGET state.

## Debug keys and log lines

Add-on keys (read in ReShade's `reshade_begin_effects` event):

| Key | Action |
|---|---|
| F9 | Arm (once, in gameplay). A second press logs `[IGNORED]`. |
| F10 | Toggle the ±0.032 eye separation (off after arming) |
| F11 | Cycle HUD mode: DUPLICATE / HIDE (proves the selected set is the HUD) / NORMAL |

Log: `%LOCALAPPDATA%\LAD8VR\LAD8VR_Stereo.txt` (recreated each launch).

```
[BOOT] v37 ready; UI identified by shader pair; ...     add-on loaded by ReShade
[ON] stereo + gates armed; learning the HUD ...           F9 accepted
[LEARNED] presents=45 ... uiShaders=N selected=M          after each change to the UI set
[CAND] rank=.. pipeline=0x.. SELECTED|rejected ...        one row per UI-shaped candidate
[GUARD] selection exceeded 24 pipelines; trimmed ...      the criteria are admitting too much
[WARN] no UI pipelines identified ...                     HUD not found; nothing duplicated
[FRAME] presents=.. mode=DUPLICATE duplicated=.. initializerHits=..   every 300 presents
[EYES] separation ON / OFF                                F10
[MODE] DUPLICATE / HIDE / NORMAL                          F11
```

`LAD8VR_Stereo_Crash.txt` receives one `[CRASH] code=... rva=...` line for the first access
violation seen while armed. The game's own crash handler does not capture these faults.

Operational rules: never arm at load or from a boot-time thread (suspending threads during
device creation deadlocks the boot); refresh breakpoints periodically to catch new render
threads; clear `Dr6` and set the resume flag before continuing; keep exactly one add-on from
this project active; never install or rename add-ons while the game is running.

## Tools

Python 3; `find_identity_gates.py` needs `pip install capstone`.

- `tools/dump_live_code_ranges.py <pid> <image_base> <out_dir>`: reads the code ranges listed
  in its `RANGES` table out of the running game process (read-only access) into
  `<name>_rva_<RVA>.bin` files. Edit `RANGES` for the code you want. The output is game code;
  do not commit it (`*.bin` is git-ignored).
- `tools/find_identity_gates.py`: disassembles those dumps with Capstone and reports every
  rip-relative load of the current-group global followed by `cmp dword [reg], N` and a
  conditional jump, with the exact fall-through address. It found the three gates in one pass.
  Verify the encoding before use (`0F 84` = `je rel32`, `74` = `je rel8`).

## Where to go next

1. **OpenXR delivery inside the add-on.** ReShade exposes the D3D12 device and queue, so the
   bring-up in `src/proxy/lad_xr.cpp` can move into the add-on. Copy the left and right halves
   of the side-by-side frame into two swapchain images (or one double-wide image with two
   sub-rects) and submit a projection layer. Drive the two eye transforms at `0x48EB590` from
   `xrLocateViews` instead of the fixed ±0.032, and make the engine's projection match the FOV
   declared to the runtime. Check first how the engine's per-eye projection maps each eye into
   its half; whether each half is anamorphic or at its own aspect was never settled.
2. **A version check.** Verify the bytes at each breakpoint address (and the initializer) before
   arming; refuse to arm on mismatch.
3. **More gates.** Buckets 14 (11,391 vs 2,808), 15 (48,858 vs 30,015), 27 (2,406 vs 0) and an
   explicit coordinator gate on bucket 31 (eye 1 0/230) are still eye-0-heavy. With all four
   debug registers in use, the options are: make eye 1's group identity 1 (satisfies every gate
   at once; previously crashed at `0x27BA731` inside `0x27BA600`, which would need neutralising),
   or patch the `je` bytes (removes the limit, but risks the DRM integrity checks this project has
   avoided so far).
4. **Minimap:** sampled-texture classification, as described above.
5. **Menus:** read the game's real pause/menu state if menu-specific handling is ever needed.
6. **World-space icons and billboards** are camera-facing geometry that read the view matrix in
   their vertex shader; check them in both eyes once a headset path exists.
