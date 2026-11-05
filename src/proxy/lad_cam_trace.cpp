#include "lad_cam_trace.h"

#include "logger.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr LONG kMaxHits = 64;
constexpr int kCallerCapacity = 8;
constexpr uintptr_t kCameraBuilderCallRva = 0x64D9F;
constexpr uintptr_t kCameraUploadPreCopyRva = 0x30084B;
constexpr uintptr_t kCameraUploadPostCopyRva = 0x300857;
constexpr size_t kBuiltCbStride = 0x1F0;
constexpr size_t kBuilderOutputAdvance = 0x380;
constexpr size_t kOffP = 0;
constexpr size_t kOffV = 64;
constexpr size_t kOffVP = 128;
constexpr size_t kOffRtSize = 464;
constexpr size_t kCopySnapshotSize = 0x200;

struct TraceHit
{
    DWORD threadId = 0;
    uint64_t rip = 0;
    uint64_t rsp = 0;
    uint64_t regs[15] = {};
    uint64_t callers[kCallerCapacity] = {};
};

TraceHit g_hits[kMaxHits];
volatile LONG g_hitCount = 0;
volatile LONG g_traceStarted = 0;
volatile LONG g_armed = 0;
volatile LONG g_stop = 0;
volatile LONG g_enabledState = -1;
volatile LONG64 g_watchAddress = 0;
volatile LONG64 g_upstreamCandidate = 0;
volatile LONG g_builderHits = 0;
volatile LONG g_builderMatches = 0;
volatile LONG g_builderBlocksPatched = 0;
volatile LONG64 g_builderFirstBase = 0;
volatile LONG64 g_builderFirstCaller = 0;
volatile LONG g_ownerStage = 0;
volatile LONG g_ownerThread = 0;
volatile LONG64 g_ownerAddress = 0;
volatile LONG64 g_ownerOutput = 0;
volatile LONG64 g_ownerSource = 0;
volatile LONG g_ownerSnapshotReady = 0;
unsigned char g_ownerObjectSnapshot[0x41000] = {};
unsigned char g_ownerCbpairSnapshot[0x3E0] = {};
volatile LONG g_copySnapshotReady = 0;
volatile LONG64 g_copySourceBase = 0;
volatile LONG64 g_copyDestinationBase = 0;
unsigned char g_copySourceSnapshot[kCopySnapshotSize] = {};
unsigned char g_copyDestinationSnapshot[kCopySnapshotSize] = {};
uintptr_t g_exeBase = 0;
uintptr_t g_exeEnd = 0;
PVOID g_veh = nullptr;

bool ReadU64Guarded(const uint64_t* address, uint64_t* out) noexcept
{
    __try
    {
        *out = *address;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadBytesGuarded(const unsigned char* address, unsigned char* out, size_t count) noexcept
{
    __try
    {
        memcpy(out, address, count);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool InGameImage(uint64_t address) noexcept
{
    return address >= g_exeBase + 0x1000 && address < g_exeEnd;
}

bool ProductClose(const float* a, const float* b, const float* expected) noexcept
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[row * 4 + k] * b[k * 4 + col];
            const float want = expected[row * 4 + col];
            const float scale = fabsf(sum) + fabsf(want) + 1.0f;
            if (fabsf(sum - want) / scale > 0.002f) return false;
        }
    }
    return true;
}

int FullCb12Convention(const unsigned char* address) noexcept
{
    __try
    {
        const float* p = reinterpret_cast<const float*>(address + kOffP);
        const float* v = reinterpret_cast<const float*>(address + kOffV);
        const float* vp = reinterpret_cast<const float*>(address + kOffVP);
        const float* rt = reinterpret_cast<const float*>(address + kOffRtSize);
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(p[i]) || !std::isfinite(v[i]) || !std::isfinite(vp[i])) return 0;
        if (!std::isfinite(rt[0]) || !std::isfinite(rt[1]) ||
            rt[0] < 640.0f || rt[1] < 360.0f || rt[0] > 16384.0f || rt[1] > 16384.0f)
            return 0;
        if (p[0] == 0.0f || p[5] == 0.0f) return 0;
        const float aspect = fabsf(p[5] / p[0]);
        if (aspect < 1.2f || aspect > 2.4f) return 0;
        for (int row = 0; row < 3; ++row)
        {
            const float length = v[row * 4] * v[row * 4] +
                                 v[row * 4 + 1] * v[row * 4 + 1] +
                                 v[row * 4 + 2] * v[row * 4 + 2];
            if (length < 0.98f || length > 1.02f) return 0;
        }
        const float dot = v[0] * v[4] + v[1] * v[5] + v[2] * v[6];
        if (fabsf(dot) > 0.02f) return 0;
        if (ProductClose(v, p, vp)) return 1;
        if (ProductClose(p, v, vp)) return 2;
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

int PatchValidatedCameraUpload(unsigned char* address, float clipShift) noexcept
{
    __try
    {
        const int convention = FullCb12Convention(address);
        if (convention == 0) return 0;
        float* vp = reinterpret_cast<float*>(address + kOffVP);
        if (convention == 2)
        {
            for (int col = 0; col < 4; ++col) vp[col] += clipShift * vp[12 + col];
        }
        else
        {
            for (int row = 0; row < 4; ++row) vp[row * 4] += clipShift * vp[row * 4 + 3];
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

bool CaptureOwnerSnapshots(uintptr_t owner, uintptr_t source) noexcept
{
    __try
    {
        // Both copies happen inside the matching upload breakpoint, before the stack source
        // can be reused and in the same instant as the owner that produced it.
        memcpy(g_ownerCbpairSnapshot, reinterpret_cast<const void*>(source),
               sizeof(g_ownerCbpairSnapshot));
        memcpy(g_ownerObjectSnapshot, reinterpret_cast<const void*>(owner),
               sizeof(g_ownerObjectSnapshot));
        MemoryBarrier();
        InterlockedExchange(&g_ownerSnapshotReady, 1);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

LONG CALLBACK TraceVeh(EXCEPTION_POINTERS* ep)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr || ep->ContextRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || g_armed == 0)
        return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* ctx = ep->ContextRecord;
    if ((ctx->Dr6 & 1ull) == 0) return EXCEPTION_CONTINUE_SEARCH;

    if (g_enabledState == 3)
    {
        const DWORD threadId = GetCurrentThreadId();
        if (ctx->Rip == g_exeBase + kCameraBuilderCallRva && g_ownerStage == 0)
        {
            if (InterlockedCompareExchange(&g_ownerStage, 1, 0) == 0)
            {
                InterlockedExchange(&g_ownerThread, static_cast<LONG>(threadId));
                InterlockedExchange64(&g_ownerAddress, static_cast<LONG64>(ctx->Rcx));
                InterlockedExchange64(&g_ownerOutput, static_cast<LONG64>(ctx->Rdx));

                // Follow only this invocation on this thread to the generic uploader.
                ctx->Dr0 = g_exeBase + kCameraUploadPreCopyRva;
                ctx->Dr7 &= ~(0xFull << 16);
                ctx->Dr7 |= 1ull;
            }
            else
            {
                ctx->Dr7 &= ~1ull;
            }
            ctx->Dr6 = 0;
            ctx->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (ctx->Rip == g_exeBase + kCameraUploadPreCopyRva &&
            threadId == static_cast<DWORD>(g_ownerThread) && g_ownerStage == 1)
        {
            const uintptr_t expected = static_cast<uintptr_t>(g_ownerOutput);
            if (ctx->Rdi == 0x3E && ctx->Rsi == expected)
            {
                InterlockedExchange64(&g_ownerSource, static_cast<LONG64>(ctx->Rsi));
                CaptureOwnerSnapshots(
                    static_cast<uintptr_t>(g_ownerAddress),
                    static_cast<uintptr_t>(ctx->Rsi));
                InterlockedExchange(&g_ownerStage, 2);
                ctx->Dr7 &= ~1ull;
            }
            ctx->Dr6 = 0;
            ctx->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Another thread reached the original call after the first mapping was claimed.
        ctx->Dr7 &= ~1ull;
        ctx->Dr6 = 0;
        ctx->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (g_enabledState == 2)
    {
        if (ctx->Rip == g_exeBase + kCameraUploadPreCopyRva)
        {
            InterlockedIncrement(&g_builderHits);
            uint64_t returnAddress = 0;
            const bool hasReturn = ReadU64Guarded(
                reinterpret_cast<const uint64_t*>(ctx->Rsp + 0x28), &returnAddress);

            // A complete cb12 is exactly 0x1F0 bytes. Follow only sources whose P, V,
            // VP product identity, and render size match the live camera structure.
            if (ctx->Rdi == 0x1F && ctx->Rsi != 0 &&
                FullCb12Convention(reinterpret_cast<const unsigned char*>(
                    static_cast<uintptr_t>(ctx->Rsi))) != 0)
            {
                InterlockedIncrement(&g_builderMatches);
                if (hasReturn)
                    InterlockedCompareExchange64(&g_builderFirstCaller,
                                                  static_cast<LONG64>(returnAddress), 0);
                ctx->Dr0 = g_exeBase + kCameraUploadPostCopyRva;
                ctx->Dr7 &= ~(0xFull << 16);
                ctx->Dr7 |= 1ull;
            }
            ctx->Dr6 = 0;
            ctx->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (ctx->Rip == g_exeBase + kCameraUploadPostCopyRva)
        {
            if (ctx->Rdi >= kBuiltCbStride)
            {
                unsigned char* destination = reinterpret_cast<unsigned char*>(
                    static_cast<uintptr_t>(ctx->Rdi) - kBuiltCbStride);
                const ULONGLONG phase = GetTickCount64() % 2400;
                const float shift = phase < 1200 ? 0.35f : -0.35f;
                const int patched = PatchValidatedCameraUpload(destination, shift);
                if (patched != 0)
                {
                    InterlockedIncrement(&g_builderBlocksPatched);
                    InterlockedCompareExchange64(
                        &g_builderFirstBase, reinterpret_cast<LONG64>(destination), 0);
                }
            }

            // Re-arm this thread for the next generic upload entry.
            ctx->Dr0 = g_exeBase + kCameraUploadPreCopyRva;
            ctx->Dr7 &= ~(0xFull << 16);
            ctx->Dr7 |= 1ull;
            ctx->Dr6 = 0;
            ctx->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    const LONG index = InterlockedIncrement(&g_hitCount) - 1;
    if (index >= 0 && index < kMaxHits)
    {
        TraceHit& hit = g_hits[index];
        hit.threadId = GetCurrentThreadId();
        hit.rip = ctx->Rip;
        hit.rsp = ctx->Rsp;
        hit.regs[0] = ctx->Rax;
        hit.regs[1] = ctx->Rbx;
        hit.regs[2] = ctx->Rcx;
        hit.regs[3] = ctx->Rdx;
        hit.regs[4] = ctx->Rsi;
        hit.regs[5] = ctx->Rdi;
        hit.regs[6] = ctx->Rbp;
        hit.regs[7] = ctx->R8;
        hit.regs[8] = ctx->R9;
        hit.regs[9] = ctx->R10;
        hit.regs[10] = ctx->R11;
        hit.regs[11] = ctx->R12;
        hit.regs[12] = ctx->R13;
        hit.regs[13] = ctx->R14;
        hit.regs[14] = ctx->R15;

        const uint64_t* stack = reinterpret_cast<const uint64_t*>(ctx->Rsp);
        int found = 0;
        for (int word = 0; word < 192 && found < kCallerCapacity; ++word)
        {
            uint64_t candidate = 0;
            if (!ReadU64Guarded(stack + word, &candidate)) break;
            if (!InGameImage(candidate)) continue;

            bool duplicate = false;
            for (int i = 0; i < found; ++i)
                if (hit.callers[i] == candidate) duplicate = true;
            if (!duplicate) hit.callers[found++] = candidate;
        }
        MemoryBarrier();

        // Follow the first measured copy, before this transient destination slot is
        // reused by a later, unrelated upload. The two non-temporal copy paths leave
        // the source corresponding to the watched byte in RCX or RDX respectively.
        const uintptr_t watched = static_cast<uintptr_t>(g_watchAddress);
        LONG64 source = 0;
        if (ctx->Rip == g_exeBase + 0x2DAD05 && ctx->Rdx == watched)
        {
            source = static_cast<LONG64>(ctx->Rcx);
        }
        else if (ctx->Rip == g_exeBase + 0x2DACC9 &&
                 ctx->Rax <= watched && watched - ctx->Rax < 64)
        {
            source = static_cast<LONG64>(ctx->Rdx + (watched - ctx->Rax));
        }
        else if (ctx->Rip == g_exeBase + 0x300854 && ctx->Rdi > watched)
        {
            const uintptr_t progress = static_cast<uintptr_t>(ctx->Rdi) - watched;
            if (progress <= 64 && (progress & 7) == 0 && ctx->Rsi > progress)
                source = static_cast<LONG64>(ctx->Rsi - progress);
        }
        if (source != 0)
        {
            // The upstream allocation is normally populated once before the reporter can
            // re-arm. Snapshot both complete cb12-sized blocks synchronously in the VEH.
            const uintptr_t sourceChunk = static_cast<uintptr_t>(source);
            if (sourceChunk >= kOffV && watched >= kOffV &&
                InterlockedCompareExchange(&g_copySnapshotReady, -1, 0) == 0)
            {
                const uintptr_t sourceBase = sourceChunk - kOffV;
                const uintptr_t destinationBase = watched - kOffV;
                const bool sourceOk = ReadBytesGuarded(
                    reinterpret_cast<const unsigned char*>(sourceBase),
                    g_copySourceSnapshot, sizeof(g_copySourceSnapshot));
                const bool destinationOk = ReadBytesGuarded(
                    reinterpret_cast<const unsigned char*>(destinationBase),
                    g_copyDestinationSnapshot, sizeof(g_copyDestinationSnapshot));
                if (sourceOk && destinationOk)
                {
                    InterlockedExchange64(&g_copySourceBase,
                                              static_cast<LONG64>(sourceBase));
                    InterlockedExchange64(&g_copyDestinationBase,
                                              static_cast<LONG64>(destinationBase));
                    MemoryBarrier();
                    InterlockedExchange(&g_copySnapshotReady, 1);
                }
                else
                {
                    InterlockedExchange(&g_copySnapshotReady, 0);
                }
            }
            InterlockedCompareExchange64(&g_upstreamCandidate, source, 0);
        }
    }

    ctx->Dr6 = 0;
    if (index + 1 >= kMaxHits) ctx->Dr7 &= ~1ull;
    return EXCEPTION_CONTINUE_EXECUTION;
}

unsigned SetBreakpointOnThreads(uintptr_t address, bool enable, unsigned* busy, bool execute)
{
    const DWORD processId = GetCurrentProcessId();
    const DWORD selfId = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    unsigned applied = 0;
    unsigned occupied = 0;
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != processId || entry.th32ThreadID == selfId) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                           THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                       FALSE, entry.th32ThreadID);
            if (thread == nullptr) continue;
            const DWORD suspendResult = SuspendThread(thread);
            if (suspendResult == static_cast<DWORD>(-1))
            {
                CloseHandle(thread);
                continue;
            }

            CONTEXT context = {};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context))
            {
                bool change = true;
                if (enable && (context.Dr7 & 1ull) != 0 && context.Dr0 != address)
                {
                    ++occupied;
                    change = false; // Preserve a debugger or another probe's DR0.
                }
                if (change)
                {
                    if (enable)
                    {
                        context.Dr0 = address;
                        context.Dr7 &= ~(0xFull << 16);
                        if (!execute)
                            context.Dr7 |= (0xDull << 16); // RW0=write, LEN0=4 bytes.
                        // Execution breakpoint leaves RW0/LEN0 at zero.
                        context.Dr7 |= 1ull;
                    }
                    else if (context.Dr0 == address)
                    {
                        context.Dr7 &= ~1ull;
                        context.Dr0 = 0;
                    }
                    context.Dr6 = 0;
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(thread, &context)) ++applied;
                }
            }
            ResumeThread(thread);
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (busy != nullptr) *busy = occupied;
    return applied;
}

std::string ModuleAddress(uint64_t address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
    {
        char line[64];
        sprintf_s(line, "0x%llX", static_cast<unsigned long long>(address));
        return line;
    }

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;

    char line[320];
    sprintf_s(line, "%s+0x%llX",
              LAD8VR::Log::WideToUtf8(name).c_str(),
              static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
    return line;
}

void LogCode(const char* tag, uint64_t address)
{
    unsigned char bytes[32] = {};
    if (address < 16 || !ReadBytesGuarded(reinterpret_cast<const unsigned char*>(address - 16),
                                           bytes, sizeof(bytes)))
        return;

    char hex[sizeof(bytes) * 3 + 1] = {};
    char* cursor = hex;
    for (const unsigned char value : bytes)
    {
        sprintf_s(cursor, 4, "%02X ", value);
        cursor += 3;
    }

    char line[480];
    sprintf_s(line, "[CAMTRACE] %s %s bytes[-16..+15]=%s", tag,
              ModuleAddress(address).c_str(), hex);
    LAD8VR::Log::Line(line);
}

void DumpCapturedStage(unsigned stage, uintptr_t watched, unsigned cleared)
{
    MemoryBarrier();
    const LONG captured = g_hitCount < kMaxHits ? g_hitCount : kMaxHits;
    char line[512];
    sprintf_s(line,
              "[CAMTRACE] stage=%u complete: watched=0x%llX hits=%ld cleared=%u; dumping CPU writers",
              stage, static_cast<unsigned long long>(watched), captured, cleared);
    LAD8VR::Log::Line(line);

    uint64_t loggedRips[kMaxHits] = {};
    LONG loggedCount = 0;
    for (LONG i = 0; i < captured; ++i)
    {
        const TraceHit& hit = g_hits[i];
        bool duplicate = false;
        for (LONG j = 0; j < loggedCount; ++j)
            if (loggedRips[j] == hit.rip) duplicate = true;
        if (duplicate) continue;
        loggedRips[loggedCount++] = hit.rip;

        sprintf_s(line,
                  "[CAMTRACE] writer[%ld] tid=%lu rip=%s rsp=0x%llX "
                  "rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX rbp=%llX",
                  loggedCount - 1, hit.threadId, ModuleAddress(hit.rip).c_str(),
                  static_cast<unsigned long long>(hit.rsp),
                  static_cast<unsigned long long>(hit.regs[0]),
                  static_cast<unsigned long long>(hit.regs[1]),
                  static_cast<unsigned long long>(hit.regs[2]),
                  static_cast<unsigned long long>(hit.regs[3]),
                  static_cast<unsigned long long>(hit.regs[4]),
                  static_cast<unsigned long long>(hit.regs[5]),
                  static_cast<unsigned long long>(hit.regs[6]));
        LAD8VR::Log::Line(line);

        sprintf_s(line,
                  "[CAMTRACE]   r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX "
                  "r13=%llX r14=%llX r15=%llX",
                  static_cast<unsigned long long>(hit.regs[7]),
                  static_cast<unsigned long long>(hit.regs[8]),
                  static_cast<unsigned long long>(hit.regs[9]),
                  static_cast<unsigned long long>(hit.regs[10]),
                  static_cast<unsigned long long>(hit.regs[11]),
                  static_cast<unsigned long long>(hit.regs[12]),
                  static_cast<unsigned long long>(hit.regs[13]),
                  static_cast<unsigned long long>(hit.regs[14]));
        LAD8VR::Log::Line(line);
        LogCode("writer", hit.rip);

        for (int caller = 0; caller < kCallerCapacity && hit.callers[caller] != 0; ++caller)
        {
            sprintf_s(line, "[CAMTRACE]   caller[%d]=%s", caller,
                      ModuleAddress(hit.callers[caller]).c_str());
            LAD8VR::Log::Line(line);
            LogCode("caller", hit.callers[caller]);
        }
    }

    sprintf_s(line, "[CAMTRACE] stage=%u distinct writers=%ld", stage, loggedCount);
    LAD8VR::Log::Line(line);
}

void DumpGameCode(const wchar_t* fileName, uintptr_t rva, size_t size)
{
    std::vector<unsigned char> bytes(size);
    if (!ReadBytesGuarded(reinterpret_cast<const unsigned char*>(g_exeBase + rva),
                          bytes.data(), bytes.size()))
    {
        LAD8VR::Log::Line("[CAMTRACE] runtime code dump read failed");
        return;
    }

    wchar_t directory[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", directory, MAX_PATH) == 0) return;
    wcscat_s(directory, L"\\LAD8VR");
    CreateDirectoryW(directory, nullptr);

    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\%s", directory, fileName);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                              nullptr);
    CloseHandle(file);

    char line[360] = {};
    sprintf_s(line,
              "[CAMTRACE] runtime code dump %s RVA=0x%llX size=0x%zX wrote=%lu ok=%d",
              LAD8VR::Log::WideToUtf8(fileName).c_str(),
              static_cast<unsigned long long>(rva), size, written, ok ? 1 : 0);
    LAD8VR::Log::Line(line);
}

void DumpMemoryFile(const wchar_t* fileName, const unsigned char* address, size_t size)
{
    std::vector<unsigned char> bytes(size);
    if (!ReadBytesGuarded(address, bytes.data(), bytes.size()))
    {
        LAD8VR::Log::Line("[CAMOWNER] memory dump read failed");
        return;
    }

    wchar_t directory[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", directory, MAX_PATH) == 0) return;
    wcscat_s(directory, L"\\LAD8VR");
    CreateDirectoryW(directory, nullptr);

    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\%s", directory, fileName);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                              nullptr);
    CloseHandle(file);

    char line[360] = {};
    sprintf_s(line, "[CAMOWNER] dump %s address=0x%p size=0x%zX wrote=%lu ok=%d",
              LAD8VR::Log::WideToUtf8(fileName).c_str(), address, size, written, ok ? 1 : 0);
    LAD8VR::Log::Line(line);
}

bool IsStackSource(uintptr_t source)
{
    const LONG captured = g_hitCount < kMaxHits ? g_hitCount : kMaxHits;
    for (LONG i = 0; i < captured; ++i)
    {
        const uintptr_t rsp = static_cast<uintptr_t>(g_hits[i].rsp);
        const uintptr_t distance = source > rsp ? source - rsp : rsp - source;
        if (distance <= (8u << 20)) return true;
    }
    return false;
}

DWORD WINAPI ReporterThread(LPVOID)
{
    constexpr unsigned kMaxStages = 4;
    for (unsigned stage = 0; stage < kMaxStages && g_stop == 0; ++stage)
    {
        const ULONGLONG started = GetTickCount64();
        while (g_stop == 0 && g_hitCount < kMaxHits &&
               g_upstreamCandidate == 0 && GetTickCount64() - started < 25000)
            Sleep(100);

        const uintptr_t watched = static_cast<uintptr_t>(g_watchAddress);
        unsigned busy = 0;
        const unsigned cleared = SetBreakpointOnThreads(watched, false, &busy, false);
        InterlockedExchange(&g_armed, 0);
        DumpCapturedStage(stage, watched, cleared);

        if (stage == 0)
        {
            // Denuvo leaves the on-disk text unusable, so save only the live code ranges
            // proven by this trace. These are small, read-only snapshots for offline
            // disassembly of the upload routine and its two caller levels.
            DumpGameCode(L"camtrace_rva_00060000.bin", 0x00060000, 0x00010000);
            DumpGameCode(L"camtrace_rva_00300000.bin", 0x00300000, 0x00010000);
            DumpGameCode(L"camtrace_rva_02780000.bin", 0x02780000, 0x00010000);
            DumpGameCode(L"camtrace_rva_027B0000.bin", 0x027B0000, 0x00020000);
            DumpGameCode(L"camtrace_rva_02860000.bin", 0x02860000, 0x00010000);
            if (g_copySnapshotReady == 1)
            {
                DumpMemoryFile(L"camtrace_copy_source.bin", g_copySourceSnapshot,
                               sizeof(g_copySourceSnapshot));
                DumpMemoryFile(L"camtrace_copy_destination.bin",
                               g_copyDestinationSnapshot,
                               sizeof(g_copyDestinationSnapshot));
                char snapshotLine[320] = {};
                sprintf_s(snapshotLine,
                          "[CAMTRACE] synchronous copy snapshot source=0x%llX destination=0x%llX",
                          static_cast<unsigned long long>(g_copySourceBase),
                          static_cast<unsigned long long>(g_copyDestinationBase));
                LAD8VR::Log::Line(snapshotLine);
            }
        }

        const uintptr_t upstream = static_cast<uintptr_t>(
            InterlockedExchange64(&g_upstreamCandidate, 0));
        if (upstream != 0 && IsStackSource(upstream))
        {
            char line[280] = {};
            sprintf_s(line,
                      "[CAMTRACE] STOP upstream=0x%llX is a transient stack source; "
                      "runtime caller code captured for producer analysis",
                      static_cast<unsigned long long>(upstream));
            LAD8VR::Log::Line(line);
            return 0;
        }
        if (g_stop != 0 || upstream == 0 || upstream == watched || stage + 1 >= kMaxStages)
            return 0;

        char line[320];
        sprintf_s(line,
                  "[CAMTRACE] CHASE stage=%u -> stage=%u destination=0x%llX upstream=0x%llX",
                  stage, stage + 1,
                  static_cast<unsigned long long>(watched),
                  static_cast<unsigned long long>(upstream));
        LAD8VR::Log::Line(line);

        ZeroMemory(g_hits, sizeof(g_hits));
        InterlockedExchange(&g_hitCount, 0);
        InterlockedExchange64(&g_watchAddress, static_cast<LONG64>(upstream));
        InterlockedExchange(&g_armed, 1);

        const unsigned applied = SetBreakpointOnThreads(upstream, true, &busy, false);
        sprintf_s(line,
                  "[CAMTRACE] stage=%u ARMED upstream write breakpoint 0x%llX across %u threads "
                  "(%u DR0 slots already occupied)",
                  stage + 1, static_cast<unsigned long long>(upstream), applied, busy);
        LAD8VR::Log::Line(line);
        if (applied == 0)
        {
            InterlockedExchange(&g_armed, 0);
            return 0;
        }
    }
    return 0;
}

DWORD WINAPI OwnerReporterThread(LPVOID)
{
    const ULONGLONG started = GetTickCount64();
    while (g_stop == 0 && g_ownerStage < 2 && GetTickCount64() - started < 25000)
        Sleep(50);

    unsigned busy = 0;
    SetBreakpointOnThreads(g_exeBase + kCameraBuilderCallRva, false, &busy, true);
    SetBreakpointOnThreads(g_exeBase + kCameraUploadPreCopyRva, false, &busy, true);
    InterlockedExchange(&g_armed, 0);

    char line[420] = {};
    sprintf_s(line,
              "[CAMOWNER] mapping complete: stage=%ld tid=%ld owner=0x%llX output=0x%llX "
              "source=0x%llX",
              g_ownerStage, g_ownerThread,
              static_cast<unsigned long long>(g_ownerAddress),
              static_cast<unsigned long long>(g_ownerOutput),
              static_cast<unsigned long long>(g_ownerSource));
    LAD8VR::Log::Line(line);

    if (g_ownerStage == 2 && g_ownerSnapshotReady != 0)
    {
        DumpMemoryFile(L"camowner_object.bin", g_ownerObjectSnapshot,
                       sizeof(g_ownerObjectSnapshot));
        DumpMemoryFile(L"camowner_cbpair.bin", g_ownerCbpairSnapshot,
                       sizeof(g_ownerCbpairSnapshot));
        LAD8VR::Log::Line("[CAMOWNER] synchronous owner/camera snapshot pair written");
    }
    return 0;
}

DWORD WINAPI BuilderReporterThread(LPVOID)
{
    const ULONGLONG started = GetTickCount64();
    ULONGLONG lastReport = 0;
    while (g_stop == 0)
    {
        Sleep(250);
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed - lastReport < 3000) continue;
        lastReport = elapsed;

        char line[420] = {};
        sprintf_s(line,
                  "[CAMBUILDER] validated cb12 proof: generic=%ld matched=%ld patched=%ld firstDest=0x%llX firstCaller=%s",
                  g_builderHits, g_builderMatches, g_builderBlocksPatched,
                  static_cast<unsigned long long>(g_builderFirstBase),
                  ModuleAddress(static_cast<uintptr_t>(g_builderFirstCaller)).c_str());
        LAD8VR::Log::Line(line);
    }

    // Never race live worker threads by clearing DR0 here. The operating system
    // discards every thread's debug registers during process teardown.
    LAD8VR::Log::Line("[CAMBUILDER] process stopping; DR0 cleanup left to OS teardown");
    return 0;
}

LONG ReadModeFromIni()
{
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ReadModeFromIni), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) wcscpy_s(slash + 1, 32, L"lad8vr.ini");
    if (GetPrivateProfileIntW(L"Camera", L"OwnerTrace", 0, path) != 0) return 3;
    if (GetPrivateProfileIntW(L"Camera", L"BuilderProof", 0, path) != 0) return 2;
    if (GetPrivateProfileIntW(L"Camera", L"TraceWriter", 0, path) != 0) return 1;
    return 0;
}
}

namespace LAD8VR::CamTrace
{
bool Enabled() noexcept
{
    LONG state = g_enabledState;
    if (state < 0)
    {
        state = ReadModeFromIni();
        InterlockedCompareExchange(&g_enabledState, state, -1);
        state = g_enabledState;
    }
    return state != 0;
}

void MaybeArm(const unsigned char* address) noexcept
{
    if (address == nullptr || !Enabled()) return;
    if (InterlockedCompareExchange(&g_traceStarted, 1, 0) != 0) return;
    InterlockedExchange(&g_armed, 1);

    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_exeBase + dos->e_lfanew);
    g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
    const bool ownerTrace = g_enabledState == 3;
    const bool builderProof = g_enabledState == 2;
    const uintptr_t initialWatch = ownerTrace
        ? g_exeBase + kCameraBuilderCallRva
        : (builderProof ? g_exeBase + kCameraUploadPreCopyRva
                        : reinterpret_cast<uintptr_t>(address));
    InterlockedExchange64(&g_watchAddress, static_cast<LONG64>(initialWatch));
    InterlockedExchange64(&g_upstreamCandidate, 0);
    InterlockedExchange(&g_hitCount, 0);
    InterlockedExchange(&g_builderHits, 0);
    InterlockedExchange(&g_builderMatches, 0);
    InterlockedExchange(&g_builderBlocksPatched, 0);
    InterlockedExchange64(&g_builderFirstBase, 0);
    InterlockedExchange64(&g_builderFirstCaller, 0);
    InterlockedExchange(&g_ownerStage, 0);
    InterlockedExchange(&g_ownerThread, 0);
    InterlockedExchange64(&g_ownerAddress, 0);
    InterlockedExchange64(&g_ownerOutput, 0);
    InterlockedExchange64(&g_ownerSource, 0);
    InterlockedExchange(&g_ownerSnapshotReady, 0);
    InterlockedExchange(&g_copySnapshotReady, 0);
    InterlockedExchange64(&g_copySourceBase, 0);
    InterlockedExchange64(&g_copyDestinationBase, 0);
    InterlockedExchange(&g_stop, 0);

    if (g_veh == nullptr) g_veh = AddVectoredExceptionHandler(1, &TraceVeh);
    if (g_veh == nullptr)
    {
        LAD8VR::Log::Line("[CAMTRACE] AddVectoredExceptionHandler failed");
        InterlockedExchange(&g_armed, 0);
        return;
    }

    unsigned busy = 0;
    const unsigned applied = SetBreakpointOnThreads(
        static_cast<uintptr_t>(g_watchAddress), true, &busy, ownerTrace || builderProof);
    char line[320];
    if (ownerTrace)
    {
        sprintf_s(line,
                  "[CAMOWNER] ARMED one-shot owner mapper at LikeADragon8.exe+0x%llX "
                  "across %u threads (%u DR0 slots already occupied)",
                  static_cast<unsigned long long>(kCameraBuilderCallRva), applied, busy);
    }
    else if (builderProof)
    {
        sprintf_s(line,
                  "[CAMBUILDER] ARMED pre-copy camera upload breakpoint at LikeADragon8.exe+0x%llX "
                  "across %u threads (%u DR0 slots already occupied)",
                  static_cast<unsigned long long>(kCameraUploadPreCopyRva), applied, busy);
    }
    else
    {
        sprintf_s(line,
                  "[CAMTRACE] ARMED write breakpoint at cb12.c_matV[0] 0x%llX across %u threads "
                  "(%u DR0 slots already occupied)",
                  static_cast<unsigned long long>(g_watchAddress), applied, busy);
    }
    LAD8VR::Log::Line(line);

    if (applied == 0)
    {
        InterlockedExchange(&g_armed, 0);
        return;
    }
    LPTHREAD_START_ROUTINE reporter = ownerTrace
        ? &OwnerReporterThread
        : (builderProof ? &BuilderReporterThread : &ReporterThread);
    CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
}

void Stop() noexcept
{
    InterlockedExchange(&g_stop, 1);
}
}
