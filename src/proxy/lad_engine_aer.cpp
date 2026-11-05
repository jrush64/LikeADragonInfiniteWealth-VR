#include "lad_engine_aer.h"

#include "logger.h"

#include <windows.h>
#include <tlhelp32.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace
{
constexpr uintptr_t kBuilderPreRva = 0x027F6303;
constexpr uintptr_t kBuilderPostRva = 0x027F6305;
constexpr uintptr_t kContextStateOffset = 0x12CC0;
constexpr uintptr_t kCameraBlockOffset = 0x480;
constexpr size_t kOffP = 0;
constexpr size_t kOffV = 64;
constexpr size_t kOffVP = 128;
constexpr size_t kOffT3 = 384;
constexpr size_t kOffRt = 464;
constexpr size_t kOffLocation = 480;
constexpr size_t kMaxThreads = 256;
constexpr LONG kEyeTagCapacity = 64;

struct Mat4 { float m[16]; };
struct ThreadPatch
{
    volatile LONG tid = 0;
    volatile LONG active = 0;
    volatile LONG slot = -1;
    volatile LONG64 source = 0;
    Mat4 v = {};
    Mat4 vp = {};
    Mat4 t3 = {};
    float location[4] = {};
};

uintptr_t g_imageBase = 0;
PVOID g_veh = nullptr;
volatile LONG g_started = 0;
volatile LONG g_active = 0;
volatile LONG g_buildEye = 0;
volatile LONG g_nextBuildEye = 0;
volatile LONG g_frameOpen = 0;
volatile LONG g_tagRead = 0;
volatile LONG g_tagWrite = 0;
volatile LONG64 g_framesStarted = 0;
volatile LONG64 g_framesCompleted = 0;
volatile LONG64 g_frameOverlaps = 0;
std::array<unsigned char, kEyeTagCapacity> g_eyeTags = {};
volatile LONG g_shutdown = 0;
float g_eyeOffset = 0.032f;
bool g_swapEyes = false;
ULONGLONG g_lastArm = 0;
ULONGLONG g_lastReport = 0;
volatile LONG64 g_preHits = 0;
volatile LONG64 g_patched = 0;
volatile LONG64 g_restored = 0;
std::array<ThreadPatch, kMaxThreads> g_patches = {};
std::unordered_set<DWORD> g_seenThreads;

void PushEyeTag(uint32_t eye)
{
    LONG write = InterlockedCompareExchange(&g_tagWrite, 0, 0);
    LONG read = InterlockedCompareExchange(&g_tagRead, 0, 0);
    if (write - read >= kEyeTagCapacity)
        InterlockedIncrement(&g_tagRead);
    g_eyeTags[static_cast<size_t>(write % kEyeTagCapacity)] =
        static_cast<unsigned char>(eye & 1u);
    MemoryBarrier();
    InterlockedExchange(&g_tagWrite, write + 1);
}

bool ReadBytes(const void* address, void* output, size_t size)
{
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, output, size, &done) != FALSE &&
           done == size;
}

bool WriteBytes(void* address, const void* input, size_t size)
{
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), address, input, size, &done) != FALSE &&
           done == size;
}

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 result = {};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k)
                result.m[row * 4 + col] += a.m[row * 4 + k] * b.m[k * 4 + col];
    return result;
}

bool ValidateCamera(const unsigned char* source, Mat4& p, Mat4& v, Mat4& vp)
{
    float rt[2] = {};
    if (!ReadBytes(source + kOffP, &p, sizeof(p)) ||
        !ReadBytes(source + kOffV, &v, sizeof(v)) ||
        !ReadBytes(source + kOffVP, &vp, sizeof(vp)) ||
        !ReadBytes(source + kOffRt, rt, sizeof(rt)))
        return false;
    for (float value : p.m) if (!std::isfinite(value)) return false;
    for (float value : v.m) if (!std::isfinite(value)) return false;
    if (rt[0] < 16.0f || rt[0] > 16384.0f || rt[1] < 16.0f || rt[1] > 16384.0f)
        return false;
    if (p.m[0] == 0.0f || p.m[5] == 0.0f) return false;
    const float aspect = std::fabs(p.m[5] / p.m[0]);
    if (aspect < 1.2f || aspect > 2.4f) return false;
    for (int row = 0; row < 3; ++row)
    {
        const float length = v.m[row * 4] * v.m[row * 4] +
                             v.m[row * 4 + 1] * v.m[row * 4 + 1] +
                             v.m[row * 4 + 2] * v.m[row * 4 + 2];
        if (length < 0.98f || length > 1.02f) return false;
    }
    const float dot01 = v.m[0] * v.m[4] + v.m[1] * v.m[5] + v.m[2] * v.m[6];
    return std::fabs(dot01) < 0.02f;
}

ThreadPatch* FindPatch(DWORD tid)
{
    for (ThreadPatch& entry : g_patches)
    {
        const LONG existing = InterlockedCompareExchange(&entry.tid, 0, 0);
        if (existing == static_cast<LONG>(tid)) return &entry;
        if (existing == 0 &&
            InterlockedCompareExchange(&entry.tid, static_cast<LONG>(tid), 0) == 0)
            return &entry;
    }
    return nullptr;
}

unsigned TriggeredSlot(const CONTEXT* cpu, uintptr_t expected)
{
    const uintptr_t addresses[4] = {static_cast<uintptr_t>(cpu->Dr0),
                                    static_cast<uintptr_t>(cpu->Dr1),
                                    static_cast<uintptr_t>(cpu->Dr2),
                                    static_cast<uintptr_t>(cpu->Dr3)};
    for (unsigned slot = 0; slot < 4; ++slot)
        if ((cpu->Dr6 & (1ull << slot)) != 0 && addresses[slot] == expected) return slot;
    return 4;
}

void SetDebugAddress(CONTEXT* cpu, unsigned slot, uintptr_t address)
{
    switch (slot)
    {
    case 0: cpu->Dr0 = address; break;
    case 1: cpu->Dr1 = address; break;
    case 2: cpu->Dr2 = address; break;
    case 3: cpu->Dr3 = address; break;
    }
    cpu->Dr7 &= ~(0xFull << (16 + slot * 4));
    cpu->Dr7 |= (1ull << (slot * 2));
}

bool Restore(ThreadPatch* entry)
{
    if (entry == nullptr || InterlockedCompareExchange(&entry->active, 0, 0) == 0)
        return false;
    auto* source = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(
        InterlockedCompareExchange64(&entry->source, 0, 0)));
    const bool ok = source != nullptr &&
        WriteBytes(source + kOffV, &entry->v, sizeof(entry->v)) &&
        WriteBytes(source + kOffVP, &entry->vp, sizeof(entry->vp)) &&
        WriteBytes(source + kOffT3, &entry->t3, sizeof(entry->t3)) &&
        WriteBytes(source + kOffLocation, entry->location, sizeof(entry->location));
    InterlockedExchange(&entry->active, 0);
    if (ok) InterlockedIncrement64(&g_restored);
    return ok;
}

bool Apply(CONTEXT* cpu, unsigned slot, ThreadPatch* entry, uint8_t bucket)
{
    if (bucket < 2 || bucket > 31) return false;
    uintptr_t state = 0;
    const uintptr_t context = static_cast<uintptr_t>(cpu->Rdx);
    if (context < 0x10000 ||
        !ReadBytes(reinterpret_cast<const void*>(context + kContextStateOffset), &state,
                   sizeof(state)) || state < 0x10000)
        return false;
    auto* source = reinterpret_cast<unsigned char*>(state + kCameraBlockOffset);
    Mat4 p = {}, v = {}, vp = {};
    if (!ValidateCamera(source, p, v, vp)) return false;
    Mat4 t3 = {};
    float location[4] = {};
    if (!ReadBytes(source + kOffT3, &t3, sizeof(t3)) ||
        !ReadBytes(source + kOffLocation, location, sizeof(location)))
        return false;

    entry->v = v;
    entry->vp = vp;
    entry->t3 = t3;
    for (int i = 0; i < 4; ++i) entry->location[i] = location[i];
    InterlockedExchange64(&entry->source,
                          static_cast<LONG64>(reinterpret_cast<uintptr_t>(source)));
    InterlockedExchange(&entry->slot, static_cast<LONG>(slot));

    LONG eye = InterlockedCompareExchange(&g_buildEye, 0, 0);
    if (g_swapEyes) eye = 1 - eye;
    const float offset = eye == 0 ? -g_eyeOffset : g_eyeOffset;
    v.m[3] += offset;
    const Mat4 alteredVP = Multiply(p, v);
    t3.m[3] += p.m[0] * offset;
    location[0] -= offset * v.m[0];
    location[1] -= offset * v.m[1];
    location[2] -= offset * v.m[2];
    const bool ok = WriteBytes(source + kOffV, &v, sizeof(v)) &&
                    WriteBytes(source + kOffVP, &alteredVP, sizeof(alteredVP)) &&
                    WriteBytes(source + kOffT3, &t3, sizeof(t3)) &&
                    WriteBytes(source + kOffLocation, location, sizeof(location));
    if (!ok)
    {
        WriteBytes(source + kOffV, &entry->v, sizeof(entry->v));
        WriteBytes(source + kOffVP, &entry->vp, sizeof(entry->vp));
        WriteBytes(source + kOffT3, &entry->t3, sizeof(entry->t3));
        WriteBytes(source + kOffLocation, entry->location, sizeof(entry->location));
        return false;
    }
    InterlockedExchange(&entry->active, 1);
    InterlockedIncrement64(&g_patched);
    SetDebugAddress(cpu, slot, g_imageBase + kBuilderPostRva);
    return true;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* exception)
{
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* cpu = exception->ContextRecord;
    const uintptr_t rip = static_cast<uintptr_t>(cpu->Rip);
    const uintptr_t pre = g_imageBase + kBuilderPreRva;
    const uintptr_t post = g_imageBase + kBuilderPostRva;
    const unsigned slot = TriggeredSlot(cpu, rip);
    if (slot >= 4 || (rip != pre && rip != post)) return EXCEPTION_CONTINUE_SEARCH;

    ThreadPatch* entry = FindPatch(GetCurrentThreadId());
    if (rip == pre)
    {
        InterlockedIncrement64(&g_preHits);
        uint8_t bucket = 0;
        SIZE_T read = 0;
        ReadProcessMemory(GetCurrentProcess(),
                          reinterpret_cast<const void*>(static_cast<uintptr_t>(cpu->Rcx) + 0x24),
                          &bucket, sizeof(bucket), &read);
        if (entry != nullptr && read == sizeof(bucket) &&
            InterlockedCompareExchange(&g_active, 0, 0) != 0)
        {
            // Own the eye at the engine's scene-build boundary, not at the later Present.
            if (bucket == 2)
            {
                if (InterlockedExchange(&g_frameOpen, 1) != 0)
                    InterlockedIncrement64(&g_frameOverlaps);
                const LONG eye = InterlockedXor(&g_nextBuildEye, 1) & 1;
                InterlockedExchange(&g_buildEye, eye);
                InterlockedIncrement64(&g_framesStarted);
            }
            if (InterlockedCompareExchange(&g_frameOpen, 0, 0) != 0)
                Apply(cpu, slot, entry, bucket);
            // Bucket 31 closes the observed 2..31 pass sequence. Carry its eye to Present.
            if (bucket == 31 && InterlockedExchange(&g_frameOpen, 0) != 0)
            {
                PushEyeTag(static_cast<uint32_t>(
                    InterlockedCompareExchange(&g_buildEye, 0, 0)));
                InterlockedIncrement64(&g_framesCompleted);
            }
        }
    }
    else
    {
        Restore(entry);
        SetDebugAddress(cpu, slot, pre);
    }
    cpu->Dr6 = 0;
    cpu->EFlags |= 0x10000;
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool SetThreadBreakpoint(DWORD tid)
{
    if (tid == GetCurrentThreadId()) return false;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, tid);
    if (thread == nullptr) return false;
    if (SuspendThread(thread) == static_cast<DWORD>(-1))
    {
        CloseHandle(thread);
        return false;
    }
    CONTEXT cpu = {};
    cpu.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = false;
    if (GetThreadContext(thread, &cpu))
    {
        DWORD64* addresses[4] = {&cpu.Dr0, &cpu.Dr1, &cpu.Dr2, &cpu.Dr3};
        for (unsigned slot = 0; slot < 4; ++slot)
        {
            if (((cpu.Dr7 >> (slot * 2)) & 0x3ull) != 0) continue;
            *addresses[slot] = g_imageBase + kBuilderPreRva;
            cpu.Dr6 = 0;
            cpu.Dr7 |= (1ull << (slot * 2));
            cpu.Dr7 &= ~(0xFull << (16 + slot * 4));
            ok = SetThreadContext(thread, &cpu) != FALSE;
            break;
        }
    }
    ResumeThread(thread);
    CloseHandle(thread);
    return ok;
}

void LoadConfig()
{
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadConfig), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) wcscpy_s(slash + 1, 32, L"lad8vr.ini");
    int millimetres = GetPrivateProfileIntW(L"AER", L"EyeOffsetMillimetres", 32, path);
    if (millimetres < 0) millimetres = 0;
    if (millimetres > 100) millimetres = 100;
    g_eyeOffset = static_cast<float>(millimetres) / 1000.0f;
    g_swapEyes = GetPrivateProfileIntW(L"AER", L"SwapEyes", 0, path) != 0;
}
}

namespace LAD8VR::EngineAer
{
void Start() noexcept
{
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    g_imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    LoadConfig();
    g_veh = AddVectoredExceptionHandler(1, Handler);
    char line[192] = {};
    sprintf_s(line, "[AER] engine camera ready eyeOffset=%.4f swapEyes=%d veh=%s",
              g_eyeOffset, g_swapEyes ? 1 : 0, g_veh != nullptr ? "yes" : "NO");
    Log::Line(line);
}

void Tick() noexcept
{
    if (g_veh == nullptr || InterlockedCompareExchange(&g_shutdown, 0, 0) != 0) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastArm >= 500)
    {
        g_lastArm = now;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 entry = {};
            entry.dwSize = sizeof(entry);
            if (Thread32First(snapshot, &entry))
            {
                do
                {
                    if (entry.th32OwnerProcessID != GetCurrentProcessId() ||
                        entry.th32ThreadID == GetCurrentThreadId() ||
                        g_seenThreads.find(entry.th32ThreadID) != g_seenThreads.end())
                        continue;
                    g_seenThreads.insert(entry.th32ThreadID);
                    SetThreadBreakpoint(entry.th32ThreadID);
                } while (Thread32Next(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
    }
    if (now - g_lastReport >= 3000)
    {
        g_lastReport = now;
        char line[320] = {};
        sprintf_s(line, "[AER] active=%ld buildEye=%ld threads=%zu pre=%lld patched=%lld restored=%lld frames=%lld/%lld overlap=%lld queued=%ld",
                  InterlockedCompareExchange(&g_active, 0, 0),
                  InterlockedCompareExchange(&g_buildEye, 0, 0), g_seenThreads.size(),
                  InterlockedCompareExchange64(&g_preHits, 0, 0),
                  InterlockedCompareExchange64(&g_patched, 0, 0),
                  InterlockedCompareExchange64(&g_restored, 0, 0),
                  InterlockedCompareExchange64(&g_framesCompleted, 0, 0),
                  InterlockedCompareExchange64(&g_framesStarted, 0, 0),
                  InterlockedCompareExchange64(&g_frameOverlaps, 0, 0),
                  InterlockedCompareExchange(&g_tagWrite, 0, 0) -
                      InterlockedCompareExchange(&g_tagRead, 0, 0));
        Log::Line(line);
    }
}

void SetActive(bool active) noexcept
{
    const LONG value = active ? 1 : 0;
    const LONG old = InterlockedExchange(&g_active, value);
    if (active && old == 0)
    {
        InterlockedExchange(&g_buildEye, 0);
        InterlockedExchange(&g_nextBuildEye, 0);
        InterlockedExchange(&g_frameOpen, 0);
        InterlockedExchange(&g_tagRead, 0);
        InterlockedExchange(&g_tagWrite, 0);
    }
    if (old != value) Log::Line(active ? "[AER] camera shifting active" : "[AER] camera shifting paused");
}

bool Active() noexcept { return InterlockedCompareExchange(&g_active, 0, 0) != 0; }
bool TryConsumeCompletedEye(uint32_t& eye) noexcept
{
    const LONG read = InterlockedCompareExchange(&g_tagRead, 0, 0);
    const LONG write = InterlockedCompareExchange(&g_tagWrite, 0, 0);
    if (read >= write) return false;
    MemoryBarrier();
    eye = static_cast<uint32_t>(
        g_eyeTags[static_cast<size_t>(read % kEyeTagCapacity)] & 1u);
    InterlockedExchange(&g_tagRead, read + 1);
    return true;
}

void Shutdown() noexcept
{
    InterlockedExchange(&g_shutdown, 1);
    InterlockedExchange(&g_active, 0);
    for (ThreadPatch& entry : g_patches) Restore(&entry);
    // The process is terminating when this is called. Do not suspend every game thread from
    // DllMain; the process cannot execute another frame after the proxy is detached.
    if (g_veh != nullptr)
    {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
    }
}
}
