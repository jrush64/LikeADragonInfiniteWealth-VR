# Building Like a Dragon: Infinite Wealth VR

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 (Community, Professional, Enterprise, or the standalone Build Tools)
  with the **"Desktop development with C++"** workload installed. The build scripts find it
  automatically via `vswhere.exe`.
- Nothing else. The ReShade add-on API headers, MinHook and the OpenXR headers are vendored in
  `third_party/`.

## Build the add-on (the release)

```
scripts\build.bat
```

Output: `builds\LAD8VR.addon64` (and `builds\LAD8VR.map`, useful for resolving crash
addresses).

The build is a single `cl.exe` invocation (no solution, no CMake). The add-on is a DLL with the
`.addon64` extension that exports `NAME` and `DESCRIPTION` for ReShade. It links the CRT
statically (`/MT`), so no VC++ redistributable is needed beside it. It does not link against
ReShade; ReShade finds and loads it at runtime.

### Experimental v41 variant (untested)

```
scripts\build.bat v41
```

Output: `builds\experimental\LAD8VR_v41_untested.addon64`. Only one add-on from this project may
be active at a time, so remove `LAD8VR.addon64` from the game folder before trying it. Read the
minimap section of `docs/TECHNICAL_NOTES.md` first.

## Build the research proxy (optional, not part of the release)

```
scripts\build_proxy.bat
```

Output: `builds\research\dxgi.dll` (git-ignored). This is the `dxgi.dll` proxy described under
"The research dxgi.dll proxy" in the technical notes. It imports `openxr_loader.dll` at runtime,
which this repo does not ship; use an x64 build of the official
[Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK). The build generates the
loader's import library from `third_party/openxr/openxr_loader.def`. The proxy and ReShade cannot
both be installed as `dxgi.dll`; the proxy is for development only.

## Deploying

The game executable lives in `<Like a Dragon Infinite Wealth install>\runtime\media\`, not in
the install root. With ReShade 6.7.1 (full add-on support) installed there as `dxgi.dll`, copy
`builds\LAD8VR.addon64` into the same folder while the game is closed. Full end-user steps are
in [`release/README.txt`](release/README.txt).

Logs are written to `%LOCALAPPDATA%\LAD8VR\`: `LAD8VR_Stereo.txt` (add-on) and
`LAD8VR_Stereo_Crash.txt` (first access violation while armed, if any). The research proxy
writes `LAD8VR_Log.txt` there. Expected log lines are listed in `docs/TECHNICAL_NOTES.md`.

## Third-party components

| Component | Location | License |
|---|---|---|
| ReShade add-on API headers | `third_party/reshade/include` | BSD-3-Clause (`third_party/reshade/LICENSE.md`) |
| MinHook (research proxy only) | `third_party/minhook` | BSD-2-Clause (`LICENSE.txt`) |
| OpenXR headers (research proxy only) | `third_party/openxr/include` | Apache-2.0 or MIT (`third_party/openxr/LICENSE`) |
| ReShade 6.7.1 runtime | not included; [reshade.me](https://reshade.me) | ReShade's own terms |
| OpenXR loader | not included; Khronos OpenXR-SDK | Apache-2.0 |
