// Stereo by patching the camera at the one moment it can be patched: inside
// ID3D12CommandQueue::ExecuteCommandLists, after the game has recorded its scene draws and
// before the GPU reads the camera buffer those draws reference. Patching cb12 at Present is
// always too late (the GPU already consumed it) and patching a memory copy at any other time
// is racy; this window is neither.
//
// This first build is a PROOF, not stereo yet. It shoves the camera sideways by a large fixed
// amount every frame, mono, so the effect is visible on the flat screen with no headset. If
// the world visibly slides, the interception point works and per-eye offsetting is the same
// operation with a small alternating value. If it does not, the timing theory is wrong, and that
// is learned in one look instead of building AER on a bad foundation.

#include "lad_stereo.h"

#include "lad_camera.h"
#include "logger.h"

#include <MinHook.h>

#include <cmath>
#include <string>

namespace
{
constexpr size_t kOffP = 0;
constexpr size_t kOffV = 64;
constexpr size_t kOffVP = 128;
constexpr size_t kOffRtSize = 464;
constexpr size_t kAlign = 256;
constexpr size_t kCbSize = 492;
// 256 KB, not 12 MB. The first build swept 24 MB per frame inside ExecuteCommandLists and
// dropped the game to a couple of frames a second: that memory is write combined, so a sweep
// that is merely expensive on the background thread is ruinous in the submit path. The
// background sweep already knows where the camera is; this only has to cover the ring's drift
// between that sweep and now.
constexpr size_t kFrameWindow = 256u << 10;

using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                                       ID3D12CommandList* const*);
ExecuteCommandListsFn g_realExecute = nullptr;

// Sweeping the ring for something that "looks like" the camera was always a guess: the ring
// holds thousands of valid cb12 copies and only ONE is bound by the draws of this frame. The
// draws name it by GPU virtual address, so the only way to stop guessing is to watch the bind
// itself and translate that address back to writable memory.
//
//   CreateCommittedResource  -> remember every upload buffer's GPU address range and its
//                               mapped CPU pointer
//   SetGraphicsRootConstantBufferView -> the GPU address the scene pass actually binds
//   translate                -> the exact bytes the GPU is about to read
using CreateCommittedResourceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,
                                                              const D3D12_HEAP_PROPERTIES*,
                                                              D3D12_HEAP_FLAGS,
                                                              const D3D12_RESOURCE_DESC*,
                                                              D3D12_RESOURCE_STATES,
                                                              const D3D12_CLEAR_VALUE*, REFIID,
                                                              void**);
using CreateCommandListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT,
                                                        D3D12_COMMAND_LIST_TYPE,
                                                        ID3D12CommandAllocator*,
                                                        ID3D12PipelineState*, REFIID, void**);
using SetGraphicsRootCbvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                      D3D12_GPU_VIRTUAL_ADDRESS);

// Only 11 upload buffers were ever tracked and no camera bind ever resolved, which means the
// engine is not creating its constant memory with CreateCommittedResource. It sub-allocates
// from big heaps with CreatePlacedResource, so that is the call that has to be watched.
using CreatePlacedResourceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
                                                           const D3D12_RESOURCE_DESC*,
                                                           D3D12_RESOURCE_STATES,
                                                           const D3D12_CLEAR_VALUE*, REFIID,
                                                           void**);
CreatePlacedResourceFn g_realCreatePlaced = nullptr;
CreateCommittedResourceFn g_realCreateCommitted = nullptr;
CreateCommandListFn g_realCreateCommandList = nullptr;
SetGraphicsRootCbvFn g_realSetGraphicsRootCbv = nullptr;

struct UploadBuffer
{
    D3D12_GPU_VIRTUAL_ADDRESS gpuStart = 0;
    UINT64 size = 0;
    unsigned char* cpu = nullptr;
};
constexpr size_t kMaxUploads = 65536;
UploadBuffer g_uploads[kMaxUploads];
volatile LONG g_uploadCount = 0;

// The bound camera address, published by the bind hook and consumed at submit time.
unsigned char* volatile g_boundCamera = nullptr;
volatile LONG g_boundHits = 0;
// Separates "the bind hook never fires" from "it fires but the address will not translate"
// from "it translates but the bytes are not cb12". Three different failures, three different
// fixes, and last run could not tell them apart.
volatile LONG g_cbvSeen = 0;
volatile LONG g_cbvResolved = 0;

// cbvSeen stayed at 0 with the hook confirmed installed, which settles it: this engine never
// binds constants as root CBVs. It binds DESCRIPTOR TABLES. So the camera has to be followed
// one link further back:
//
//   CreateDescriptorHeap        -> the CPU and GPU base of each heap, so a GPU handle seen at
//                                  draw time can be turned back into a CPU handle
//   CreateConstantBufferView    -> the descriptor slot the engine writes, and the GPU address
//                                  of the buffer it points at
//   SetGraphicsRootDescriptorTable -> the table the scene pass actually binds
//
// Chain those and a bound table resolves to the exact constant buffer bytes.
using CreateDescriptorHeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,
                                                           const D3D12_DESCRIPTOR_HEAP_DESC*,
                                                           REFIID, void**);
using CreateCbvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
using SetGraphicsRootTableFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                        D3D12_GPU_DESCRIPTOR_HANDLE);

CreateDescriptorHeapFn g_realCreateDescriptorHeap = nullptr;
CreateCbvFn g_realCreateCbv = nullptr;
SetGraphicsRootTableFn g_realSetGraphicsRootTable = nullptr;

volatile LONG g_size512 = 0;
volatile LONG g_sizeSmall = 0;
volatile LONG g_sizeMid = 0;
volatile LONG g_sizeBig = 0;

struct HeapMap
{
    SIZE_T cpuBase = 0;
    UINT64 gpuBase = 0;
    UINT64 gpuEnd = 0;
    UINT increment = 0;
};
// 256, up from 64: the old cap dropped overflow heaps silently, and 20% of table binds failed
// handle conversion - the main scene pass was invisible inside that gap.
constexpr size_t kMaxHeaps = 256;
HeapMap g_heaps[kMaxHeaps];
// Heap objects already registered, so the SetDescriptorHeaps hook can skip known ones with a
// pointer scan instead of a GetDesc call per bind.
ID3D12DescriptorHeap* g_knownHeapPtrs[kMaxHeaps] = {};
volatile LONG g_heapCount = 0;
volatile LONG g_heapDropped = 0;
volatile LONG g_lateHeaps = 0;
UINT g_cbvIncrement = 0;

// Registers a shader-visible CBV/SRV/UAV heap wherever it is first seen - at creation, or at
// first bind for heaps that predate the hooks or arrived some other way. Returns false for
// heaps that are not shader-visible CBV heaps (samplers), so callers can stop re-checking.
bool RegisterCbvHeap(ID3D12DescriptorHeap* heap, UINT increment)
{
    if (heap == nullptr || increment == 0) return false;
    const UINT64 gpuBase = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    if (gpuBase == 0) return false;

    const LONG existing = g_heapCount < static_cast<LONG>(kMaxHeaps)
                              ? g_heapCount
                              : static_cast<LONG>(kMaxHeaps);
    for (LONG i = 0; i < existing; ++i)
        if (g_heaps[i].gpuBase == gpuBase) return true;

    const D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
    if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0)
        return false;

    const LONG slot = InterlockedIncrement(&g_heapCount) - 1;
    if (slot >= static_cast<LONG>(kMaxHeaps))
    {
        InterlockedDecrement(&g_heapCount);
        InterlockedIncrement(&g_heapDropped);
        return false;
    }
    g_cbvIncrement = increment;
    g_heaps[slot].cpuBase = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    g_heaps[slot].increment = increment;
    g_heaps[slot].gpuEnd = gpuBase + static_cast<UINT64>(desc.NumDescriptors) * increment;
    // gpuBase published last: GpuHandleToCpu treats a nonzero gpuBase as "entry complete".
    g_heaps[slot].gpuBase = gpuBase;
    g_knownHeapPtrs[slot] = heap;

    char line[160] = {};
    sprintf_s(line, "[HEAP] registered #%ld base=0x%llx descriptors=%u", slot,
              static_cast<unsigned long long>(gpuBase), desc.NumDescriptors);
    LAD8VR::Log::Line(line);
    return true;
}

// Sampler heaps flow through SetDescriptorHeaps too; remembering the rejects keeps the hook
// from paying a GetDesc on them every bind.
ID3D12DescriptorHeap* g_ignoredHeapPtrs[64] = {};
volatile LONG g_ignoredHeapCount = 0;

// Descriptor slot -> the GPU address of the buffer it views. Open addressed so the draw path
// stays a couple of probes rather than a scan.
// 1M entries, up from 64K: the engine runs three shader-visible heaps of a MILLION
// descriptors each (measured, [HEAP] lines), and a 64K table was shedding records to
// collisions all session.
constexpr size_t kCbvTableSize = 1u << 20;
struct CbvEntry
{
    volatile SIZE_T cpuHandle;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    // CBV size travels with the address so the probe can tell a 512-byte cb12 candidate from
    // the material and light buffers that flow through the same descriptor path.
    UINT size;
};
CbvEntry g_cbvTable[kCbvTableSize];

volatile LONG g_tableSeen = 0;
volatile LONG g_handleOk = 0;
volatile LONG g_cbvRecorded = 0;
// resolved counts descriptors that named a buffer; translated counts those whose buffer
// can actually be reached in memory. 2.7M resolved with 0 camera binds is either "the bytes
// are never reached" or "the bytes are never cb12", and those need completely different fixes.
volatile LONG g_translated = 0;
// Widest render target seen among translated constant buffers. If the camera is passing
// through here at all, this lands on 3840; if it stays 0, cb12 never reaches this path.
volatile LONG g_widestRt = 0;

// tables fired 53630 times and resolved stayed at 0, which points at the standard D3D12
// pattern: descriptors are authored in a CPU-only staging heap and copied into the
// shader-visible heap the draws bind. The mapping was recorded against the staging handle, so
// the bound copy never matched. Following the copies fixes that.
using CopyDescriptorsSimpleFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT,
                                                         D3D12_CPU_DESCRIPTOR_HANDLE,
                                                         D3D12_CPU_DESCRIPTOR_HANDLE,
                                                         D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyDescriptorsFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                                   UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                                   const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
CopyDescriptorsSimpleFn g_realCopyDescriptorsSimple = nullptr;
CopyDescriptorsFn g_realCopyDescriptors = nullptr;

bool g_enabled = false;
// One bit per entry in the specs table, so a bad hook can be bisected from the ini without a
// rebuild. Enabling all of them at once and guessing which crashed has cost several launches.
unsigned g_hookMask = 0xFFFFFFFFu;
// Makes every resource-creation hook a pure call-through: the detour is installed and runs,
// but does no work of its own. Deferring the Map did not stop the crash and the hook body is
// now almost empty, so this separates "the code in the detour" from "MinHook trampolining this
// particular function at all". Those need completely different fixes.
bool g_passthrough = false;
// The proof offset sways sinusoidally instead of holding constant. A constant camera shift is
// invisible on success - the world just looks normal from a different spot. A sway is
// unmissable, and "is the whole world rocking or only the plants" is a question anyone can
// answer at a glance.
float g_offsetAmplitude = 0.0f;
// The WriteBufferImmediate probe: proves the camera is writable at a raw GPU virtual address,
// with no resource tracking, no Map, and no hook anywhere near resource creation - the exact
// machinery every crash came from.
bool g_probe = false;
// Delta calibration: probe machinery writes each candidate's own GPU VA into its padding
// bytes; the ring sweep reads it back from the CPU side; VA - CPU is one constant per ring
// region and translation becomes a subtraction. Phase 2 of the plan, no device wrapper.
bool g_calibrate = false;
// Hook CopyBufferRegion/CopyResource to find and patch the staging source that feeds the
// main pass's camera buffer. Bisectable from the ini because these are two more MinHooks on
// the command-list vtable, and one bad prologue cost a day once already.
bool g_hookCopies = true;
void AddProbeCandidate(D3D12_GPU_VIRTUAL_ADDRESS gpu);
void EnqueueCandidate(unsigned char* cpu);
unsigned char* TranslateViaDelta(D3D12_GPU_VIRTUAL_ADDRESS gpu);
bool StampingActive();
void NoteMysteryVa(D3D12_GPU_VIRTUAL_ADDRESS gpu);
// Total-coverage entry point: every 512-byte CBV the engine creates or copies, from any hook.
// Ring-backed ones get the validated CPU patch; out-of-ring ones get the live camera written
// blind by WBI. No table walking anywhere - a bindless mega-heap makes walking meaningless.
void NoteCbv(D3D12_GPU_VIRTUAL_ADDRESS gpu, UINT size);
float g_offset = 0.0f;
// Bytes 492-511 of the 512-byte CBV slot are padding: cb12 is 492 bytes and no shader reads
// past 491. The GPU-side stamp lives at 492 (magic, VA low, VA high), the CPU-side frame
// stamps at 504/508. The game rewriting a slot clears them; everything self-heals.
constexpr size_t kOffStamp = 492;
constexpr UINT kStampMagic = 0x4C414438; // "8DAL" little-endian, reads as LAD8 in a hex dump
constexpr size_t kOffFrameStamp = 504;   // "enqueued this frame" - bind threads
constexpr size_t kOffPatchStamp = 508;   // "patched this frame" - submit thread
volatile LONG g_patchedThisFrame = 0;
const unsigned char* g_lastAddr = nullptr;
unsigned long long g_patches = 0;
ULONGLONG g_lastLog = 0;

struct Mat4
{
    float m[16];
};

bool ReadBytes(const unsigned char* address, void* out, size_t size)
{
    __try
    {
        memcpy(out, address, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteBytes(unsigned char* address, const void* in, size_t size)
{
    __try
    {
        memcpy(address, in, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Column-vector, row-major storage: element (row, col) is m[row * 4 + col], and the composite
// is VP = P * V.
Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 out = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[r * 4 + k] * b.m[k * 4 + c];
            out.m[r * 4 + c] = sum;
        }
    return out;
}

bool IsGameplayCb12(const unsigned char* address, Mat4* v, Mat4* p)
{
    // Cheap prefilter: row 0 of the view matrix is unit length in either convention.
    float row0[3] = {};
    if (!ReadBytes(address + kOffV, row0, sizeof(row0))) return false;
    const float len = row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2];
    if (len < 0.98f || len > 1.02f) return false;

    Mat4 vp = {};
    float rt[2] = {};
    if (!ReadBytes(address + kOffP, p->m, sizeof(p->m))) return false;
    if (!ReadBytes(address + kOffV, v->m, sizeof(v->m))) return false;
    if (!ReadBytes(address + kOffVP, vp.m, sizeof(vp.m))) return false;
    if (!ReadBytes(address + kOffRtSize, rt, sizeof(rt))) return false;

    if (rt[0] < 640.0f || rt[0] > 16384.0f || rt[1] < 360.0f || rt[1] > 16384.0f) return false;
    const float m11 = p->m[5];
    if (m11 == 0.0f || p->m[0] == 0.0f) return false;
    const float aspect = fabsf(m11 / p->m[0]);
    if (aspect < 1.2f || aspect > 2.4f) return false;
    const float fovY = 2.0f * atanf(1.0f / fabsf(m11)) * 57.2957795f;
    if (fovY < 20.0f || fovY > 120.0f) return false;

    // Confirm it really is cb12: VP == P * V across all 16 floats.
    const Mat4 expected = Multiply(*p, *v);
    float rtWidthBest = rt[0];
    (void)rtWidthBest;
    for (int i = 0; i < 16; ++i)
    {
        const float d = fabsf(expected.m[i] - vp.m[i]);
        const float s = fabsf(expected.m[i]) + fabsf(vp.m[i]) + 1.0f;
        if (d / s > 0.01f) return false;
    }
    return true;
}

// Finds the widest gameplay cb12 in a byte range and applies the eye offset to it in place.
bool PatchRange(const unsigned char* start, size_t size, const unsigned char** best,
                float* bestWidth)
{
    bool patched = false;
    const size_t usable = size > kCbSize ? size - kCbSize : 0;
    for (size_t off = 0; off < usable; off += kAlign)
    {
        Mat4 v = {};
        Mat4 p = {};
        const unsigned char* addr = start + off;
        if (!IsGameplayCb12(addr, &v, &p)) continue;

        float rt[2] = {};
        ReadBytes(addr + kOffRtSize, rt, sizeof(rt));
        if (*best != nullptr && rt[0] <= *bestWidth) continue;

        // Shift the eye by translating the view in view-space X, then rebuild VP = P * V so
        // shaders using either matrix agree. c_location is left for the AER build; a slightly
        // wrong specular highlight does not affect whether the geometry moves.
        v.m[3] += g_offset;
        const Mat4 vp = Multiply(p, v);
        if (WriteBytes(const_cast<unsigned char*>(addr) + kOffV, v.m, sizeof(v.m)) &&
            WriteBytes(const_cast<unsigned char*>(addr) + kOffVP, vp.m, sizeof(vp.m)))
        {
            *best = addr;
            *bestWidth = rt[0];
            patched = true;
        }
    }
    return patched;
}

void PatchThisFrame()
{
    const unsigned char* bases[8] = {};
    size_t sizes[8] = {};
    const size_t count = LAD8VR::Camera::GetRingRegions(bases, sizes, 8);
    if (count == 0) return;

    // Seed from the background sweep, which is already tracking the main scene camera. Falling
    // back to this probe's own last hit only matters if that thread has not published yet.
    const unsigned char* seed = LAD8VR::Camera::CurrentCameraAddress();
    if (seed == nullptr) seed = g_lastAddr;
    if (seed == nullptr) return;

    const unsigned char* best = nullptr;
    float bestWidth = 0.0f;

    // A small window around the seed, and NO full-region fallback. Missing a frame costs one
    // frame of stereo; searching a gigabyte in the submit path costs the whole game.
    for (size_t r = 0; r < count; ++r)
    {
        const unsigned char* end = bases[r] + sizes[r];
        if (seed < bases[r] || seed >= end) continue;
        const unsigned char* from =
            static_cast<size_t>(seed - bases[r]) > kFrameWindow ? seed - kFrameWindow : bases[r];
        const size_t span =
            static_cast<size_t>((from + 2 * kFrameWindow < end ? from + 2 * kFrameWindow : end) - from);
        PatchRange(from, span, &best, &bestWidth);
        break;
    }

    if (best != nullptr)
    {
        g_lastAddr = best;
        ++g_patches;
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastLog > 3000)
        {
            g_lastLog = now;
            char line[160] = {};
            sprintf_s(line, "[STEREO] patched cb12 %llu times, latest at 0x%p width %.0f",
                      g_patches, static_cast<const void*>(best), bestWidth);
            LAD8VR::Log::Line(line);
        }
    }
}

volatile LONG g_translateHint = 0;

unsigned char* TranslateGpuAddress(D3D12_GPU_VIRTUAL_ADDRESS gpu)
{
    const LONG count = g_uploadCount > static_cast<LONG>(kMaxUploads)
                           ? static_cast<LONG>(kMaxUploads)
                           : g_uploadCount;

    // Try the buffer that answered last time first. This is called millions of times a second
    // from the draw path, and a linear walk of 5800 buffers per call is billions of compares
    // a frame; consecutive binds almost always come from the same arena.
    const LONG hint = g_translateHint;
    if (hint >= 0 && hint < count)
    {
        const UploadBuffer& u = g_uploads[hint];
        if (u.cpu != nullptr && gpu >= u.gpuStart && gpu < u.gpuStart + u.size)
            return u.cpu + (gpu - u.gpuStart);
    }

    for (LONG i = 0; i < count; ++i)
    {
        const UploadBuffer& u = g_uploads[i];
        if (u.cpu == nullptr || gpu < u.gpuStart || gpu >= u.gpuStart + u.size) continue;
        g_translateHint = i;
        return u.cpu + (gpu - u.gpuStart);
    }
    return nullptr;
}

size_t CbvSlot(SIZE_T handle)
{
    // Descriptor handles are increment-sized apart, so the low bits are nearly constant.
    // Mixing before masking keeps the table from degenerating into one long probe chain.
    const SIZE_T mixed = (handle >> 5) * 0x9E3779B97F4A7C15ull;
    return static_cast<size_t>(mixed & (kCbvTableSize - 1));
}

void RememberCbv(SIZE_T cpuHandle, D3D12_GPU_VIRTUAL_ADDRESS gpu, UINT size)
{
    size_t slot = CbvSlot(cpuHandle);
    for (size_t probe = 0; probe < 32; ++probe)
    {
        const SIZE_T existing = g_cbvTable[slot].cpuHandle;
        if (existing == 0 || existing == cpuHandle)
        {
            g_cbvTable[slot].cpuHandle = cpuHandle;
            g_cbvTable[slot].gpu = gpu;
            g_cbvTable[slot].size = size;
            return;
        }
        slot = (slot + 1) & (kCbvTableSize - 1);
    }
}

D3D12_GPU_VIRTUAL_ADDRESS LookupCbv(SIZE_T cpuHandle, UINT* sizeOut)
{
    size_t slot = CbvSlot(cpuHandle);
    for (size_t probe = 0; probe < 32; ++probe)
    {
        const SIZE_T existing = g_cbvTable[slot].cpuHandle;
        if (existing == 0) return 0;
        if (existing == cpuHandle)
        {
            if (sizeOut != nullptr) *sizeOut = g_cbvTable[slot].size;
            return g_cbvTable[slot].gpu;
        }
        slot = (slot + 1) & (kCbvTableSize - 1);
    }
    return 0;
}

// A shader-visible heap's GPU handles run parallel to its CPU handles, so the offset of a GPU
// handle from its heap's GPU base gives the matching CPU handle directly.
SIZE_T GpuHandleToCpu(UINT64 gpuHandle)
{
    const LONG count = g_heapCount;
    for (LONG i = 0; i < count; ++i)
    {
        const HeapMap& h = g_heaps[i];
        if (h.cpuBase == 0 || gpuHandle < h.gpuBase || gpuHandle >= h.gpuEnd) continue;
        return h.cpuBase + static_cast<SIZE_T>(gpuHandle - h.gpuBase);
    }
    return 0;
}

void PatchIfCamera(unsigned char* cpu)
{
    Mat4 v = {};
    Mat4 p = {};

    // Record the widest render target that passes the cb12 product test even if the gameplay
    // gate rejects it. That separates "cb12 never comes through here" from "it does, but the
    // gate is wrong about which one is the scene camera".
    {
        float rt[2] = {};
        Mat4 tv = {};
        Mat4 tp = {};
        Mat4 tvp = {};
        if (ReadBytes(cpu + kOffP, tp.m, sizeof(tp.m)) &&
            ReadBytes(cpu + kOffV, tv.m, sizeof(tv.m)) &&
            ReadBytes(cpu + kOffVP, tvp.m, sizeof(tvp.m)) &&
            ReadBytes(cpu + kOffRtSize, rt, sizeof(rt)))
        {
            const Mat4 expected = Multiply(tp, tv);
            bool product = true;
            for (int i = 0; i < 16 && product; ++i)
            {
                const float d = fabsf(expected.m[i] - tvp.m[i]);
                const float s = fabsf(expected.m[i]) + fabsf(tvp.m[i]) + 1.0f;
                if (d / s > 0.01f) product = false;
            }
            if (product && rt[0] > 0.0f && rt[0] < 20000.0f &&
                static_cast<LONG>(rt[0]) > g_widestRt)
                g_widestRt = static_cast<LONG>(rt[0]);
        }
    }

    // During a probe run the GPU-side write must be the only thing touching the camera, or a
    // corrupted screen cannot be attributed. Diagnostics above still run; writes stop here.
    if (g_probe) return;

    if (!IsGameplayCb12(cpu, &v, &p)) return;
    g_boundCamera = cpu;
    InterlockedIncrement(&g_boundHits);
    v.m[3] += g_offset;
    const Mat4 vp = Multiply(p, v);
    WriteBytes(cpu + kOffV, v.m, sizeof(v.m));
    WriteBytes(cpu + kOffVP, vp.m, sizeof(vp.m));
    ++g_patches;
}

// The bind the scene pass actually uses on this engine. A table can hold many descriptors and
// the camera is not necessarily the first, so a short run from the base is walked.
void STDMETHODCALLTYPE HookedSetGraphicsRootTable(ID3D12GraphicsCommandList* list, UINT rootIndex,
                                                  D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
    if (g_enabled)
    {
        InterlockedIncrement(&g_tableSeen);
        SIZE_T cpu = GpuHandleToCpu(gpuHandle.ptr);
        if (cpu != 0 && g_cbvIncrement != 0)
        {
            InterlockedIncrement(&g_handleOk);
            // 64, not 24: the main scene pass never flickered under the probe while clouds
            // did, and a cb12 descriptor past the walk's end is the simplest explanation.
            // Misses terminate on the first empty hash slot, so the widening is cheap.
            for (UINT i = 0; i < 64; ++i)
            {
                UINT viewSize = 0;
                const D3D12_GPU_VIRTUAL_ADDRESS gpu = LookupCbv(cpu + i * g_cbvIncrement, &viewSize);
                if (gpu == 0) continue;
                InterlockedIncrement(&g_cbvResolved);
                // 492 bytes rounds to a 512-byte CBV; nothing else needs to be known about a
                // buffer to make it a probe candidate. The probe writes by GPU VA, so the
                // translate failing (the whole Phase 5 wall) does not matter here.
                if (viewSize == 512 && (g_probe || StampingActive())) AddProbeCandidate(gpu);
                // One entry point for every camera-sized view, shared with the create and
                // copy hooks: readable ones queue for the validated CPU patch, out-of-ring
                // ones queue for the blind GPU-timeline write.
                NoteCbv(gpu, viewSize);
            }
        }
    }
    g_realSetGraphicsRootTable(list, rootIndex, gpuHandle);
}

// Descriptor copies carry the buffer association with them. Propagating it is what lets a
// handle bound from a shader-visible heap resolve to the buffer authored in a staging heap.
void STDMETHODCALLTYPE HookedCopyDescriptorsSimple(ID3D12Device* device, UINT count,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE src,
                                                   D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && g_cbvIncrement != 0)
    {
        for (UINT i = 0; i < count; ++i)
        {
            UINT size = 0;
            const D3D12_GPU_VIRTUAL_ADDRESS gpu = LookupCbv(src.ptr + i * g_cbvIncrement, &size);
            if (gpu != 0)
            {
                RememberCbv(dest.ptr + i * g_cbvIncrement, gpu, size);
                NoteCbv(gpu, size);
            }
        }
    }
    g_realCopyDescriptorsSimple(device, count, dest, src, type);
}

void STDMETHODCALLTYPE HookedCopyDescriptors(ID3D12Device* device, UINT destRangeCount,
                                             const D3D12_CPU_DESCRIPTOR_HANDLE* destStarts,
                                             const UINT* destSizes, UINT srcRangeCount,
                                             const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts,
                                             const UINT* srcSizes,
                                             D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && g_cbvIncrement != 0 &&
        destStarts != nullptr && srcStarts != nullptr)
    {
        // Ranges on both sides are walked as one flat sequence, which is exactly how the API
        // defines the pairing.
        UINT destRange = 0, destIndex = 0, srcRange = 0, srcIndex = 0;
        while (destRange < destRangeCount && srcRange < srcRangeCount)
        {
            const UINT destSize = destSizes != nullptr ? destSizes[destRange] : 1;
            const UINT srcSize = srcSizes != nullptr ? srcSizes[srcRange] : 1;
            UINT viewSize = 0;
            const D3D12_GPU_VIRTUAL_ADDRESS gpu =
                LookupCbv(srcStarts[srcRange].ptr + srcIndex * g_cbvIncrement, &viewSize);
            if (gpu != 0)
            {
                RememberCbv(destStarts[destRange].ptr + destIndex * g_cbvIncrement, gpu, viewSize);
                NoteCbv(gpu, viewSize);
            }
            if (++destIndex >= destSize) { destIndex = 0; ++destRange; }
            if (++srcIndex >= srcSize) { srcIndex = 0; ++srcRange; }
        }
    }
    g_realCopyDescriptors(device, destRangeCount, destStarts, destSizes, srcRangeCount, srcStarts,
                          srcSizes, type);
}

void STDMETHODCALLTYPE HookedCreateCbv(ID3D12Device* device,
                                       const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    if (desc != nullptr && desc->BufferLocation != 0)
    {
        RememberCbv(dest.ptr, desc->BufferLocation, desc->SizeInBytes);
        InterlockedIncrement(&g_cbvRecorded);
        NoteCbv(desc->BufferLocation, desc->SizeInBytes);
        // Size census: which view sizes exist at all. The ==512 assumption was never checked.
        const UINT s = desc->SizeInBytes;
        if (s == 512) InterlockedIncrement(&g_size512);
        else if (s < 512) InterlockedIncrement(&g_sizeSmall);
        else if (s <= 4096) InterlockedIncrement(&g_sizeMid);
        else InterlockedIncrement(&g_sizeBig);
    }
    g_realCreateCbv(device, desc, dest);
}

HRESULT STDMETHODCALLTYPE HookedCreateDescriptorHeap(ID3D12Device* device,
                                                     const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                                     REFIID riid, void** out)
{
    const HRESULT hr = g_realCreateDescriptorHeap(device, desc, riid, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && desc != nullptr &&
        desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0)
    {
        ID3D12DescriptorHeap* heap = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&heap))) &&
            heap != nullptr)
        {
            RegisterCbvHeap(heap, device->GetDescriptorHandleIncrementSize(
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
            heap->Release();
        }
    }
    return hr;
}

// Catches heaps missed at creation - whatever made them, the game has to bind them here
// before any draw can use them. This closed a 20% hole in handle conversion (2.4M invisible
// table binds per session) that the main scene pass was hiding inside.
using SetDescriptorHeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                      ID3D12DescriptorHeap* const*);
SetDescriptorHeapsFn g_realSetDescriptorHeaps = nullptr;

void STDMETHODCALLTYPE HookedSetDescriptorHeaps(ID3D12GraphicsCommandList* list, UINT count,
                                                ID3D12DescriptorHeap* const* heaps)
{
    if (heaps != nullptr)
    {
        for (UINT i = 0; i < count; ++i)
        {
            ID3D12DescriptorHeap* heap = heaps[i];
            if (heap == nullptr) continue;
            // Pointer scans against known and ignored heaps keep the hot path free of
            // GetDesc calls.
            const LONG known = g_heapCount < static_cast<LONG>(kMaxHeaps)
                                   ? g_heapCount
                                   : static_cast<LONG>(kMaxHeaps);
            bool seen = false;
            for (LONG k = 0; k < known && !seen; ++k) seen = g_knownHeapPtrs[k] == heap;
            const LONG ignored = g_ignoredHeapCount < 64 ? g_ignoredHeapCount : 64;
            for (LONG k = 0; k < ignored && !seen; ++k) seen = g_ignoredHeapPtrs[k] == heap;
            if (seen) continue;

            UINT increment = g_cbvIncrement;
            if (increment == 0)
            {
                ID3D12Device* device = nullptr;
                if (SUCCEEDED(heap->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
                {
                    increment = device->GetDescriptorHandleIncrementSize(
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    device->Release();
                }
            }
            if (RegisterCbvHeap(heap, increment))
            {
                InterlockedIncrement(&g_lateHeaps);
            }
            else
            {
                const LONG slot = InterlockedIncrement(&g_ignoredHeapCount) - 1;
                if (slot < 64) g_ignoredHeapPtrs[slot] = heap;
                else InterlockedDecrement(&g_ignoredHeapCount);
            }
        }
    }
    g_realSetDescriptorHeaps(list, count, heaps);
}

// Every constant buffer bind for graphics passes through here. Most are materials, lights,
// bones; the one that validates as cb12 with a full-size render target is the scene camera,
// and unlike the ring sweep this is not a guess about which copy matters.
void STDMETHODCALLTYPE HookedSetGraphicsRootCbv(ID3D12GraphicsCommandList* list, UINT rootIndex,
                                                D3D12_GPU_VIRTUAL_ADDRESS gpu)
{
    if (g_enabled)
    {
        InterlockedIncrement(&g_cbvSeen);
        unsigned char* cpu = TranslateGpuAddress(gpu);
        if (cpu != nullptr)
        {
            InterlockedIncrement(&g_cbvResolved);
            Mat4 v = {};
            Mat4 p = {};
            if (IsGameplayCb12(cpu, &v, &p))
            {
                g_boundCamera = cpu;
                InterlockedIncrement(&g_boundHits);

                // Patch it here, at the bind, rather than at submit. This is the last moment
                // the value is definitely still the patched one and definitely the one these draws use.
                v.m[3] += g_offset;
                const Mat4 vp = Multiply(p, v);
                WriteBytes(cpu + kOffV, v.m, sizeof(v.m));
                WriteBytes(cpu + kOffVP, vp.m, sizeof(vp.m));
                ++g_patches;
            }
        }
    }
    g_realSetGraphicsRootCbv(list, rootIndex, gpu);
}

void TrackUploadBuffer(void* resourceOut, UINT64 width);

// Mapping a resource from inside the call that creates it reenters the driver mid-create.
// That is what crashes the moment any resource-creation hook is enabled (HookMask=125 dies,
// 124 is stable), and it is a bad - Hinny that GOAT - idea regardless of whether a driver tolerates it. The hook
// now only takes a reference and records the size; the Map and the address query happen later,
// on the present thread, where nothing is in the middle of constructing anything.
struct PendingUpload
{
    ID3D12Resource* resource;
    UINT64 width;
};
constexpr size_t kMaxPending = 8192;
PendingUpload g_pending[kMaxPending];
volatile LONG g_pendingCount = 0;
volatile LONG g_pendingDropped = 0;

// Every allocation hook already receives the heap properties, so whether the CPU can write the
// buffer is known without asking the resource and without speculatively mapping it. Mapping
// from inside the creation hook reenters the driver mid-create, thousands of times a second
// once the slots were right, and that is what stopped the launch.
bool HeapIsCpuWritable(const D3D12_HEAP_PROPERTIES* props)
{
    if (props == nullptr) return false;
    if (props->Type == D3D12_HEAP_TYPE_UPLOAD) return true;
    return props->Type == D3D12_HEAP_TYPE_CUSTOM &&
           props->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
           props->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
}

bool PlacedHeapIsCpuWritable(ID3D12Heap* heap)
{
    if (heap == nullptr) return false;
    const D3D12_HEAP_DESC desc = heap->GetDesc();
    return HeapIsCpuWritable(&desc.Properties);
}

HRESULT STDMETHODCALLTYPE HookedCreateCommittedResource(
    ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out)
{
    const HRESULT hr = g_realCreateCommitted(device, heap, flags, desc, state, clear, riid, out);
    // No heap-type filter. 10.5M descriptors resolved to buffer addresses and only 15 of them
    // landed in anything tracked, because the filter only accepted D3D12_HEAP_TYPE_UPLOAD. An
    // engine that wants control over write combining allocates HEAP_TYPE_CUSTOM instead, which
    // is exactly what the ring memory measured as. Map is the honest test of "can the CPU
    // write this": it succeeds on upload and CPU-visible custom heaps and fails on DEFAULT.
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && desc != nullptr &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        // Mapping an upload buffer the game also maps is safe: D3D12 refcounts Map and hands
        // back the same pointer, and nothing ever unmaps.
        if (HeapIsCpuWritable(heap)) TrackUploadBuffer(*out, desc->Width);
    }
    return hr;
}

// Shared by both creation paths: map the buffer and remember its GPU range against the CPU
// pointer, so a bound GPU address can be turned back into writable bytes.
// Called from inside a resource-creation hook. Takes a reference and leaves: no Map, no
// address query, nothing that reenters the driver while it is still building the resource.
void TrackUploadBuffer(void* resourceOut, UINT64 width)
{
    if (g_passthrough) return;
    ID3D12Resource* resource = nullptr;
    if (FAILED(static_cast<IUnknown*>(resourceOut)->QueryInterface(IID_PPV_ARGS(&resource))) ||
        resource == nullptr)
        return;

    const LONG slot = InterlockedIncrement(&g_pendingCount) - 1;
    if (slot < static_cast<LONG>(kMaxPending))
    {
        g_pending[slot].width = width;
        // Published last: the drain treats a non-null resource as "this entry is complete".
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&g_pending[slot].resource),
                                   resource);
    }
    else
    {
        InterlockedDecrement(&g_pendingCount);
        InterlockedIncrement(&g_pendingDropped);
        resource->Release();
    }
}

// Runs on the present thread, where nothing is mid-create and Map is safe.
void DrainPendingUploads()
{
    const LONG count = g_pendingCount;
    const LONG take = count > static_cast<LONG>(kMaxPending) ? static_cast<LONG>(kMaxPending) : count;
    for (LONG i = 0; i < take; ++i)
    {
        ID3D12Resource* resource = static_cast<ID3D12Resource*>(InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(&g_pending[i].resource), nullptr));
        if (resource == nullptr) continue;

        void* mapped = nullptr;
        D3D12_RANGE nothing = {0, 0};
        if (SUCCEEDED(resource->Map(0, &nothing, &mapped)) && mapped != nullptr)
        {
            const LONG slot = InterlockedIncrement(&g_uploadCount) - 1;
            if (slot < static_cast<LONG>(kMaxUploads))
            {
                g_uploads[slot].gpuStart = resource->GetGPUVirtualAddress();
                g_uploads[slot].size = g_pending[i].width;
                g_uploads[slot].cpu = static_cast<unsigned char*>(mapped);
            }
            else
            {
                InterlockedDecrement(&g_uploadCount);
            }
        }
        resource->Release();
    }
    // Entries are consumed in place; reset only once the whole batch is drained.
    if (take == count) InterlockedExchange(&g_pendingCount, 0);
}

HRESULT STDMETHODCALLTYPE HookedCreatePlacedResource(ID3D12Device* device, ID3D12Heap* heap,
                                                     UINT64 heapOffset,
                                                     const D3D12_RESOURCE_DESC* desc,
                                                     D3D12_RESOURCE_STATES state,
                                                     const D3D12_CLEAR_VALUE* clear, REFIID riid,
                                                     void** out)
{
    const HRESULT hr =
        g_realCreatePlaced(device, heap, heapOffset, desc, state, clear, riid, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && desc != nullptr &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        // Same reasoning as the committed path: try to map it and let that decide, rather than
        // insisting the heap be declared UPLOAD.
        if (PlacedHeapIsCpuWritable(heap)) TrackUploadBuffer(*out, desc->Width);
    }
    return hr;
}

// Only ~1% of bound buffers translated even with everything hooked at device init, and the
// reason is version. This game runs on ID3D12Device10, and the newer allocation entry points
// are separate vtable slots: CreateCommittedResource / CreatePlacedResource are the D3D12.0
// spellings, and an engine on Device8/10 calls the numbered variants instead.
//
// D3D12_RESOURCE_DESC and D3D12_RESOURCE_DESC1 share their leading fields (Dimension,
// Alignment, Width), so one reader serves every variant and the later descs need no headers.
UINT64 BufferWidthIfBuffer(const void* descPtr)
{
    if (descPtr == nullptr) return 0;
    const auto* desc = static_cast<const D3D12_RESOURCE_DESC*>(descPtr);
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return 0;
    return desc->Width;
}

using Committed1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                 D3D12_HEAP_FLAGS, const void*,
                                                 D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                                 void*, REFIID, void**);
using Committed3Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                 D3D12_HEAP_FLAGS, const void*, UINT32,
                                                 const D3D12_CLEAR_VALUE*, void*, UINT32,
                                                 const DXGI_FORMAT*, REFIID, void**);
using Placed1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const void*,
                                              D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                              REFIID, void**);
using Placed2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const void*,
                                              UINT32, const D3D12_CLEAR_VALUE*, UINT32,
                                              const DXGI_FORMAT*, REFIID, void**);

Committed1Fn g_realCommitted1 = nullptr;
Committed1Fn g_realCommitted2 = nullptr;
Committed3Fn g_realCommitted3 = nullptr;
Placed1Fn g_realPlaced1 = nullptr;
Placed2Fn g_realPlaced2 = nullptr;

// CreateCommittedResource1 and 2 differ only in desc type, which this reads generically.
HRESULT STDMETHODCALLTYPE HookedCommitted1(ID3D12Device* device,
                                           const D3D12_HEAP_PROPERTIES* heap,
                                           D3D12_HEAP_FLAGS flags, const void* desc,
                                           D3D12_RESOURCE_STATES state,
                                           const D3D12_CLEAR_VALUE* clear, void* session,
                                           REFIID riid, void** out)
{
    const HRESULT hr = g_realCommitted1(device, heap, flags, desc, state, clear, session, riid, out);
    const UINT64 width = BufferWidthIfBuffer(desc);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && width != 0 && HeapIsCpuWritable(heap))
            TrackUploadBuffer(*out, width);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCommitted2(ID3D12Device* device,
                                           const D3D12_HEAP_PROPERTIES* heap,
                                           D3D12_HEAP_FLAGS flags, const void* desc,
                                           D3D12_RESOURCE_STATES state,
                                           const D3D12_CLEAR_VALUE* clear, void* session,
                                           REFIID riid, void** out)
{
    const HRESULT hr = g_realCommitted2(device, heap, flags, desc, state, clear, session, riid, out);
    const UINT64 width = BufferWidthIfBuffer(desc);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && width != 0 && HeapIsCpuWritable(heap))
            TrackUploadBuffer(*out, width);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCommitted3(ID3D12Device* device,
                                           const D3D12_HEAP_PROPERTIES* heap,
                                           D3D12_HEAP_FLAGS flags, const void* desc, UINT32 layout,
                                           const D3D12_CLEAR_VALUE* clear, void* session,
                                           UINT32 numCastable, const DXGI_FORMAT* castable,
                                           REFIID riid, void** out)
{
    const HRESULT hr = g_realCommitted3(device, heap, flags, desc, layout, clear, session,
                                        numCastable, castable, riid, out);
    const UINT64 width = BufferWidthIfBuffer(desc);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && width != 0 && HeapIsCpuWritable(heap))
            TrackUploadBuffer(*out, width);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedPlaced1(ID3D12Device* device, ID3D12Heap* heap, UINT64 offset,
                                        const void* desc, D3D12_RESOURCE_STATES state,
                                        const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out)
{
    const HRESULT hr = g_realPlaced1(device, heap, offset, desc, state, clear, riid, out);
    const UINT64 width = BufferWidthIfBuffer(desc);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && width != 0 && PlacedHeapIsCpuWritable(heap))
            TrackUploadBuffer(*out, width);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedPlaced2(ID3D12Device* device, ID3D12Heap* heap, UINT64 offset,
                                        const void* desc, UINT32 layout,
                                        const D3D12_CLEAR_VALUE* clear, UINT32 numCastable,
                                        const DXGI_FORMAT* castable, REFIID riid, void** out)
{
    const HRESULT hr = g_realPlaced2(device, heap, offset, desc, layout, clear, numCastable,
                                     castable, riid, out);
    const UINT64 width = BufferWidthIfBuffer(desc);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && width != 0 && PlacedHeapIsCpuWritable(heap))
            TrackUploadBuffer(*out, width);
    return hr;
}

// ---- WriteBufferImmediate probe -------------------------------------------------------------
//
// The descriptor chain names cb12 by GPU virtual address every frame, and the one blocker is
// that nothing can turn that address into CPU-writable memory. WriteBufferImmediate does not
// need to: it writes 32-bit values at a raw GPU VA from a command list, executed on the GPU
// timeline. Submitted on the same queue immediately before the game's own lists, the write
// lands after the game's CPU wrote the camera and before any draw consumes it.
//
// This probe zeroes c_matVP in every 512-byte CBV the frame binds. A zero view-projection
// collapses all geometry that consumes it, so the verdict is one look at the flat screen:
// scene falls apart = the camera is writable and Phase 2 is worth building; scene untouched =
// the resolved addresses or the timing are wrong, and the frame capture is back on.

// Candidates are collected at bind time (SetGraphicsRootDescriptorTable) and reset each
// present. Writing a stale ring slot is harmless - the GPU already consumed it - so the reset
// only exists to keep the list current, not to protect anything.
constexpr LONG kMaxProbeCandidates = 512;
D3D12_GPU_VIRTUAL_ADDRESS g_probeCandidates[kMaxProbeCandidates] = {};
volatile LONG g_probeCandidateCount = 0;
volatile LONG g_probeCandDropped = 0;
volatile LONG g_probeSubmits = 0;
volatile LONG g_probeSkips = 0;
LONG g_probeCandsLastFrame = 0;

void AddProbeCandidate(D3D12_GPU_VIRTUAL_ADDRESS gpu)
{
    // Racy dedup: two threads can both miss and insert the same address. A duplicate write
    // costs 16 dwords of GPU work; a lock on the bind path costs the frame rate.
    const LONG seen = g_probeCandidateCount;
    const LONG limit = seen < kMaxProbeCandidates ? seen : kMaxProbeCandidates;
    for (LONG i = 0; i < limit; ++i)
        if (g_probeCandidates[i] == gpu) return;
    const LONG slot = InterlockedIncrement(&g_probeCandidateCount) - 1;
    if (slot < kMaxProbeCandidates)
    {
        g_probeCandidates[slot] = gpu;
    }
    else
    {
        InterlockedDecrement(&g_probeCandidateCount);
        InterlockedIncrement(&g_probeCandDropped);
    }
}

// A small ring of allocator+list pairs, reused only once the GPU has finished with them. The
// probe never waits: if every slot is still in flight it skips this submission and counts the
// skip, because blocking the submit thread is how earlier builds hung the game.
struct ProbeSlot
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList2* list = nullptr;
    UINT64 fenceValue = 0;
};
constexpr size_t kProbeSlots = 8;
ProbeSlot g_probeSlots[kProbeSlots];
ID3D12Fence* g_probeFence = nullptr;
UINT64 g_probeFenceNext = 0;
bool g_probeInitFailed = false;
volatile LONG g_probeBusy = 0;

bool InitProbeObjects(ID3D12CommandQueue* queue)
{
    if (g_probeFence != nullptr) return true;
    if (g_probeInitFailed) return false;
    g_probeInitFailed = true; // cleared only on full success, so a partial init never retries

    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        LAD8VR::Log::Line("[PROBE] GetDevice failed, probe disabled");
        return false;
    }

    bool ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_probeFence)));
    for (size_t i = 0; ok && i < kProbeSlots; ++i)
    {
        ok = SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&g_probeSlots[i].allocator)));
        if (!ok) break;
        ID3D12GraphicsCommandList* list = nullptr;
        ok = SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 g_probeSlots[i].allocator, nullptr,
                                                 IID_PPV_ARGS(&list))) &&
             list != nullptr;
        if (ok)
        {
            // WriteBufferImmediate lives on ID3D12GraphicsCommandList2. The Agility SDK ships
            // in the game folder, so this QI failing would itself be news worth logging.
            ok = SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&g_probeSlots[i].list)));
            list->Close(); // lists are created open; close so every use starts with Reset
            list->Release();
        }
    }
    device->Release();

    if (!ok)
    {
        LAD8VR::Log::Line("[PROBE] init failed (no GraphicsCommandList2?), probe disabled");
        return false;
    }
    g_probeInitFailed = false;
    LAD8VR::Log::Line("[PROBE] objects ready, zeroing c_matVP of every bound 512-byte CBV");
    return true;
}

// Slot acquire/submit split out so the patch pipeline can ride the same fence ring. Callers
// must hold g_probeBusy across the pair; the slot index only advances at submit.
ID3D12GraphicsCommandList2* AcquireProbeList(ID3D12CommandQueue* queue)
{
    if (!InitProbeObjects(queue)) return nullptr;
    ProbeSlot& slot = g_probeSlots[g_probeFenceNext % kProbeSlots];
    if (slot.fenceValue != 0 && g_probeFence->GetCompletedValue() < slot.fenceValue)
    {
        InterlockedIncrement(&g_probeSkips);
        return nullptr;
    }
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator, nullptr)))
        return nullptr;
    return slot.list;
}

void SubmitAcquiredList(ID3D12CommandQueue* queue)
{
    ProbeSlot& slot = g_probeSlots[g_probeFenceNext % kProbeSlots];
    if (FAILED(slot.list->Close())) return;
    ID3D12CommandList* lists[] = {static_cast<ID3D12CommandList*>(slot.list)};
    // Through the trampoline, not the vtable, or the proxy's own ExecuteCommandLists hook re-enters.
    g_realExecute(queue, 1, lists);
    slot.fenceValue = ++g_probeFenceNext;
    queue->Signal(g_probeFence, slot.fenceValue);
    InterlockedIncrement(&g_probeSubmits);
}

void SubmitProbe(ID3D12CommandQueue* queue)
{
    const LONG candCount = g_probeCandidateCount;
    const LONG cands = candCount < kMaxProbeCandidates ? candCount : kMaxProbeCandidates;
    if (cands == 0) return;

    ID3D12GraphicsCommandList2* list = AcquireProbeList(queue);
    if (list == nullptr) return;

    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[16];
    for (LONG c = 0; c < cands; ++c)
    {
        if (g_probe)
        {
            // Destructive probe: zero the 16 dwords of c_matVP. A zero view-projection
            // collapses all geometry that consumes it - the Phase 1 verdict, one look.
            const D3D12_GPU_VIRTUAL_ADDRESS vp = g_probeCandidates[c] + kOffVP;
            for (UINT d = 0; d < 16; ++d)
            {
                params[d].Dest = vp + d * 4;
                params[d].Value = 0;
            }
            // Default mode: no barrier semantics, executes in command order, which on this
            // queue is before the game's lists.
            list->WriteBufferImmediate(16, params, nullptr);
        }
        else
        {
            // Calibration stamp: the buffer's own VA written into its padding, invisible to
            // every shader, read back CPU-side by TryCalibrate via the ring sweep.
            const D3D12_GPU_VIRTUAL_ADDRESS va = g_probeCandidates[c];
            params[0].Dest = va + kOffStamp;
            params[0].Value = kStampMagic;
            params[1].Dest = va + kOffStamp + 4;
            params[1].Value = static_cast<UINT>(va & 0xFFFFFFFFull);
            params[2].Dest = va + kOffStamp + 8;
            params[2].Value = static_cast<UINT>(va >> 32);
            list->WriteBufferImmediate(3, params, nullptr);
        }
    }
    SubmitAcquiredList(queue);
}

void MaybeProbe(ID3D12CommandQueue* queue)
{
    // One submitter at a time; a second concurrent queue skips rather than waits. Loading
    // screens run copy queues through this same hook, so the type check is not optional.
    if (InterlockedCompareExchange(&g_probeBusy, 1, 0) != 0) return;
    const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) SubmitProbe(queue);
    InterlockedExchange(&g_probeBusy, 0);
}

// ---- delta calibration and the patch pipeline it unlocks ------------------------------------
//
// The ring sweep locates live cb12 copies by CPU address; the descriptor chain names them by
// GPU VA; nothing ever married the two. The stamp does: each candidate carries its own VA in
// its padding, so the first time the sweep lands on a stamped buffer, VA - CPU falls out. One
// resource is one contiguous VA range, so that difference holds for the whole ring region.

struct RegionDelta
{
    const unsigned char* base;
    size_t size;
    long long delta; // (long long)VA - (long long)CPU, signed on purpose
    bool valid;
};
constexpr LONG kMaxDeltas = 8;
RegionDelta g_deltas[kMaxDeltas] = {};
volatile LONG g_deltaCount = 0;

// Stamping exists to find the first delta; after that its submissions are pure frame-time
// cost with nothing left to learn.
bool StampingActive() { return g_calibrate && g_deltaCount == 0; }

// Out-of-ring 512-byte CBV targets seen this frame. Unreadable from the CPU, so they get the
// live gameplay camera written blind on the GPU timeline. Some will not be cameras; a probe
// build accepts the glitches as information.
constexpr LONG kMaxBlind = 512;
D3D12_GPU_VIRTUAL_ADDRESS g_blindVas[kMaxBlind] = {};
volatile LONG g_blindCount = 0;
volatile LONG g_blindDropped = 0;
LONG g_blindLastFrame = 0;

// Boot-time descriptors never pass through NoteCbv again - calibration was not live when
// they were created, so the per-frame blind list never sees them. The CreateCBV hook records
// every descriptor from the first frame of the process unconditionally; this list is that
// record, swept once when the delta lands, written blind every frame thereafter.
constexpr LONG kMaxPersistentBlind = 1024;
D3D12_GPU_VIRTUAL_ADDRESS g_persistentBlind[kMaxPersistentBlind] = {};
volatile LONG g_persistentBlindCount = 0;
volatile LONG g_persistentBlindDropped = 0;

void AddBlindVa(D3D12_GPU_VIRTUAL_ADDRESS gpu)
{
    const LONG seen = g_blindCount;
    const LONG limit = seen < kMaxBlind ? seen : kMaxBlind;
    for (LONG i = 0; i < limit; ++i)
        if (g_blindVas[i] == gpu) return;
    const LONG slot = InterlockedIncrement(&g_blindCount) - 1;
    if (slot < kMaxBlind)
    {
        g_blindVas[slot] = gpu;
    }
    else
    {
        InterlockedDecrement(&g_blindCount);
        InterlockedIncrement(&g_blindDropped);
    }
}

// >= 512, not == 512: a CBV only has to be at least 492 bytes to satisfy cb12, and the exact
// filter silently dropped every larger view all day. Ring candidates are content-validated
// before patching, so the widening costs false blind writes at worst.
void NoteCbv(D3D12_GPU_VIRTUAL_ADDRESS gpu, UINT size)
{
    if (size < 512 || gpu == 0 || !g_calibrate || g_deltaCount == 0) return;
    unsigned char* bytes = TranslateViaDelta(gpu);
    if (bytes != nullptr)
    {
        InterlockedIncrement(&g_translated);
        EnqueueCandidate(bytes);
    }
    else
    {
        NoteMysteryVa(gpu);
        AddBlindVa(gpu);
    }
}

volatile LONG g_frameId = 1;

// Candidates translated at bind time, validated and patched once per frame at submit time.
// The bind path does only a 4-byte stamp check, because it runs millions of times a second
// and the ring is write-combined memory where every read costs ~100x normal RAM.
constexpr LONG kMaxPendingPatch = 4096;
unsigned char* g_pendingPatch[kMaxPendingPatch] = {};
volatile LONG g_pendingPatchCount = 0;
volatile LONG g_pendingPatchDropped = 0;
volatile LONG g_patchProcessed = 0;
volatile LONG g_patchBusy = 0;
LONG g_pendingLastFrame = 0;

unsigned char* TranslateViaDelta(D3D12_GPU_VIRTUAL_ADDRESS gpu)
{
    const LONG n = g_deltaCount < kMaxDeltas ? g_deltaCount : kMaxDeltas;
    for (LONG i = 0; i < n; ++i)
    {
        const RegionDelta& r = g_deltas[i];
        if (!r.valid) continue;
        const long long cpu = static_cast<long long>(gpu) - r.delta;
        const long long base = reinterpret_cast<long long>(r.base);
        if (cpu >= base && cpu < base + static_cast<long long>(r.size))
            return reinterpret_cast<unsigned char*>(cpu);
    }
    return nullptr;
}

void EnqueueCandidate(unsigned char* cpu)
{
    UINT stamp = 0;
    if (!ReadBytes(cpu + kOffFrameStamp, &stamp, sizeof(stamp))) return;
    const UINT frame = static_cast<UINT>(g_frameId);
    if (stamp == frame) return;
    WriteBytes(cpu + kOffFrameStamp, &frame, sizeof(frame));
    const LONG slot = InterlockedIncrement(&g_pendingPatchCount) - 1;
    if (slot < kMaxPendingPatch)
    {
        g_pendingPatch[slot] = cpu;
    }
    else
    {
        InterlockedDecrement(&g_pendingPatchCount);
        InterlockedIncrement(&g_pendingPatchDropped);
    }
}

D3D12_GPU_VIRTUAL_ADDRESS CpuToVa(const unsigned char* cpu)
{
    const LONG n = g_deltaCount < kMaxDeltas ? g_deltaCount : kMaxDeltas;
    for (LONG i = 0; i < n; ++i)
    {
        const RegionDelta& r = g_deltas[i];
        if (!r.valid || cpu < r.base || cpu >= r.base + r.size) continue;
        return static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(
            reinterpret_cast<long long>(cpu) + r.delta);
    }
    return 0;
}

// What actually got patched, by render-target width. The run where the world refused to move
// could not say whether the 3840-wide camera was being patched and overwritten, or never
// patched at all - two opposite bugs. This names it in one log line.
volatile LONG g_patchW3840 = 0;
volatile LONG g_patchW2560 = 0;
volatile LONG g_patchW1920 = 0;
volatile LONG g_patchWOther = 0;
volatile LONG g_patchWbiWrites = 0;
volatile LONG g_patchWbiSkips = 0;

// Persistent out-of-ring camera buffers (the ones the main pass actually reads) and the live
// camera values to write into them. Filled by the mystery tracker further down; consumed by
// ProcessPendingCandidates, so declared here.
D3D12_GPU_VIRTUAL_ADDRESS g_hotMystery[8] = {};
volatile LONG g_hotCount = 0;
volatile LONG g_copyMatches = 0;
volatile LONG g_copySrcPatched = 0;
volatile LONG g_mystWrites = 0;
Mat4 g_liveV = {};
Mat4 g_liveVP = {};
volatile LONG g_liveFrame = 0;

// One WBI param per dword of V (16) and VP (16) per patched buffer. Sized for ~500 ring
// patches plus the full blind list in one frame; silently dropping the tail here would
// silently drop the main camera.
constexpr LONG kMaxPatchParams = 65536;
D3D12_WRITEBUFFERIMMEDIATE_PARAMETER g_patchParams[kMaxPatchParams];
LONG g_patchParamCount = 0; // only touched under g_patchBusy

void AppendPatchParams(D3D12_GPU_VIRTUAL_ADDRESS va, const Mat4& v, const Mat4& vp)
{
    if (g_patchParamCount + 32 > kMaxPatchParams) return;
    for (int d = 0; d < 16; ++d)
    {
        UINT bits = 0;
        memcpy(&bits, &v.m[d], sizeof(bits));
        g_patchParams[g_patchParamCount].Dest = va + kOffV + d * 4;
        g_patchParams[g_patchParamCount].Value = bits;
        ++g_patchParamCount;
    }
    for (int d = 0; d < 16; ++d)
    {
        UINT bits = 0;
        memcpy(&bits, &vp.m[d], sizeof(bits));
        g_patchParams[g_patchParamCount].Dest = va + kOffVP + d * 4;
        g_patchParams[g_patchParamCount].Value = bits;
        ++g_patchParamCount;
    }
}

// Runs in the ECL hook, before the submission is forwarded: every enqueued candidate is still
// unread by the GPU at this point. Two bind threads can race the frame stamp and enqueue the
// same buffer twice; the patch stamp makes the patch itself idempotent, which matters because
// the eye offset is applied with +=.
//
// The patch is written twice, on purpose. The CPU write is immediate; the WBI list re-writes
// the same absolute values on the GPU timeline just before the draws, so if the game's CPU
// rewrites the buffer after this hook - the leading suspect for the main camera refusing to
// move - the GPU-side write still lands last and wins.
void ProcessPendingCandidates(ID3D12CommandQueue* queue)
{
    if (g_deltaCount == 0) return;
    if (InterlockedCompareExchange(&g_patchBusy, 1, 0) != 0) return;

    const LONG count =
        g_pendingPatchCount < kMaxPendingPatch ? g_pendingPatchCount : kMaxPendingPatch;
    const UINT frame = static_cast<UINT>(g_frameId);
    g_patchParamCount = 0;
    for (LONG i = g_patchProcessed; i < count; ++i)
    {
        unsigned char* cpu = g_pendingPatch[i];
        if (cpu == nullptr) continue;
        UINT patched = 0;
        if (!ReadBytes(cpu + kOffPatchStamp, &patched, sizeof(patched)) || patched == frame)
            continue;
        WriteBytes(cpu + kOffPatchStamp, &frame, sizeof(frame));

        // The ring holds ~2000 valid cb12 copies a frame (cascades, probes, mip pyramids).
        // Reading 200+ bytes of write-combined memory for each would eat the submit thread,
        // so an 8-byte render-target gate goes first: only the large views - the main scene
        // pass and its full/half-res post copies - are worth the full read.
        float rt[2] = {};
        if (!ReadBytes(cpu + kOffRtSize, rt, sizeof(rt))) continue;
        if (rt[0] < 1920.0f || rt[1] < 1080.0f) continue;
        if (static_cast<LONG>(rt[0]) > g_widestRt) g_widestRt = static_cast<LONG>(rt[0]);

        Mat4 v = {};
        Mat4 p = {};
        if (!IsGameplayCb12(cpu, &v, &p)) continue;
        g_boundCamera = cpu;
        InterlockedIncrement(&g_boundHits);
        v.m[3] += g_offset;
        const Mat4 vp = Multiply(p, v);
        WriteBytes(cpu + kOffV, v.m, sizeof(v.m));
        WriteBytes(cpu + kOffVP, vp.m, sizeof(vp.m));
        ++g_patches;

        const LONG w = static_cast<LONG>(rt[0]);
        if (w >= 3840) InterlockedIncrement(&g_patchW3840);
        else if (w >= 2560) InterlockedIncrement(&g_patchW2560);
        else if (w >= 1920) InterlockedIncrement(&g_patchW1920);
        else InterlockedIncrement(&g_patchWOther);

        if (w >= 3840)
        {
            // v/vp already carry the eye offset here - these are the values the persistent
            // camera buffers should hold.
            g_liveV = v;
            g_liveVP = vp;
            g_liveFrame = static_cast<LONG>(frame);
        }

        const D3D12_GPU_VIRTUAL_ADDRESS va = CpuToVa(cpu);
        if (va != 0) AppendPatchParams(va, v, vp);
    }
    g_patchProcessed = count;

    // Every out-of-ring camera-sized CBV target this frame gets the live gameplay camera
    // written blind on the GPU timeline, just before the draws, after anything the game's
    // CPU wrote. The values are stale by at most one frame on the first submissions, which
    // camera continuity forgives. Some of these are not cameras; the glitches are data.
    const bool liveFresh = g_liveFrame != 0 &&
                           static_cast<UINT>(g_liveFrame) + 1 >= static_cast<UINT>(g_frameId);
    const LONG blind = g_blindCount < kMaxBlind ? g_blindCount : kMaxBlind;
    if (blind > 0 && liveFresh)
    {
        for (LONG i = 0; i < blind; ++i)
        {
            AppendPatchParams(g_blindVas[i], g_liveV, g_liveVP);
            InterlockedIncrement(&g_mystWrites);
        }
    }
    // The boot-swept persistent targets, every frame: their descriptors were written once at
    // process start and will never pass through a hook again.
    const LONG persistent = g_persistentBlindCount < kMaxPersistentBlind
                                ? g_persistentBlindCount
                                : kMaxPersistentBlind;
    if (persistent > 0 && liveFresh)
        for (LONG i = 0; i < persistent; ++i)
            AppendPatchParams(g_persistentBlind[i], g_liveV, g_liveVP);

    if (g_patchParamCount > 0 && queue != nullptr)
    {
        if (InterlockedCompareExchange(&g_probeBusy, 1, 0) == 0)
        {
            ID3D12GraphicsCommandList2* list = AcquireProbeList(queue);
            if (list != nullptr)
            {
                for (LONG p = 0; p < g_patchParamCount; p += 16)
                {
                    const LONG chunk =
                        g_patchParamCount - p < 16 ? g_patchParamCount - p : 16;
                    list->WriteBufferImmediate(static_cast<UINT>(chunk), &g_patchParams[p],
                                               nullptr);
                }
                SubmitAcquiredList(queue);
                InterlockedIncrement(&g_patchWbiWrites);
            }
            else
            {
                InterlockedIncrement(&g_patchWbiSkips);
            }
            InterlockedExchange(&g_probeBusy, 0);
        }
        else
        {
            InterlockedIncrement(&g_patchWbiSkips);
        }
    }
    InterlockedExchange(&g_patchBusy, 0);
}

// Present-time, a few hundred guarded 4-byte reads around the sweep's live camera. Once the
// camera's region has a delta the stamping stops - its work is done and its submissions cost
// frame time.
void TryCalibrate()
{
    if (!g_calibrate || g_deltaCount > 0) return;
    const unsigned char* bases[8] = {};
    size_t sizes[8] = {};
    const size_t rc = LAD8VR::Camera::GetRingRegions(bases, sizes, 8);
    const unsigned char* seed = LAD8VR::Camera::CurrentCameraAddress();
    if (rc == 0 || seed == nullptr) return;

    for (size_t r = 0; r < rc; ++r)
    {
        const unsigned char* end = bases[r] + sizes[r];
        if (seed < bases[r] || seed >= end) continue;

        constexpr size_t kWindow = 64u << 10;
        const unsigned char* from =
            static_cast<size_t>(seed - bases[r]) > kWindow ? seed - kWindow : bases[r];
        const unsigned char* to = seed + kWindow < end - 512 ? seed + kWindow : end - 512;
        for (const unsigned char* a = from; a <= to; a += kAlign)
        {
            UINT magic = 0, lo = 0, hi = 0;
            if (!ReadBytes(a + kOffStamp, &magic, sizeof(magic)) || magic != kStampMagic)
                continue;
            if (!ReadBytes(a + kOffStamp + 4, &lo, sizeof(lo)) ||
                !ReadBytes(a + kOffStamp + 8, &hi, sizeof(hi)))
                continue;
            const ULONGLONG va = (static_cast<ULONGLONG>(hi) << 32) | lo;
            const long long delta =
                static_cast<long long>(va) - reinterpret_cast<long long>(a);
            const LONG slot = InterlockedIncrement(&g_deltaCount) - 1;
            if (slot < kMaxDeltas)
            {
                g_deltas[slot].base = bases[r];
                g_deltas[slot].size = sizes[r];
                g_deltas[slot].delta = delta;
                g_deltas[slot].valid = true;
                char line[200] = {};
                sprintf_s(line,
                          "[CAL] delta found: region 0x%p (%zu MB) VA-CPU=0x%llx, translation "
                          "is live",
                          static_cast<const void*>(bases[r]), sizes[r] >> 20,
                          static_cast<unsigned long long>(delta));
                LAD8VR::Log::Line(line);

                // Boot sweep: every camera-sized CBV recorded since process start whose
                // buffer sits outside the ring becomes a permanent blind-write target. This
                // is the only coverage for descriptors created before calibration went live.
                LONG swept = 0;
                for (size_t e = 0; e < kCbvTableSize; ++e)
                {
                    if (g_cbvTable[e].cpuHandle == 0 || g_cbvTable[e].size < 512) continue;
                    const D3D12_GPU_VIRTUAL_ADDRESS va = g_cbvTable[e].gpu;
                    if (va == 0 || TranslateViaDelta(va) != nullptr) continue;
                    bool dup = false;
                    const LONG have = g_persistentBlindCount < kMaxPersistentBlind
                                          ? g_persistentBlindCount
                                          : kMaxPersistentBlind;
                    for (LONG d = 0; d < have && !dup; ++d) dup = g_persistentBlind[d] == va;
                    if (dup) continue;
                    const LONG bslot = InterlockedIncrement(&g_persistentBlindCount) - 1;
                    if (bslot < kMaxPersistentBlind)
                    {
                        g_persistentBlind[bslot] = va;
                        ++swept;
                    }
                    else
                    {
                        InterlockedDecrement(&g_persistentBlindCount);
                        InterlockedIncrement(&g_persistentBlindDropped);
                    }
                }
                char sweepLine[160] = {};
                sprintf_s(sweepLine, "[CAL] boot sweep: %ld persistent blind targets (%ld dropped)",
                          swept, g_persistentBlindDropped);
                LAD8VR::Log::Line(sweepLine);
            }
            else
            {
                InterlockedDecrement(&g_deltaCount);
            }
            return;
        }
        return;
    }
}

// ---- the mystery buffer: finding the camera the descriptor chain cannot see -----------------
//
// Eleven 3840-wide camera copies per frame take the patch - CPU write AND GPU-timeline write -
// and the world does not move. So the main pass reads a camera buffer that is none of them.
// The engine GPU-copies its per-pass constants from unbound staging slots into a default-heap
// buffer, and the main pass's CBV points at the destination. The staging source is never
// CBV-bound (copies need no descriptors), so the descriptor chain never names it.
//
// Two moves: persistent untranslatable CBV VAs identify the destination (a default-heap
// buffer keeps its VA; ring slots never repeat one), and the copy hooks catch the transfer,
// walk back to the source, and patch THAT - before the copy has executed.

struct MysteryVa
{
    D3D12_GPU_VIRTUAL_ADDRESS va;
    volatile LONG hits;
    volatile LONG frames;
    UINT lastFrame;
};
constexpr size_t kMysterySize = 256;
MysteryVa g_mystery[kMysterySize] = {};

void NoteMysteryVa(D3D12_GPU_VIRTUAL_ADDRESS va)
{
    size_t slot = static_cast<size_t>((va >> 8) * 0x9E3779B97F4A7C15ull) & (kMysterySize - 1);
    for (int probe = 0; probe < 8; ++probe)
    {
        MysteryVa& m = g_mystery[slot];
        if (m.va == va)
        {
            InterlockedIncrement(&m.hits);
            const UINT f = static_cast<UINT>(g_frameId);
            if (m.lastFrame != f)
            {
                m.lastFrame = f;
                InterlockedIncrement(&m.frames);
            }
            return;
        }
        if (m.va == 0)
        {
            m.va = va;
            m.hits = 1;
            m.frames = 1;
            m.lastFrame = static_cast<UINT>(g_frameId);
            return;
        }
        slot = (slot + 1) & (kMysterySize - 1);
    }
}

// Rebuilt every present from entries seen across 60+ distinct frames; the copy hooks scan
// only this short list, because they run at copy-record rate.
void RebuildHotMystery()
{
    LONG count = 0;
    for (size_t i = 0; i < kMysterySize && count < 8; ++i)
        if (g_mystery[i].va != 0 && g_mystery[i].frames >= 60) g_hotMystery[count++] = g_mystery[i].va;
    g_hotCount = count;
}

// The copy is recorded, not executed, when this runs - patching the staging source here means
// the GPU copy carries the patched camera into the buffer the main pass actually reads.
void PatchCopySource(ID3D12Resource* src, UINT64 cb12OffsetInSrc)
{
    if (src == nullptr) return;
    const D3D12_GPU_VIRTUAL_ADDRESS sva = src->GetGPUVirtualAddress();
    if (sva == 0) return;
    unsigned char* cpu = TranslateViaDelta(sva + cb12OffsetInSrc);
    if (cpu == nullptr) return;
    Mat4 v = {};
    Mat4 p = {};
    if (!IsGameplayCb12(cpu, &v, &p)) return;
    v.m[3] += g_offset;
    const Mat4 vp = Multiply(p, v);
    WriteBytes(cpu + kOffV, v.m, sizeof(v.m));
    WriteBytes(cpu + kOffVP, vp.m, sizeof(vp.m));
    InterlockedIncrement(&g_copySrcPatched);
}

using CopyBufferRegionFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*,
                                                    UINT64, ID3D12Resource*, UINT64, UINT64);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*,
                                                ID3D12Resource*);
CopyBufferRegionFn g_realCopyBufferRegion = nullptr;
CopyResourceFn g_realCopyResource = nullptr;

void STDMETHODCALLTYPE HookedCopyBufferRegion(ID3D12GraphicsCommandList* list,
                                              ID3D12Resource* dst, UINT64 dstOff,
                                              ID3D12Resource* src, UINT64 srcOff, UINT64 bytes)
{
    if (g_enabled && g_hotCount > 0 && dst != nullptr && src != nullptr && bytes >= kCbSize)
    {
        // GetGPUVirtualAddress returns 0 for textures, which rejects them for free.
        const D3D12_GPU_VIRTUAL_ADDRESS dva = dst->GetGPUVirtualAddress();
        if (dva != 0)
        {
            const LONG n = g_hotCount < 8 ? g_hotCount : 8;
            for (LONG i = 0; i < n; ++i)
            {
                const D3D12_GPU_VIRTUAL_ADDRESS mva = g_hotMystery[i];
                if (mva >= dva + dstOff && mva + kCbSize <= dva + dstOff + bytes)
                {
                    InterlockedIncrement(&g_copyMatches);
                    PatchCopySource(src, srcOff + (mva - (dva + dstOff)));
                }
            }
        }
    }
    g_realCopyBufferRegion(list, dst, dstOff, src, srcOff, bytes);
}

void STDMETHODCALLTYPE HookedCopyResource(ID3D12GraphicsCommandList* list, ID3D12Resource* dst,
                                          ID3D12Resource* src)
{
    if (g_enabled && g_hotCount > 0 && dst != nullptr && src != nullptr)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS dva = dst->GetGPUVirtualAddress();
        if (dva != 0)
        {
            const LONG n = g_hotCount < 8 ? g_hotCount : 8;
            for (LONG i = 0; i < n; ++i)
            {
                // Whole-resource copy, destination size unknown here: accept a generous span
                // and let the source-side cb12 validation be the real filter.
                const D3D12_GPU_VIRTUAL_ADDRESS mva = g_hotMystery[i];
                if (mva >= dva && mva - dva < (64ull << 20))
                {
                    InterlockedIncrement(&g_copyMatches);
                    PatchCopySource(src, mva - dva);
                }
            }
        }
    }
    g_realCopyResource(list, dst, src);
}

HRESULT STDMETHODCALLTYPE HookedCreateCommandList(ID3D12Device* device, UINT nodeMask,
                                                  D3D12_COMMAND_LIST_TYPE type,
                                                  ID3D12CommandAllocator* allocator,
                                                  ID3D12PipelineState* pso, REFIID riid, void** out)
{
    const HRESULT hr = g_realCreateCommandList(device, nodeMask, type, allocator, pso, riid, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && g_realSetGraphicsRootCbv == nullptr &&
        type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list))) &&
            list != nullptr)
        {
            // Slot 38: IUnknown 0-2, ID3D12Object 3-6, ID3D12DeviceChild 7,
            // ID3D12CommandList 8 GetType, then ID3D12GraphicsCommandList 9 Close ..
            // 37 SetComputeRootConstantBufferView, 38 SetGraphicsRootConstantBufferView.
            void** vtable = *reinterpret_cast<void***>(list);
            if (MH_CreateHook(vtable[38], &HookedSetGraphicsRootCbv,
                              reinterpret_cast<void**>(&g_realSetGraphicsRootCbv)) == MH_OK &&
                MH_EnableHook(vtable[38]) == MH_OK)
                LAD8VR::Log::Line("[STEREO] SetGraphicsRootConstantBufferView hooked");
            else
                LAD8VR::Log::Line("[STEREO] failed to hook SetGraphicsRootConstantBufferView");

            // Slot 32: SetGraphicsRootDescriptorTable. This is the one that matters here.
            if (MH_CreateHook(vtable[32], &HookedSetGraphicsRootTable,
                              reinterpret_cast<void**>(&g_realSetGraphicsRootTable)) == MH_OK &&
                MH_EnableHook(vtable[32]) == MH_OK)
                LAD8VR::Log::Line("[STEREO] SetGraphicsRootDescriptorTable hooked");
            else
                LAD8VR::Log::Line("[STEREO] failed to hook SetGraphicsRootDescriptorTable");

            // Slot 28: SetDescriptorHeaps - late registration of heaps the creation hook
            // never saw. This is the fix for the 20% handle-conversion hole.
            if (MH_CreateHook(vtable[28], &HookedSetDescriptorHeaps,
                              reinterpret_cast<void**>(&g_realSetDescriptorHeaps)) == MH_OK &&
                MH_EnableHook(vtable[28]) == MH_OK)
                LAD8VR::Log::Line("[STEREO] SetDescriptorHeaps hooked");
            else
                LAD8VR::Log::Line("[STEREO] failed to hook SetDescriptorHeaps");

            // Slots 15/17 (from d3d12.h's ID3D12GraphicsCommandListVtbl, not hand-counted):
            // the copy hooks that chase the main pass's camera back to its staging source.
            if (g_hookCopies)
            {
                if (MH_CreateHook(vtable[15], &HookedCopyBufferRegion,
                                  reinterpret_cast<void**>(&g_realCopyBufferRegion)) == MH_OK &&
                    MH_EnableHook(vtable[15]) == MH_OK)
                    LAD8VR::Log::Line("[STEREO] CopyBufferRegion hooked");
                else
                    LAD8VR::Log::Line("[STEREO] failed to hook CopyBufferRegion");
                if (MH_CreateHook(vtable[17], &HookedCopyResource,
                                  reinterpret_cast<void**>(&g_realCopyResource)) == MH_OK &&
                    MH_EnableHook(vtable[17]) == MH_OK)
                    LAD8VR::Log::Line("[STEREO] CopyResource hooked");
                else
                    LAD8VR::Log::Line("[STEREO] failed to hook CopyResource");
            }
            list->Release();
        }
    }
    return hr;
}

void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* queue, UINT count,
                                                 ID3D12CommandList* const* lists)
{
    // The probe goes in front of EVERY direct-queue submission, not once per frame: a slot
    // already consumed ignores the write harmlessly, and any candidate bound by THIS
    // submission's lists was resolved while they were recorded, so it is in the list by now.
    if (g_enabled && (g_probe || StampingActive())) MaybeProbe(queue);

    // Candidates enqueued during this submission's recording are patched before it reaches
    // the GPU. No-op until calibration has produced a delta. Direct queues only: the WBI
    // patch list must ride the queue whose draws consume the buffers, or its ordering
    // guarantee means nothing.
    if (g_enabled && g_calibrate && g_deltaCount > 0)
    {
        const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) ProcessPendingCandidates(queue);
    }

    // The bind hook now does the patching, at the exact buffer the draws reference. The ring
    // sweep stays only as a fallback for the case where the bind hook never installs.
    if (g_enabled && g_realSetGraphicsRootCbv == nullptr &&
        InterlockedCompareExchange(&g_patchedThisFrame, 1, 0) == 0)
    {
        __try
        {
            PatchThisFrame();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    g_realExecute(queue, count, lists);
}
}

// Everything that must be instrumented on the device itself, split out so it can run the
// instant the device exists rather than waiting for a swapchain.
struct HookSpec
{
    size_t slot;
    void* detour;
    void** original;
    const char* name;
};
// Give the device its own copy of its vtable and swap the detours into the copy, instead of
// letting MinHook rewrite the function prologues in place.
//
// Measured: CreateCommittedResource cannot be trampolined at all on this game. It crashed
// before the first present even in pure passthrough, with the detour installed and doing
// nothing but calling through, while five other slots on the SAME vtable hooked fine. That is
// a prologue MinHook cannot relocate, and no amount of changing what the detour does can fix
// it. Copying the table sidesteps the whole problem: not one byte of code is modified, the
// originals stay pristine and are called directly, and each device gets its own table so
// nothing is shared with anyone else who might be hooking.
// One copy array PER DEVICE. Sharing a single array meant each new device stomped the table
// the previous one was dispatching through; guarding to a single device instead fixed the
// crash but instrumented only the first device, which is a throwaway probe, so the hooks
// never fired on the device that actually renders. The originals stay global because every
// device here comes from the same D3D12Core and so shares the same real functions.
constexpr size_t kMaxDevices = 8;
void* g_vtableCopies[kMaxDevices][128] = {};
volatile LONG g_deviceCount = 0;
bool InstallVtableCopy(ID3D12Device* device, size_t slots, const HookSpec* specs, size_t count,
                       unsigned mask)
{
    void** original = *reinterpret_cast<void***>(device);
    if (slots > 128) slots = 128;
    const LONG deviceIndex = InterlockedIncrement(&g_deviceCount) - 1;
    if (deviceIndex >= static_cast<LONG>(kMaxDevices)) return false;
    void** copy = g_vtableCopies[deviceIndex];
    memcpy(copy, original, slots * sizeof(void*));

    unsigned index = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const unsigned bit = 1u << index++;
        if (specs[i].slot >= slots || (mask & bit) == 0) continue;
        // Never capture a detour as the original. D3D12CreateDevice can hand back a device
        // whose vtable is already one of these copies, and capturing from that makes the
        // detour call itself forever. Skip any slot that already holds a detour.
        if (original[specs[i].slot] == specs[i].detour) continue;
        if (*specs[i].original == nullptr) *specs[i].original = original[specs[i].slot];
        copy[specs[i].slot] = specs[i].detour;
        LAD8VR::Log::Line(std::string("[STEREO] bit ") + std::to_string(bit) + " " +
                          specs[i].name + " swapped");
    }

    DWORD previous = 0;
    void** slot = reinterpret_cast<void**>(device);
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) return false;
    *slot = copy;
    VirtualProtect(slot, sizeof(void*), previous, &previous);
    return true;
}
// Set by InstallDeviceHooks so Init() can swap the render device's vtable using the same
// table. The swap has to happen there, not at device creation, because only the swapchain's
// command queue identifies which of this game's four devices actually renders.
const HookSpec* g_specs = nullptr;
size_t g_specCount = 0;
size_t g_specSlots = 0;

void InstallDeviceHooks(ID3D12Device* device)
{
    // Exactly one device. g_vtableCopy is a single array and every hook stores its original
    // in a single global, so instrumenting a second device overwrites the table the first one
    // is still dispatching through. This game creates four.
    if (device == nullptr) return;
    // MinHook installs once (its trampolines are global); the vtable copy runs for every
    // device, each with its own array, so whichever device actually renders is covered.
    static bool minhookDone = false;
    const bool installMinHook = !minhookDone;
    minhookDone = true;
    if (device == nullptr) return;

    // Once per process, not once per vtable. Every hook here stores its trampoline in a single
    // global, so hooking a second device with a different vtable overwrites the first device's
    // original and calls on the first device then dispatch into the second device's
    // implementation. This game creates four devices; hooking more than one is what crashed it
    // the moment the slot numbers were right enough for these to actually be called.

    void** dv = *reinterpret_cast<void***>(device);

    // Check the vtable SLOT is readable before reading it. The previous version passed
    // dv[slots - 1] to IsBadReadPtr, which dereferences the slot to pass its value: the
    // out-of-bounds read happens before the check that was supposed to prevent it. That is
    // what killed the launch, twice, at exactly this point.
    // ID3D12Device10Vtbl has 74 entries (0..73, CreateReservedResource2 last).
    size_t slots = 42;
    while (slots < 74 && !IsBadReadPtr(&dv[slots], sizeof(void*)) &&
           !IsBadCodePtr(reinterpret_cast<FARPROC>(dv[slots])))
        ++slots;
    // ID3D12Device vtable slots.
    const HookSpec specs[] = {
        // Slots read out of d3d12.h's ID3D12Device10Vtbl, not counted by hand. Every number
        // here was previously wrong: the hand count drifted by two from CheckFeatureSupport
        // onward, so CreateCommittedResource was hooking CreatePlacedResource, and
        // CreateCommittedResource3 at 75 was past the end of the vtable entirely, which is
        // what crashed the launch.
        {25, &HookedCreateCommittedResource, reinterpret_cast<void**>(&g_realCreateCommitted),
         "CreateCommittedResource"},
        {27, &HookedCreatePlacedResource, reinterpret_cast<void**>(&g_realCreatePlaced),
         "CreatePlacedResource"},
        {14, &HookedCreateDescriptorHeap, reinterpret_cast<void**>(&g_realCreateDescriptorHeap),
         "CreateDescriptorHeap"},
        {23, &HookedCopyDescriptors, reinterpret_cast<void**>(&g_realCopyDescriptors),
         "CopyDescriptors"},
        {24, &HookedCopyDescriptorsSimple,
         reinterpret_cast<void**>(&g_realCopyDescriptorsSimple), "CopyDescriptorsSimple"},
        {17, &HookedCreateCbv, reinterpret_cast<void**>(&g_realCreateCbv),
         "CreateConstantBufferView"},
        {12, &HookedCreateCommandList, reinterpret_cast<void**>(&g_realCreateCommandList),
         "CreateCommandList"},
        {51, &HookedCommitted1, reinterpret_cast<void**>(&g_realCommitted1),
         "CreateCommittedResource1"},
        {64, &HookedCommitted2, reinterpret_cast<void**>(&g_realCommitted2),
         "CreateCommittedResource2"},
        {71, &HookedCommitted3, reinterpret_cast<void**>(&g_realCommitted3),
         "CreateCommittedResource3"},
        {65, &HookedPlaced1, reinterpret_cast<void**>(&g_realPlaced1), "CreatePlacedResource1"},
        {72, &HookedPlaced2, reinterpret_cast<void**>(&g_realPlaced2), "CreatePlacedResource2"},
    };
    // Resource creation goes through the vtable copy; the descriptor chain stays on MinHook
    // because it has been running for hours without trouble and there is no reason to disturb
    // it. Slots 12/14/17/23/24 are the descriptor chain, everything else is resource creation.
    g_specs = specs;
    g_specCount = sizeof(specs) / sizeof(specs[0]);
    g_specSlots = slots;

    unsigned specIndex = 0;
    for (const HookSpec& spec : specs)
    {
        const unsigned bit = 1u << specIndex++;
        if (!installMinHook) continue;
        if (spec.slot >= slots || (g_hookMask & bit) == 0) continue;
        const bool descriptorChain = spec.slot == 12 || spec.slot == 14 || spec.slot == 17 ||
                                     spec.slot == 23 || spec.slot == 24;
        if (!descriptorChain) continue;
        if (MH_CreateHook(dv[spec.slot], spec.detour, spec.original) == MH_OK &&
            MH_EnableHook(dv[spec.slot]) == MH_OK)
            LAD8VR::Log::Line(std::string("[STEREO] bit ") + std::to_string(bit) + " " +
                              spec.name + " hooked");
        else
            LAD8VR::Log::Line(std::string("[STEREO] failed to hook ") + spec.name);
    }

    // The vtable copy is NOT done here. This game creates four devices and swapping all four
    // crashes, while swapping one is stable but picks a throwaway adapter probe that never
    // renders. The device that owns the swapchain's command queue is the one that matters and
    // it is knowable, so the swap is deferred to Init() where that queue is in hand.
}

using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
D3D12CreateDeviceFn g_realCreateDevice = nullptr;

HRESULT WINAPI HookedD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid,
                                       void** out)
{
    const HRESULT hr = g_realCreateDevice(adapter, level, riid, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && g_enabled)
    {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&device))) &&
            device != nullptr)
        {
            LAD8VR::Log::Line("[STEREO] device created, instrumenting before first allocation");
            InstallDeviceHooks(device);
            device->Release();
        }
    }
    return hr;
}

namespace LAD8VR::Stereo
{
void InstallDeviceCreationHook() noexcept
{
    wchar_t iniPath[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallDeviceCreationHook), &self);
    GetModuleFileNameW(self, iniPath, MAX_PATH);
    if (wchar_t* slash = wcsrchr(iniPath, L'\\')) wcscpy_s(slash + 1, 32, L"lad8vr.ini");
    if (GetPrivateProfileIntW(L"Stereo", L"Enable", 0, iniPath) == 0) return;
    g_offset = static_cast<float>(GetPrivateProfileIntW(L"Stereo", L"ProofOffset", 3000, iniPath)) /
               1000.0f;
    g_passthrough = GetPrivateProfileIntW(L"Stereo", L"Passthrough", 0, iniPath) != 0;
    g_hookMask = static_cast<unsigned>(
        GetPrivateProfileIntW(L"Stereo", L"HookMask", 0x7F, iniPath));
    g_hookCopies = GetPrivateProfileIntW(L"Stereo", L"HookCopies", 1, iniPath) != 0;

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (d3d12 == nullptr) return;
    void* target = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (target == nullptr) return;

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return;
    if (MH_CreateHook(target, &HookedD3D12CreateDevice,
                      reinterpret_cast<void**>(&g_realCreateDevice)) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
    {
        g_enabled = true;
        Log::Line("[STEREO] D3D12CreateDevice hooked before device init");
    }
    else
    {
        Log::Line("[STEREO] failed to hook D3D12CreateDevice");
    }
}

void Init(IUnknown* commandQueue) noexcept
{
    if (g_realExecute != nullptr || commandQueue == nullptr) return;

    wchar_t iniPath[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Init), &self);
    GetModuleFileNameW(self, iniPath, MAX_PATH);
    if (wchar_t* slash = wcsrchr(iniPath, L'\\')) wcscpy_s(slash + 1, 32, L"lad8vr.ini");
    if (GetPrivateProfileIntW(L"Stereo", L"Enable", 0, iniPath) == 0) return;

    // Thousandths of a world unit of sway amplitude. Big for the proof so the motion is
    // unmistakable; small and alternating for real per-eye stereo.
    g_offset = static_cast<float>(GetPrivateProfileIntW(L"Stereo", L"ProofOffset", 3000, iniPath)) /
               1000.0f;
    g_offsetAmplitude = g_offset;
    g_probe = GetPrivateProfileIntW(L"Stereo", L"Probe", 0, iniPath) != 0;
    if (g_probe) Log::Line("[PROBE] armed: GPU-side write test, CPU-side patching suspended");
    g_calibrate = GetPrivateProfileIntW(L"Stereo", L"Calibrate", 0, iniPath) != 0;
    if (g_calibrate)
        Log::Line("[CAL] armed: stamping candidate VAs until the ring sweep hands back a delta");

    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(commandQueue->QueryInterface(IID_PPV_ARGS(&queue))) || queue == nullptr)
    {
        Log::Line("[STEREO] present device is not a command queue, cannot hook");
        return;
    }

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log::Line("[STEREO] MH_Initialize failed");
        queue->Release();
        return;
    }

    // ExecuteCommandLists is vtable slot 10 on ID3D12CommandQueue: IUnknown 0-2,
    // ID3D12Object 3-6, ID3D12DeviceChild 7 (GetDevice), then UpdateTileMappings 8,
    // CopyTileMappings 9, ExecuteCommandLists 10.
    void** vtable = *reinterpret_cast<void***>(queue);
    void* target = vtable[10];
    queue->Release();

    if (MH_CreateHook(target, &HookedExecuteCommandLists,
                      reinterpret_cast<void**>(&g_realExecute)) != MH_OK ||
        MH_EnableHook(target) != MH_OK)
    {
        Log::Line("[STEREO] failed to hook ExecuteCommandLists");
        return;
    }

    // Reach the device through the queue and hook resource + command list creation, so the
    // camera can be identified by the address the draws actually bind instead of by guesswork.
    // Normally already done from the D3D12CreateDevice hook; this covers the case where the
    // device was made before this DLL loaded.
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
    {
        InstallDeviceHooks(device);
        device->Release();
    }

    // Swap the vtable of the device that owns THIS queue: the one that actually renders.
    ID3D12Device* renderDevice = nullptr;
    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&renderDevice))) && renderDevice != nullptr)
    {
        InstallDeviceHooks(renderDevice);
        if (g_specs != nullptr)
        {
            unsigned resourceMask = 0;
            for (size_t i = 0; i < g_specCount; ++i)
            {
                const size_t s = g_specs[i].slot;
                const bool descriptorChain = s == 12 || s == 14 || s == 17 || s == 23 || s == 24;
                if (!descriptorChain) resourceMask |= 1u << i;
            }
            if (!InstallVtableCopy(renderDevice, g_specSlots, g_specs, g_specCount,
                                   g_hookMask & resourceMask))
                Log::Line("[STEREO] vtable copy install failed");
        }
        renderDevice->Release();
    }

    g_enabled = true;
    char line[160] = {};
    sprintf_s(line, "[STEREO] PROOF ON: shoving view X by %.3f at the bind the draws use",
              g_offset);
    Log::Line(line);
}

void OnPresent() noexcept
{
    InterlockedExchange(&g_patchedThisFrame, 0);
    if (g_probe || g_calibrate)
        g_probeCandsLastFrame = InterlockedExchange(&g_probeCandidateCount, 0);
    if (g_calibrate)
    {
        InterlockedIncrement(&g_frameId);
        g_pendingLastFrame = InterlockedExchange(&g_pendingPatchCount, 0);
        InterlockedExchange(&g_patchProcessed, 0);
        g_blindLastFrame = InterlockedExchange(&g_blindCount, 0);
        // ~1.6 s period: slow enough to read as motion, fast enough to be obvious.
        g_offset = g_offsetAmplitude * sinf(static_cast<float>(GetTickCount64() % 100000) * 0.004f);
        TryCalibrate();
        RebuildHotMystery();
    }
    DrainPendingUploads();

    const ULONGLONG now = GetTickCount64();
    if (g_enabled && now - g_lastLog > 3000)
    {
        g_lastLog = now;
        char line[200] = {};
        sprintf_s(line,
                  "[STEREO] tables=%ld handleOk=%ld cbvs=%ld resolved=%ld translated=%ld "
                  "widestRt=%ld binds=%ld patches=%llu heaps=%ld late=%ld heapDrop=%ld "
                  "cbvSeen=%ld",
                  g_tableSeen, g_handleOk, g_cbvRecorded, g_cbvResolved, g_translated,
                  g_widestRt, g_boundHits, g_patches, g_heapCount, g_lateHeaps, g_heapDropped,
                  g_cbvSeen);
        Log::Line(line);

        if (g_probe || g_calibrate)
        {
            // submits with cands>0 and no visual change = writes execute but miss, a different
            // failure from submits=0 (gating/init) or skips dominating (fence starvation).
            char probeLine[200] = {};
            sprintf_s(probeLine,
                      "[PROBE] submits=%ld skips=%ld cands=%ld dropped=%ld fenceNext=%llu "
                      "fenceDone=%llu",
                      g_probeSubmits, g_probeSkips, g_probeCandsLastFrame, g_probeCandDropped,
                      g_probeFenceNext,
                      g_probeFence != nullptr ? g_probeFence->GetCompletedValue() : 0ull);
            Log::Line(probeLine);
        }
        if (g_calibrate)
        {
            char calLine[200] = {};
            sprintf_s(calLine, "[CAL] deltas=%ld pendCands=%ld pendDrop=%ld binds=%ld patches=%llu",
                      g_deltaCount, g_pendingLastFrame, g_pendingPatchDropped, g_boundHits,
                      g_patches);
            Log::Line(calLine);
            char sizeLine[160] = {};
            sprintf_s(sizeLine, "[SIZE] cbv sizes: 512=%ld small=%ld mid(<=4K)=%ld big=%ld",
                      g_size512, g_sizeSmall, g_sizeMid, g_sizeBig);
            Log::Line(sizeLine);
            // The verdict line: is the 3840-wide camera being patched at all?
            char patchLine[200] = {};
            sprintf_s(patchLine,
                      "[PATCH] w3840=%ld w2560=%ld w1920=%ld other=%ld wbiLists=%ld wbiSkips=%ld",
                      g_patchW3840, g_patchW2560, g_patchW1920, g_patchWOther, g_patchWbiWrites,
                      g_patchWbiSkips);
            Log::Line(patchLine);
            // Persistent CBV VAs outside the ring = default-heap camera destinations; matches
            // and srcPatched say whether the copy hooks found and patched their feed.
            LONG topHits = 0;
            LONG topFrames = 0;
            D3D12_GPU_VIRTUAL_ADDRESS topVa = 0;
            for (size_t i = 0; i < kMysterySize; ++i)
                if (g_mystery[i].va != 0 && g_mystery[i].hits > topHits)
                {
                    topHits = g_mystery[i].hits;
                    topFrames = g_mystery[i].frames;
                    topVa = g_mystery[i].va;
                }
            char mystLine[240] = {};
            sprintf_s(mystLine,
                      "[MYST] hot=%ld blind=%ld persist=%ld blindDrop=%ld mystWrites=%ld "
                      "top=0x%llx hits=%ld frames=%ld",
                      g_hotCount, g_blindLastFrame, g_persistentBlindCount, g_blindDropped,
                      g_mystWrites, static_cast<unsigned long long>(topVa), topHits, topFrames);
            Log::Line(mystLine);
        }
    }
}

void Shutdown() noexcept
{
    if (g_realExecute != nullptr) MH_DisableHook(MH_ALL_HOOKS);
}
}





















