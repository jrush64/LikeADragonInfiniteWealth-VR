#pragma once

#include <cstdint>

namespace LAD8VR::EngineAer
{
// Installs the proven per-pass camera breakpoint. Safe to call repeatedly.
void Start() noexcept;
// Arms newly-created render threads. Called from the present thread.
void Tick() noexcept;
// Camera shifting is enabled only while an XR session is rendering.
void SetActive(bool active) noexcept;
bool Active() noexcept;
// Returns the eye that actually built the oldest completed scene waiting for Present.
bool TryConsumeCompletedEye(uint32_t& eye) noexcept;
void Shutdown() noexcept;
}
