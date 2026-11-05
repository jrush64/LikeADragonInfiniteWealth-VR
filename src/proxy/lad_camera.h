#pragma once

namespace LAD8VR::Camera
{
// Starts a background search for the game's per-view constant buffer (cb12). Safe to call
// every present; it only launches once.
void Start() noexcept;

// Called once per present. Logs the decoded camera while a candidate is held.
void Tick() noexcept;

void Stop() noexcept;

// The upload-ring regions cb12 is allocated from, discovered by the scan. The stereo patcher
// sweeps these each frame to find the live camera buffer. Returns the count; fills up to
// `capacity` (base, size) pairs. Zero until the scan has locked on.
size_t GetRingRegions(const unsigned char** bases, size_t* sizes, size_t capacity) noexcept;

// True once the ring has been located and the regions are valid to sweep.
bool Ready() noexcept;

// Address of the main-scene cb12 as of the last background sweep, or null. The stereo patcher
// uses this as a seed so it never has to search in the submit path.
const unsigned char* CurrentCameraAddress() noexcept;
}
