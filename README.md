# Like a Dragon: Infinite Wealth VR

An unfinished VR mod for Like a Dragon: Infinite Wealth (Steam, x64, D3D12, Dragon Engine),
built as a [ReShade](https://reshade.me) 6.7.1 add-on (`LAD8VR.addon64`). It switches on a
**dormant dual-view rendering path inside the game engine**, so the engine itself renders two
geometrically separated eye views in the same frame, side by side. It is not depth
reprojection, not alternate-eye rendering and not a duplicated flat image.

⚠️ Experimental, **unfinished** community mod. Not affiliated with SEGA or Ryu Ga Gotoku Studio.
Use at your own risk. Development has stopped, and the source is published so that others can
pick it up. **The stereo image does not reach a VR headset yet**: today it is a side-by-side
image on the flat monitor. See [What doesn't work yet](#what-doesnt-work-yet) and
[`docs/TECHNICAL_NOTES.md`](docs/TECHNICAL_NOTES.md).

Licensed under [GPL-3.0](LICENSE).

## What it does

Everything below was checked on a flat monitor only. None of it has been seen in a headset.

- **Native engine stereo (flat only).** Pressing F9 in gameplay activates the engine's own
  dual-view path. Both eyes render complete geometry, lighting and shadows from their own
  camera, in the same frame, as one side-by-side image at the game's output resolution (left eye in the
  left half, right eye in the right half). The second eye builds its own
  render commands; nothing is copied from the first eye. Earlier builds of the same mechanism
  ran for thousands of frames with no crash.
- **Real camera parallax.** F10 writes ±32 mm (64 mm total) into the engine's two eye
  transforms, which the engine applies to the camera before draw submission.
- **HUD in both eyes (flat only).** The engine draws the HUD once, across the full
  side-by-side frame. The add-on recognises the HUD's draws and re-issues each one into both
  halves. Field HUD, battle UI and menus appear correctly in both eyes.
- **No game code modified.** The engine is switched and its skip branches are opened with
  hardware (debug-register) breakpoints and a vectored exception handler, not code patches,
  so the game's integrity checks are not touched. Engine memory is written in only two places:
  the activation dword and the eye transforms.

## What doesn't work yet

- **No headset output.** The native stereo is only shown as a side-by-side image on the
  monitor. Delivering it to an OpenXR runtime is the next milestone and has not been started
  inside the add-on. A separate research `dxgi.dll` proxy in `src/proxy/` contains working
  D3D12 OpenXR bring-up code, but its current state is a failed camera experiment (see below),
  and ReShade and that proxy cannot be loaded at the same time.
- **Minimap in the left eye only.** This is the accepted state. Five attempts to duplicate it
  either missed it or tiled the whole screen; the technical notes explain why and what a real
  fix would need. An untested sixth attempt is in `src/addon/experimental/`.
- **No head tracking, fixed eye separation.** The separation is a hardcoded ±32 mm toggled by
  F10, and it starts OFF after arming. There is no positional or rotational tracking.
- **Exact game build only.** The add-on uses fixed addresses in `LikeADragon8.exe` and does
  **not** check the game version. On a different build the breakpoints land on the wrong code
  and the game will most likely crash. The build it was made for is identified in the
  technical notes.
- **Arm once, in gameplay.** Press F9 only after a save is loaded, never on the title or loading
  screens. A second F9 is ignored; to turn stereo off, quit the game.
- **Graphics settings.** Development testing was done with DLSS/FSR/XeSS, frame generation,
  motion blur, SSAO, screen-space reflections and depth of field off. Behaviour with those on
  has not been verified.
- **Some second-eye work is still skipped.** Per-eye command counts show a few submission
  buckets (14, 15, 27, 31) still weighted toward the first eye. No visible difference has
  been found, but all four hardware breakpoint slots are in use, so more gates cannot be
  opened without a different technique.

## Building

See [BUILD.md](BUILD.md). In short: install Visual Studio 2022 with the C++ workload, then run
`scripts\build.bat`. The ReShade add-on headers are vendored in `third_party/`.

## Installing

See [`release/README.txt`](release/README.txt). You must install **ReShade 6.7.1 with full
add-on support** yourself from [reshade.me](https://reshade.me); it is not included here.

## Layout

| Path | What's there |
|---|---|
| `src/addon/` | The release add-on: engine dual-view activation, gate breakpoints, HUD duplication (`lad8vr_stereo.cpp`) |
| `src/addon/experimental/` | Untested v41 variant that tries to bring the minimap into both eyes |
| `src/proxy/` | Research `dxgi.dll` proxy: OpenXR D3D12 bring-up, failed camera/AER experiments, renderer probes. Not part of the release |
| `scripts/` | `build.bat` (add-on), `build_proxy.bat` (research proxy) |
| `third_party/` | ReShade add-on API headers, MinHook and OpenXR headers, each with its license |
| `tools/` | Python helpers: dump live code ranges and scan them for identity gates |
| `docs/` | `TECHNICAL_NOTES.md`: architecture, engine addresses, approaches that failed and why |
| `release/` | End-user install notes |
| `builds/` | A prebuilt `LAD8VR.addon64` (and its `.map`) |

## License

GPL-3.0 - see [LICENSE](LICENSE). Copyright (C) 2026 Halcyon.

Vendored dependencies keep their own licenses: the ReShade add-on API headers (BSD-3-Clause,
`third_party/reshade/LICENSE.md`), MinHook (BSD-2-Clause, `third_party/minhook/LICENSE.txt`)
and the Khronos OpenXR headers (Apache-2.0 or MIT, `third_party/openxr/`). ReShade itself and
the OpenXR loader are not included.
