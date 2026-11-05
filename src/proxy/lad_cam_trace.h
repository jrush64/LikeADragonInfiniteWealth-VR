#pragma once

namespace LAD8VR::CamTrace
{
// True when [Camera] TraceWriter is enabled for this process.
bool Enabled() noexcept;

// Arms a one-shot, read-only hardware write breakpoint on a live cb12 view-matrix float.
// Safe to call repeatedly; only the first non-null address starts a trace.
void MaybeArm(const unsigned char* address) noexcept;

// Signals the reporter to stop. Never waits, so it is safe during process shutdown.
void Stop() noexcept;
}
