// Find the game's per-view constant buffer in memory.
//
// The shader archives give the exact shape statically, before any code runs (one layout
// across all 1425 D3D12 shaders):
//
//   cbuffer cb12 : register(b12)        // 492 bytes, row major
//   {
//       float4x4 c_matP;                // offset   0
//       float4x4 c_matV;                // offset  64
//       float4x4 c_matVP;               // offset 128
//       float4x4 c_matT[4];             // offset 192
//       float4   c_rvtx_param;          // offset 448
//       float4   c_rt_size;             // offset 464
//       float3   c_location;            // offset 480   camera world position
//   };
//
// Denuvo makes static analysis of the exe useless, but it does nothing about the game's own
// data. The buffer lives in a CPU-visible upload heap, so it can be found by its shape.
//
// Four filters, cheapest first. The first pass used only orthonormality and VP == V * P and
// returned 85748 hits, because a zero or identity buffer satisfies both trivially (0*0 = 0,
// I*I = I). Degeneracy rejection and the live-change test are what actually make this
// selective:
//   1. c_matP must be structurally a perspective projection, and not the identity.
//   2. c_matV's upper 3x3 must be orthonormal, and not the identity.
//   3. c_matVP must equal c_matV * c_matP across all 16 floats. A simultaneous identity
//      nothing satisfies by accident once the degenerate cases are gone.
//   4. The buffer must CHANGE between two samples taken half a second apart. A live camera
//      moves every frame; a shadow cascade or a stale copy does not. This is also what picks
//      the player camera out of the several view buffers the engine keeps.

#include "lad_camera.h"

#include "lad_cam_trace.h"

#include "logger.h"

#include <windows.h>

#include <cmath>
#include <string>
#include <vector>

namespace
{
constexpr size_t kCbSize = 492;
constexpr size_t kOffP = 0;
constexpr size_t kOffV = 64;
constexpr size_t kOffVP = 128;
constexpr size_t kOffRtSize = 464;
constexpr size_t kOffLocation = 480;

// Root constant buffer views must be 256 byte aligned, so the buffer never starts anywhere
// else and the scan can stride by 256 instead of crawling every float.
constexpr size_t kAlign = 256;

volatile LONG g_started = 0;
volatile LONG g_found = 0;
const unsigned char* g_address = nullptr;
HANDLE g_thread = nullptr;
volatile LONG g_stop = 0;
bool g_traceWriter = false;
ULONGLONG g_lastReport = 0;

// cb12 is sub-allocated from a per-frame upload ring, so its address is different every
// frame and there is nothing to latch. Confirmed live: every locked address stopped
// validating within ~2 s and the next full scan found it somewhere else entirely.
//
// The ring itself does not move, though. Remembering the allocation regions the hits landed
// in turns a 10 second full-address-space sweep into a sub-millisecond sweep of ~32 MB, which
// is what makes a live per-frame readout affordable at all.
struct Region
{
    const unsigned char* base = nullptr;
    size_t size = 0;
};
Region g_ringRegions[8];
size_t g_ringRegionCount = 0;

// Regions that are dense with cb12-shaped buffers. These are the GPU upload heaps: slow to
// read (write combined) and full of decoys, since every stale copy carries its own
// c_location. Excluding them BY MEASUREMENT rather than by size is what lets the stable hunt
// search large regions again - the 8 MB cap was a proxy for "not GPU memory" and it was
// throwing away the game's own heap segments, which is where the real object has to live.
Region g_gpuRegions[64];
size_t g_gpuRegionCount = 0;
CRITICAL_SECTION g_ringLock;
bool g_ringLockReady = false;

// The transient buffer cannot be written usefully: the engine rewrites cb12 from its own
// camera state every frame, so any patch is gone before it is read. The durable target is the
// STABLE object upstream that feeds it, and the way to find that is to take the live camera
// position as ground truth and look for the same value somewhere that does not move.
struct StableCandidate
{
    const unsigned char* address = nullptr;
    int confirmations = 0;
};
constexpr size_t kStableCapacity = 65536;
StableCandidate g_stable[kStableCapacity];
size_t g_stableCount = 0;
const unsigned char* g_stableBest = nullptr;
volatile LONG g_stablePhase = 0;  // 0 idle, 1 collecting, 2 narrowing, 3 settled
float g_truth[3] = {};
volatile LONG g_truthValid = 0;

// Published by the background ring sweep so the stereo patcher never has to search inside
// ExecuteCommandLists. Searching there is what dropped the game to a couple of frames a
// second: 24 MB of write-combined reads per frame in the hottest path in the renderer.
const unsigned char* g_liveCamera = nullptr;

struct Mat4
{
    float m[16];
};

struct Sample
{
    Mat4 v;
    Mat4 p;
    float location[3];
    float rtSize[4];
    int convention = 0;  // 1 = VP is V * P (row vectors), 2 = VP is P * V (column vectors)
};

bool AllFinite(const float* values, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (!isfinite(values[i])) return false;
    }
    return true;
}

// Row major: element (row, col) is m[row * 4 + col].
Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 out = {};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[row * 4 + k] * b.m[k * 4 + col];
            out.m[row * 4 + col] = sum;
        }
    }
    return out;
}

bool NearlyIdentity(const Mat4& a)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            const float want = row == col ? 1.0f : 0.0f;
            if (fabsf(a.m[row * 4 + col] - want) > 0.0001f) return false;
        }
    }
    return true;
}

// A rotation's upper 3x3 is orthonormal, and orthonormality survives transposition, so this
// one test is valid whichever matrix convention the engine uses. Its rejection of the
// identity is what the first attempt was missing.
bool LooksLikeView(const Mat4& v)
{
    if (NearlyIdentity(v)) return false;
    for (int row = 0; row < 3; ++row)
    {
        const float x = v.m[row * 4 + 0];
        const float y = v.m[row * 4 + 1];
        const float z = v.m[row * 4 + 2];
        const float length = x * x + y * y + z * z;
        if (length < 0.98f || length > 1.02f) return false;
    }
    const float dot = v.m[0] * v.m[4] + v.m[1] * v.m[5] + v.m[2] * v.m[6];
    return fabsf(dot) < 0.02f;
}

bool ProductEquals(const Mat4& expected, const Mat4& vp)
{
    for (int i = 0; i < 16; ++i)
    {
        const float difference = fabsf(expected.m[i] - vp.m[i]);
        const float scale = fabsf(expected.m[i]) + fabsf(vp.m[i]) + 1.0f;
        if (difference / scale > 0.002f) return false;
    }
    return true;
}

// "row_major" in the DXIL reflection describes STORAGE, not whether the engine multiplies
// with row vectors or column vectors. Row vectors give VP = V * P; column vectors give
// VP = P * V. Attempt two assumed the first and found nothing, so both are tested and the
// log reports which one hit. Same reason the strict projection skeleton is gone: its zero
// pattern is transposed between the two conventions, and a wrong guess there rejects every
// real camera. Orthonormality plus a product identity plus liveness is already selective.
int MatchedConvention(const Mat4& v, const Mat4& p, const Mat4& vp)
{
    if (ProductEquals(Multiply(v, p), vp)) return 1;  // row vector
    if (ProductEquals(Multiply(p, v), vp)) return 2;  // column vector
    return 0;
}

// Counts how many candidates survive each stage, so a run that finds nothing still says which
// filter is responsible instead of leaving it to another guess.
struct Funnel
{
    size_t readable = 0;
    size_t finite = 0;
    size_t orthonormal = 0;
    size_t productRow = 0;
    size_t productColumn = 0;
};

// Reads inside a scanned region can fault if the game frees it mid scan, so every access is
// wrapped. Kept as a leaf function with no unwinding so __try is legal here.
bool ReadSample(const unsigned char* address, Sample* out, Funnel* funnel)
{
    __try
    {
        // Cheap rejection first. Sweeping a 995 MB ring at a 256 byte stride is 3.9M probes,
        // and copying 220 bytes per probe before deciding anything took 42 seconds a pass.
        // Row 0 of the view matrix is unit length in either convention, so three floats and
        // three multiplies throw out effectively everything before any bulk copy happens.
        const float* row0 = reinterpret_cast<const float*>(address + kOffV);
        const float row0Length = row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2];
        if (row0Length < 0.98f || row0Length > 1.02f) return false;

        memcpy(out->p.m, address + kOffP, sizeof(out->p.m));
        memcpy(out->v.m, address + kOffV, sizeof(out->v.m));
        memcpy(out->rtSize, address + kOffRtSize, sizeof(out->rtSize));
        memcpy(out->location, address + kOffLocation, sizeof(out->location));
        Mat4 vp;
        memcpy(vp.m, address + kOffVP, sizeof(vp.m));
        if (funnel != nullptr) ++funnel->readable;

        if (!AllFinite(out->p.m, 16) || !AllFinite(out->v.m, 16) || !AllFinite(vp.m, 16))
            return false;
        if (funnel != nullptr) ++funnel->finite;

        if (!LooksLikeView(out->v)) return false;
        if (funnel != nullptr) ++funnel->orthonormal;

        out->convention = MatchedConvention(out->v, out->p, vp);
        if (funnel != nullptr)
        {
            if (out->convention == 1) ++funnel->productRow;
            if (out->convention == 2) ++funnel->productColumn;
        }
        return out->convention != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The structural tests prove a buffer is A camera. They do not prove it is THE camera, and a
// scan that lands while the title screen is up will happily lock onto a static 90 degree,
// square, rt 0x0 matrix sitting in some module's data. That happened, it published a position
// of 0.29, 0, 0 as ground truth, and the stable hunt then matched 4096 addresses that were
// really just zeros. A gameplay view has a real render target and a sane frustum; requiring
// that costs nothing and makes the lock independent of when the scan happens to run.
bool IsGameplayCamera(const Sample& s)
{
    if (s.rtSize[0] < 640.0f || s.rtSize[1] < 360.0f) return false;
    if (s.rtSize[0] > 16384.0f || s.rtSize[1] > 16384.0f) return false;
    const float aspect = s.p.m[0] != 0.0f ? fabsf(s.p.m[5] / s.p.m[0]) : 0.0f;
    if (aspect < 1.2f || aspect > 2.4f) return false;
    const float m11 = s.p.m[5];
    if (m11 == 0.0f) return false;
    const float fovYDeg = 2.0f * atanf(1.0f / fabsf(m11)) * 57.2957795f;
    // Excludes the 175 degree environment probe and the narrow square shadow cascades.
    return fovYDeg > 20.0f && fovYDeg < 120.0f;
}

bool Differs(const Sample& a, const Sample& b)
{
    for (int i = 0; i < 16; ++i)
    {
        if (fabsf(a.v.m[i] - b.v.m[i]) > 0.00001f) return true;
    }
    for (int i = 0; i < 3; ++i)
    {
        if (fabsf(a.location[i] - b.location[i]) > 0.00001f) return true;
    }
    return false;
}

void Describe(const char* tag, size_t index, const unsigned char* address, const Sample& s)
{
    // m11 sits at (1,1) in either convention, so the fov readout is convention independent.
    const float m11 = s.p.m[5];
    const float fovYDeg = m11 != 0.0f ? 2.0f * atanf(1.0f / fabsf(m11)) * 57.2957795f : 0.0f;
    const float aspect = s.p.m[0] != 0.0f ? fabsf(m11 / s.p.m[0]) : 0.0f;
    char line[360] = {};
    sprintf_s(line,
              "[CAM] %s %zu 0x%p conv=%s pos %.2f %.2f %.2f fovY %.1f aspect %.2f rt %.0fx%.0f",
              tag, index, static_cast<const void*>(address),
              s.convention == 1 ? "row" : (s.convention == 2 ? "col" : "?"), s.location[0],
              s.location[1], s.location[2], fovYDeg, aspect, s.rtSize[0], s.rtSize[1]);
    LAD8VR::Log::Line(line);
}

DWORD WINAPI StableThread(LPVOID);

// Re-sweeps the upload ring on the background thread, publishing the current camera as ground
// truth for the stable hunt. Runs forever; never touches the render thread.
// Sweeps a byte range, returning the widest gameplay view in it. Widest because the ring is
// littered with stale copies and half and quarter res post-chain views; the main scene pass is
// always the biggest render target.
size_t SweepRange(const unsigned char* start, size_t size, const unsigned char** best,
                  Sample* bestSample)
{
    size_t hits = 0;
    const size_t usable = size > kCbSize ? size - kCbSize : 0;
    for (size_t offset = 0; offset < usable && g_stop == 0; offset += kAlign)
    {
        Sample sample = {};
        if (!ReadSample(start + offset, &sample, nullptr)) continue;
        if (!IsGameplayCamera(sample)) continue;
        ++hits;
        if (*best == nullptr || sample.rtSize[0] > bestSample->rtSize[0])
        {
            *best = start + offset;
            *bestSample = sample;
        }
    }
    return hits;
}

void RingSweepLoop()
{
    int missedSweeps = 0;
    // Upload heaps are write combined, and CPU reads from write combined memory are brutally
    // slow: sweeping the full 963 MB ring took 23 seconds a pass, which starved the stable
    // hunt of the ground truth it exists to consume. A ring allocator hands out addresses
    // sequentially, so this frame's camera sits near last frame's. Searching a small window
    // around the last hit turns 963 MB into 16 MB, and the full sweep becomes the fallback
    // for when the ring wraps rather than the thing done every time.
    constexpr size_t kWindow = 8u << 20;
    const unsigned char* lastGood = g_address;

    while (g_stop == 0)
    {
        EnterCriticalSection(&g_ringLock);
        Region regions[8];
        const size_t count = g_ringRegionCount;
        for (size_t i = 0; i < count; ++i) regions[i] = g_ringRegions[i];
        LeaveCriticalSection(&g_ringLock);

        const unsigned char* best = nullptr;
        Sample bestSample = {};
        size_t hits = 0;

        if (lastGood != nullptr)
        {
            for (size_t r = 0; r < count && best == nullptr; ++r)
            {
                const unsigned char* regionEnd = regions[r].base + regions[r].size;
                if (lastGood < regions[r].base || lastGood >= regionEnd) continue;
                const unsigned char* from =
                    lastGood - regions[r].base > kWindow ? lastGood - kWindow : regions[r].base;
                const size_t span = static_cast<size_t>(
                    (from + 2 * kWindow < regionEnd ? from + 2 * kWindow : regionEnd) - from);
                hits = SweepRange(from, span, &best, &bestSample);
            }
        }

        if (best == nullptr)
        {
            hits = 0;
            for (size_t r = 0; r < count && g_stop == 0; ++r)
                hits += SweepRange(regions[r].base, regions[r].size, &best, &bestSample);
        }
        if (best != nullptr) lastGood = best;

        // If the gameplay camera vanishes for a while (a cutscene, a menu, or a lock made on
        // junk in the first place) go back to a full scan rather than publishing stale truth.
        if (best == nullptr)
        {
            if (++missedSweeps > 40)
            {
                LAD8VR::Log::Line("[CAM] no gameplay camera in the ring for 10 s, full rescan");
                InterlockedExchange(&g_truthValid, 0);
                InterlockedExchange(&g_found, 0);
                InterlockedExchange(&g_started, 0);
                return;
            }
        }
        else
        {
            missedSweeps = 0;
        }

        g_liveCamera = best;
        if (best != nullptr && g_traceWriter)
            LAD8VR::CamTrace::MaybeArm(best + kOffV);
        if (best != nullptr)
        {
            g_truth[0] = bestSample.location[0];
            g_truth[1] = bestSample.location[1];
            g_truth[2] = bestSample.location[2];
            InterlockedExchange(&g_truthValid, 1);

            const ULONGLONG now = GetTickCount64();
            if (g_lastReport == 0 || now - g_lastReport >= 3000)
            {
                g_lastReport = now;
                char line[80] = {};
                sprintf_s(line, "[CAM] ring sweep %zu view buffer(s)", hits);
                LAD8VR::Log::Line(line);
                Describe("now", 0, best, bestSample);
            }
        }
        Sleep(250);
    }
}

DWORD WINAPI ScanThread(LPVOID)
{
    LAD8VR::Log::Line("[CAM] scanning for cb12");

    SYSTEM_INFO info = {};
    GetSystemInfo(&info);

    for (int pass = 0; pass < 40 && g_stop == 0 && g_found == 0; ++pass)
    {
        const unsigned char* cursor = static_cast<const unsigned char*>(info.lpMinimumApplicationAddress);
        const unsigned char* limit = static_cast<const unsigned char*>(info.lpMaximumApplicationAddress);
        std::vector<const unsigned char*> hits;
        std::vector<Sample> firstSamples;
        size_t scanned = 0;
        Funnel funnel;

        while (cursor < limit && g_stop == 0)
        {
            MEMORY_BASIC_INFORMATION region = {};
            if (VirtualQuery(cursor, &region, sizeof(region)) == 0) break;
            const unsigned char* next = static_cast<const unsigned char*>(region.BaseAddress) +
                                        region.RegionSize;

            const bool writable = (region.Protect & PAGE_READWRITE) != 0;
            const bool guarded = (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
            // Upload heaps are committed, private and plainly writable. Skipping anything
            // bigger than 256 MB keeps the game's asset arenas out of the search.
            if (region.State == MEM_COMMIT && writable && !guarded &&
                region.Type == MEM_PRIVATE && region.RegionSize <= (256u << 20))
            {
                const unsigned char* start = static_cast<const unsigned char*>(region.BaseAddress);
                const size_t usable = region.RegionSize > kCbSize ? region.RegionSize - kCbSize : 0;
                size_t hitsHere = 0;
                for (size_t offset = 0; offset < usable && hits.size() < 4096; offset += kAlign)
                {
                    Sample sample = {};
                    if (ReadSample(start + offset, &sample, &funnel))
                    {
                        ++hitsHere;
                        hits.push_back(start + offset);
                        firstSamples.push_back(sample);
                    }
                }
                // Dense with view buffers means this is a GPU upload heap, not game state.
                if (hitsHere >= 16 && g_gpuRegionCount < 64)
                {
                    g_gpuRegions[g_gpuRegionCount].base = start;
                    g_gpuRegions[g_gpuRegionCount].size = region.RegionSize;
                    ++g_gpuRegionCount;
                }
                scanned += region.RegionSize;
            }
            if (next <= cursor) break;
            cursor = next;
        }

        char line[300] = {};
        sprintf_s(line,
                  "[CAM] pass %d scanned %zu MB | funnel readable=%zu finite=%zu orthonormal=%zu "
                  "productRow=%zu productCol=%zu",
                  pass, scanned >> 20, funnel.readable, funnel.finite, funnel.orthonormal,
                  funnel.productRow, funnel.productColumn);
        LAD8VR::Log::Line(line);
        if (hits.empty())
        {
            Sleep(2000);
            continue;
        }

        // Let the game render, then keep only the buffers that actually moved. A live camera
        // changes every frame; shadow cascades, reflection views and stale copies do not.
        Sleep(500);

        std::vector<const unsigned char*> live;
        std::vector<Sample> liveSamples;
        for (size_t i = 0; i < hits.size(); ++i)
        {
            Sample now = {};
            if (!ReadSample(hits[i], &now, nullptr)) continue;
            if (!Differs(firstSamples[i], now)) continue;
            if (!IsGameplayCamera(now)) continue;
            live.push_back(hits[i]);
            liveSamples.push_back(now);
        }

        sprintf_s(line, "[CAM] %zu of them changed and look like a gameplay view", live.size());
        LAD8VR::Log::Line(line);

        if (!live.empty())
        {
            for (size_t i = 0; i < live.size() && i < 12; ++i) Describe("live", i, live[i], liveSamples[i]);

            // Record the allocation regions the live hits landed in. Those are the upload
            // ring, and re-sweeping just them each frame is what replaces chasing a pointer
            // that no longer exists by the time it is read.
            EnterCriticalSection(&g_ringLock);
            g_ringRegionCount = 0;
            for (size_t i = 0; i < live.size() && g_ringRegionCount < 8; ++i)
            {
                MEMORY_BASIC_INFORMATION mbi = {};
                if (VirtualQuery(live[i], &mbi, sizeof(mbi)) == 0) continue;
                const unsigned char* base = static_cast<const unsigned char*>(mbi.BaseAddress);
                bool known = false;
                for (size_t r = 0; r < g_ringRegionCount; ++r)
                {
                    if (g_ringRegions[r].base == base) known = true;
                }
                if (known) continue;
                g_ringRegions[g_ringRegionCount].base = base;
                g_ringRegions[g_ringRegionCount].size = mbi.RegionSize;
                ++g_ringRegionCount;
            }
            const size_t regionCount = g_ringRegionCount;
            size_t totalBytes = 0;
            for (size_t r = 0; r < regionCount; ++r) totalBytes += g_ringRegions[r].size;
            LeaveCriticalSection(&g_ringLock);

            sprintf_s(line, "[CAM] upload ring is %zu region(s), %zu MB total, sweeping those live",
                      regionCount, totalBytes >> 20);
            LAD8VR::Log::Line(line);

            g_address = live[0];
            InterlockedExchange(&g_found, 1);

            // Hand off to the live loop on this same background thread. Nothing below ever
            // touches the render thread.
            if (!g_traceWriter && InterlockedCompareExchange(&g_stablePhase, 1, 0) == 0)
                CreateThread(nullptr, 0, &StableThread, nullptr, 0, nullptr);
            RingSweepLoop();
            return 0;
        }

        // Nothing moved. Show what was found anyway: if the game was paused or on a menu this
        // is still useful, and it says the structural filter is working.
        for (size_t i = 0; i < hits.size() && i < 8; ++i) Describe("static", i, hits[i], firstSamples[i]);
        Sleep(2000);
    }

    if (g_found == 0) LAD8VR::Log::Line("[CAM] gave up, no live cb12 found");
    return 0;
}

bool InRingRegion(const unsigned char* address)
{
    for (size_t r = 0; r < g_ringRegionCount; ++r)
    {
        if (address >= g_ringRegions[r].base && address < g_ringRegions[r].base + g_ringRegions[r].size)
            return true;
    }
    for (size_t r = 0; r < g_gpuRegionCount; ++r)
    {
        if (address >= g_gpuRegions[r].base && address < g_gpuRegions[r].base + g_gpuRegions[r].size)
            return true;
    }
    return false;
}

bool ReadFloat3(const unsigned char* address, float out[3])
{
    __try
    {
        memcpy(out, address, sizeof(float) * 3);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// One __try for a whole region instead of one per read. A guard per 4 byte step over several
// GB is what made the first version of this unusably slow; a faulting region is rare and
// losing the rest of it when that happens is a fine trade.
size_t ScanRegionForFloat3(const unsigned char* start, size_t size, const float truth[3],
                           StableCandidate* out, size_t capacity, size_t count)
{
    __try
    {
        const size_t usable = size > 16 ? size - 16 : 0;
        for (size_t offset = 0; offset < usable && count < capacity; offset += 4)
        {
            const float* value = reinterpret_cast<const float*>(start + offset);
            if (value[0] != truth[0]) continue;
            if (value[1] != truth[1]) continue;
            if (value[2] != truth[2]) continue;
            out[count].address = start + offset;
            out[count].confirmations = 1;
            ++count;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return count;
}

// Exact, not approximate. The engine copies these floats bit for bit into cb12, so the source
// holds identical values and a loose epsilon only buys false positives. The 0.02 tolerance in
// the first version is why round 0 kept hitting its cap.
bool MatchesTruth(const float value[3], const float truth[3])
{
    for (int i = 0; i < 3; ++i)
    {
        if (value[i] != truth[i]) return false;
    }
    return true;
}

// Classic narrowing: collect every address holding the camera position now, then keep only
// those still holding it after the camera has moved. Two or three rounds is enough, because
// anything that keeps agreeing with a moving camera IS the camera.
// Writes an offset into the located camera position, continuously, so the effect is visible
// on the flat screen with no headset. This is the whole point of the hunt: if the view shifts,
// the engine camera is drivable - Hinny that GOAT - from outside and stereo is a matter of which offset to write
// and when. If it does not shift, the address is a copy and the hunt needs another round.
//
// Off unless lad8vr.ini asks for it, because it deliberately fights the engine for the value.
// Leaf function with no unwinding, so __try is legal here. The caller has std::string in
// scope, which is why the guard cannot live inline there.
bool WriteFloatGuarded(const unsigned char* address, float value)
{
    __try
    {
        *const_cast<float*>(reinterpret_cast<const float*>(address)) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

DWORD WINAPI WriteTestThread(LPVOID)
{
    wchar_t iniPath[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&WriteTestThread), &self);
    GetModuleFileNameW(self, iniPath, MAX_PATH);
    if (wchar_t* slash = wcsrchr(iniPath, L'\\')) wcscpy_s(slash + 1, 32, L"lad8vr.ini");

    // Stored as thousandths so it fits an integer ini read. 2000 = 2.0 world units.
    const int offsetMilli = GetPrivateProfileIntW(L"Camera", L"TestOffsetX", 0, iniPath);
    if (offsetMilli == 0)
    {
        LAD8VR::Log::Line("[CAM] write test off ([Camera] TestOffsetX=0 in lad8vr.ini)");
        return 0;
    }
    const float offset = static_cast<float>(offsetMilli) / 1000.0f;

    char line[200] = {};
    sprintf_s(line, "[CAM] WRITE TEST ON: adding %.3f to camera X at 0x%p", offset,
              static_cast<const void*>(g_stableBest));
    LAD8VR::Log::Line(line);

    // Describe the page before touching it again. The first attempt wrote once and then
    // faulted, which says nothing about why on its own.
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(g_stableBest, &mbi, sizeof(mbi)) != 0)
    {
        sprintf_s(line,
                  "[CAM] target page: base 0x%p size %zu state 0x%X protect 0x%X type 0x%X",
                  mbi.BaseAddress, mbi.RegionSize, mbi.State, mbi.Protect, mbi.Type);
        LAD8VR::Log::Line(line);

        // If it is not plainly writable, make it so. A read only or guarded page explains a
        // fault on write exactly, and is trivially fixable.
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & writable) == 0 || (mbi.Protect & PAGE_GUARD) != 0)
        {
            DWORD previous = 0;
            if (VirtualProtect(const_cast<void*>(reinterpret_cast<const void*>(g_stableBest)),
                               sizeof(float) * 3, PAGE_READWRITE, &previous))
                LAD8VR::Log::Line("[CAM] target page was not writable, protection changed");
            else
                LAD8VR::Log::Line("[CAM] target page not writable and VirtualProtect failed");
        }
    }

    unsigned long long writes = 0;
    unsigned long long faults = 0;
    unsigned long long stuck = 0;
    ULONGLONG lastLog = 0;
    while (g_stop == 0 && g_stableBest != nullptr)
    {
        const float wanted = g_truth[0] + offset;
        if (!WriteFloatGuarded(g_stableBest, wanted))
        {
            // Do not give up on the first fault. The page can be freed and recommitted, and
            // one bad moment says much less than a rate does.
            ++faults;
            if (faults > 2000)
            {
                LAD8VR::Log::Line("[CAM] write test faulting continuously, target is gone");
                return 0;
            }
            Sleep(1);
            continue;
        }
        ++writes;

        // Read it straight back. If the value just written is still there, the engine has not
        // clobbered it yet; if it never survives, the engine owns this field and the write has
        // to happen at a different moment rather than at a different address.
        float readback[3] = {};
        if (ReadFloat3(g_stableBest, readback) && readback[0] == wanted) ++stuck;

        const ULONGLONG now = GetTickCount64();
        if (now - lastLog > 3000)
        {
            lastLog = now;
            sprintf_s(line,
                      "[CAM] write test: %llu writes, %llu stuck, %llu faults, live camera x %.2f",
                      writes, stuck, faults, g_truth[0]);
            LAD8VR::Log::Line(line);
        }
        Sleep(1);
    }
    return 0;
}

void StartWriteTest()
{
    CreateThread(nullptr, 0, &WriteTestThread, nullptr, 0, nullptr);
}

DWORD WINAPI StableThread(LPVOID)
{
    LAD8VR::Log::Line("[CAM] hunting the stable camera object upstream of the ring");

    SYSTEM_INFO info = {};
    GetSystemInfo(&info);

    // Wait for the sweep to publish a camera before counting rounds at all. The first version
    // burned all twelve rounds waiting and gave up before ground truth ever arrived.
    while (g_truthValid == 0 && g_stop == 0) Sleep(250);

    float lastTruth[3] = {};
    bool haveLastTruth = false;

    for (int round = 0; round < 12 && g_stop == 0; ++round)
    {
        float truth[3] = {g_truth[0], g_truth[1], g_truth[2]};

        // An elimination round is only worth anything if the camera has moved since the last
        // one: survivors are killed by DISAGREEING with a moving camera, so a still camera
        // eliminates nothing and just burns a round. Last run stalled at 2683 survivors for
        // exactly this reason. Wait for real movement instead of counting rounds against it.
        if (haveLastTruth && g_stablePhase == 2)
        {
            int waited = 0;
            while (g_stop == 0 && waited < 120)
            {
                const float dx = g_truth[0] - lastTruth[0];
                const float dy = g_truth[1] - lastTruth[1];
                const float dz = g_truth[2] - lastTruth[2];
                if (dx * dx + dy * dy + dz * dz > 0.25f) break;
                Sleep(250);
                ++waited;
            }
            if (waited >= 120)
            {
                LAD8VR::Log::Line("[CAM] camera has not moved in 30 s, need movement to narrow");
                continue;
            }
            truth[0] = g_truth[0];
            truth[1] = g_truth[1];
            truth[2] = g_truth[2];
        }
        lastTruth[0] = truth[0];
        lastTruth[1] = truth[1];
        lastTruth[2] = truth[2];
        haveLastTruth = true;

        if (g_stablePhase == 1)
        {
            // First round: sweep everything, 4 byte stride because the position is a plain
            // float3 in some engine struct with no alignment guarantee.
            const unsigned char* cursor =
                static_cast<const unsigned char*>(info.lpMinimumApplicationAddress);
            const unsigned char* limit =
                static_cast<const unsigned char*>(info.lpMaximumApplicationAddress);
            size_t found = 0;
            size_t swept = 0;
            size_t skipped = 0;
            const ULONGLONG began = GetTickCount64();
            ULONGLONG lastProgress = began;
            while (cursor < limit && g_stop == 0 && found < kStableCapacity)
            {
                // Round 0 went silent for 45 seconds with no way to tell whether it was
                // working or wedged. Say where the time is going instead of guessing again.
                const ULONGLONG nowTick = GetTickCount64();
                if (nowTick - lastProgress > 5000)
                {
                    lastProgress = nowTick;
                    char progress[200] = {};
                    sprintf_s(progress,
                              "[CAM]   round 0 progress: %zu MB swept, %zu MB skipped, %zu hits, "
                              "%llu s elapsed, at 0x%p",
                              swept >> 20, skipped >> 20, found, (nowTick - began) / 1000,
                              static_cast<const void*>(cursor));
                    LAD8VR::Log::Line(progress);
                }
                MEMORY_BASIC_INFORMATION region = {};
                if (VirtualQuery(cursor, &region, sizeof(region)) == 0) break;
                const unsigned char* next =
                    static_cast<const unsigned char*>(region.BaseAddress) + region.RegionSize;
                const bool writable = (region.Protect & PAGE_READWRITE) != 0;
                const bool guarded = (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
                // GPU heaps are excluded by measurement now (regions found dense with cb12
                // buffers), so the size cap can go back up. The 8 MB cap made the scan fast
                // by skipping 5.6 GB, but every candidate it returned was a per-frame scratch
                // copy, and the reason is almost certainly that it was also skipping the
                // game's own large heap segments, which is where a real camera object lives.
                const bool eligible =
                    region.State == MEM_COMMIT && writable && !guarded &&
                    region.Type == MEM_PRIVATE && region.RegionSize <= (512u << 20) &&
                    !InRingRegion(static_cast<const unsigned char*>(region.BaseAddress));
                if (eligible)
                {
                    found = ScanRegionForFloat3(
                        static_cast<const unsigned char*>(region.BaseAddress), region.RegionSize,
                        truth, g_stable, kStableCapacity, found);
                    swept += region.RegionSize;
                }
                else if (region.State == MEM_COMMIT)
                {
                    skipped += region.RegionSize;
                }
                if (next <= cursor) break;
                cursor = next;
            }
            g_stableCount = found;

            char line[220] = {};
            sprintf_s(line,
                      "[CAM] stable hunt round 0: %zu address(es) hold the camera position "
                      "(%zu MB swept, %zu MB skipped, %llu s)",
                      g_stableCount, swept >> 20, skipped >> 20,
                      (GetTickCount64() - began) / 1000);
            LAD8VR::Log::Line(line);
            InterlockedExchange(&g_stablePhase, 2);
            Sleep(1500);
            continue;
        }

        // Later rounds: keep only the ones that still agree once the camera has moved. These
        // rounds are cheap no matter how slow round 0 was, because they only re-read a few
        // thousand known addresses rather than sweeping anything.
        //
        // Agreement alone is not enough, and believing it cost a run. The winner last time was
        // a page that turned out to be MEM_FREE by the time it was written: a per-frame scratch
        // allocation that kept being handed back at the same offset (0xB68 in a page ending
        // F000, across three separate runs). A scratch slot that receives a fresh copy of the
        // camera every frame passes "still agrees with a moving camera" perfectly, so the test
        // was rewarding precisely what it needed to reject. Demanding the page stay committed
        // and private the whole time is what separates an object from a slot.
        size_t kept = 0;
        for (size_t i = 0; i < g_stableCount; ++i)
        {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(g_stable[i].address, &mbi, sizeof(mbi)) == 0) continue;
            if (mbi.State != MEM_COMMIT) continue;
            if (mbi.Type != MEM_PRIVATE) continue;
            if ((mbi.Protect & PAGE_READWRITE) == 0) continue;
            if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) continue;

            float value[3] = {};
            if (!ReadFloat3(g_stable[i].address, value)) continue;
            if (!MatchesTruth(value, truth)) continue;
            g_stable[kept].address = g_stable[i].address;
            g_stable[kept].confirmations = g_stable[i].confirmations + 1;
            ++kept;
        }
        g_stableCount = kept;

        char line[200] = {};
        sprintf_s(line, "[CAM] stable hunt round %d: %zu survivor(s) still tracking", round, kept);
        LAD8VR::Log::Line(line);

        for (size_t i = 0; i < kept && i < 10; ++i)
        {
            sprintf_s(line, "[CAM]   survivor %zu at 0x%p (%d confirmations)", i,
                      static_cast<const void*>(g_stable[i].address), g_stable[i].confirmations);
            LAD8VR::Log::Line(line);
        }

        // Eight confirmations, not four. A per-frame scratch slot can look convincing for a
        // few seconds; surviving a dozen seconds of movement while staying committed is a much
        // harder thing to fake.
        if (kept > 0 && g_stable[0].confirmations >= 8)
        {
            // Prefer a survivor in ordinary heap space. The engine's camera object is a plain
            // allocation; anything up in module/static space is far more likely to be a copy
            // some system kept, and writing to it would prove nothing.
            g_stableBest = g_stable[0].address;
            for (size_t i = 0; i < kept; ++i)
            {
                if (reinterpret_cast<uintptr_t>(g_stable[i].address) < 0x00007FF000000000ull)
                {
                    g_stableBest = g_stable[i].address;
                    break;
                }
            }
            InterlockedExchange(&g_stablePhase, 3);
            sprintf_s(line, "[CAM] stable camera position located at 0x%p, this is the write target",
                      static_cast<const void*>(g_stableBest));
            LAD8VR::Log::Line(line);

            // Report every survivor with its page state. One address is a guess; the list plus
            // its provenance is evidence, and if the chosen one turns out to be wrong the next
            // candidate is already named rather than needing another hunt.
            for (size_t i = 0; i < kept && i < 8; ++i)
            {
                MEMORY_BASIC_INFORMATION mbi = {};
                VirtualQuery(g_stable[i].address, &mbi, sizeof(mbi));
                sprintf_s(line, "[CAM]   candidate %zu 0x%p state 0x%X protect 0x%X type 0x%X size %zu",
                          i, static_cast<const void*>(g_stable[i].address), mbi.State, mbi.Protect,
                          mbi.Type, mbi.RegionSize);
                LAD8VR::Log::Line(line);
            }
            StartWriteTest();
            return 0;
        }
        if (kept == 0)
        {
            LAD8VR::Log::Line("[CAM] all survivors died, restarting the hunt");
            InterlockedExchange(&g_stablePhase, 1);
        }
        Sleep(1500);
    }
    return 0;
}
}

namespace LAD8VR::Camera
{
void Start() noexcept
{
    if (InterlockedExchange(&g_started, 1) != 0) return;

    // Off by default. The hunt is a minute of background memory scanning and it has no place
    // running on a launch where someone just wants to play the game.
    wchar_t iniPath[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Start), &self);
    GetModuleFileNameW(self, iniPath, MAX_PATH);
    if (wchar_t* slash = wcsrchr(iniPath, L'\\')) wcscpy_s(slash + 1, 32, L"lad8vr.ini");
    // Runs for the developer hunt, or whenever stereo needs the ring located.
    const bool hunt = GetPrivateProfileIntW(L"Camera", L"Hunt", 0, iniPath) != 0;
    g_traceWriter = GetPrivateProfileIntW(L"Camera", L"TraceWriter", 0, iniPath) != 0;
    const bool stereo = GetPrivateProfileIntW(L"Stereo", L"Enable", 0, iniPath) != 0;
    if (!hunt && !stereo && !g_traceWriter) return;
    if (g_traceWriter)
        LAD8VR::Log::Line("[CAMTRACE] engine-side writer trace enabled; stable-memory hunt disabled");

    if (!g_ringLockReady)
    {
        InitializeCriticalSection(&g_ringLock);
        g_ringLockReady = true;
    }
    g_thread = CreateThread(nullptr, 0, &ScanThread, nullptr, 0, nullptr);
}

void Tick() noexcept
{
    // Deliberately does nothing. Every sweep and every scan runs on a background thread.
    //
    // The previous version swept the upload ring from here, which put megabytes of reads and
    // 4x4 matrix maths on the game's render thread every couple of seconds and hung the game
    // after the title screen. Nothing in the camera hunt is frame coupled, so nothing in it
    // belongs on the present thread.
}

void Stop() noexcept
{
    InterlockedExchange(&g_stop, 1);
    LAD8VR::CamTrace::Stop();
    // Never wait on the thread here. A join from DllMain deadlocks against the loader lock
    // and hangs the process on exit.
}

size_t GetRingRegions(const unsigned char** bases, size_t* sizes, size_t capacity) noexcept
{
    if (!g_ringLockReady) return 0;
    EnterCriticalSection(&g_ringLock);
    const size_t count = g_ringRegionCount < capacity ? g_ringRegionCount : capacity;
    for (size_t i = 0; i < count; ++i)
    {
        bases[i] = g_ringRegions[i].base;
        sizes[i] = g_ringRegions[i].size;
    }
    LeaveCriticalSection(&g_ringLock);
    return count;
}

bool Ready() noexcept { return g_found != 0 && g_ringRegionCount > 0; }

const unsigned char* CurrentCameraAddress() noexcept { return g_liveCamera; }
}
