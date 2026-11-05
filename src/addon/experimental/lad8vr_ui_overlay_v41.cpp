// Like a Dragon: Infinite Wealth VR - EXPERIMENTAL add-on variant (internal build v41).
// Copyright (C) 2026 Halcyon. Licensed under the GNU General Public License v3.0 (see LICENSE).
//
// UNTESTED. This is the last attempt to get the minimap into both eyes. It was built but there
// is no record of it ever being run in the game. It is the v37 release add-on plus an
// "overlay route": a small draw is duplicated if it targets the busiest HUD anchor's render
// target, happens after the last eye-1 scene draw of the previous frame, and is issued during
// the eye-0 phase. Read the minimap section of docs/TECHNICAL_NOTES.md before running it: four
// earlier rules of this kind tiled the whole screen. Build with: scripts\build.bat v41
//
// UI overlay v41 -- the UI phase on the HUD texture, no per-element classification.
//
// v23 established the mechanism. Two findings drive this build:
//
//   1. The side-by-side split is NOT viewport-based. Across 300 presents with stereo active,
//      not one draw bound a half-width viewport (eyeLeft=0, eyeRight=0); every scene draw is
//      full-width on the 3840x2160 target. The engine bakes the half-screen mapping into its
//      per-eye projection instead. (An earlier probe reached the same conclusion from the other
//      direction: "viewport-only draw classification is an invalid assumption for this path".)
//
//   2. The HUD is drawn ONCE, LAST, during the eye-0 phase, full width. v23's per-draw engine
//      eye index makes this unambiguous: almost every pipeline averages meanEye ~= 0.49 (drawn
//      for both eyes), while the HUD pipelines are exactly 0.00 and carry the highest mean draw
//      order in the frame. Because that single pass spans the whole side-by-side target in
//      screen space, screen-left elements (minimap, context prompt) land only in the left eye
//      and centre/right elements (party cards, vehicle indicator) only in the right.
//
// Fix: for each HUD draw, bind a half-width viewport and scissor and issue the draw twice --
// once into the left half, once into the right -- then skip the original. Because the UI's
// vertices already resolve to NDC spanning the full screen, a half-width viewport maps the
// COMPLETE HUD into that half. Both eyes then see the whole HUD, horizontally compressed, which
// is exactly the anamorphic form a side-by-side frame expects.
//
// The HUD pipelines are identified at RUNTIME rather than hardcoded, because pipeline handles
// differ every launch. During a learning phase the probe records, per pipeline, its draw counts
// per engine eye and its mean position in the frame; a pipeline qualifies as UI only if it draws
// exclusively at eye 0, sits late in the frame, and targets the full-width main render target.
//
// Modes (F11 cycles):
//   0 DUPLICATE (default) -- the fix: HUD drawn into both halves
//   1 HIDE                -- skip HUD draws entirely; proves the identified set IS the HUD
//   2 NORMAL              -- untouched, for A/B comparison
//
// Controls:
//   F9  arm native dual view + the v21 gates, and begin learning
//   F10 toggle the +/-0.032 geometric eye separation
//   F11 cycle mode

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "reshade/reshade.hpp"

using namespace reshade::api;

namespace
{
constexpr uintptr_t kActivationRva = 0x048EB584;
constexpr uintptr_t kEyeIndexRva = 0x048EB588;
constexpr uintptr_t kEyeMatricesRva = 0x048EB590;
constexpr uintptr_t kInitializerRva = 0x027FDCB0;
constexpr DWORD kRefreshMilliseconds = 250;
constexpr float kHalfIpd = 0.032f;
// v25 learned once, across the first 120 presents. Whatever context the player happened to be in
// during those frames defined the HUD set for the whole session, so battle-only UI pipelines were
// never seen. The visible result was a DOUBLED enemy list in the right eye: the duplicated copy
// appeared in both halves while the unidentified original still drew full width and landed in the
// right half only. Learning is continuous now -- sampled on a fraction of frames so the cost stays
// negligible, with the UI set re-evaluated periodically so a new context is picked up within a few
// frames of appearing.
// v28 worked, but the UI took a visible moment to come good -- on arming and again on entering
// battle. That wait was the identification schedule, not the drawing: 120 presents before acting
// at all, then a new pipeline needed 12 observed frames (sampled one in four, so ~48 presents)
// and then had to wait for the next scheduled re-check, up to 60 presents later. Roughly two
// seconds in the field, and up to another second and a half entering battle.
//
// Identification is event-driven now. A pipeline the probe has never seen before raises a flag
// and the next present re-evaluates immediately rather than waiting for the interval. The
// evidence bar is lower but still safe, because the qualifying test is what does the real work:
// eye-0-only on the full-width side-by-side target.
constexpr LONG kLearnPresents = 45;       // dense up-front learning before acting at all
constexpr LONG kLearnSampleEvery = 4;     // afterwards, learn on one frame in four
constexpr LONG kReselectInterval = 15;    // scheduled re-evaluation, now only a backstop
constexpr LONG kReselectMinGap = 8;       // never re-evaluate more often than this
constexpr uint64_t kMinFramesSeen = 5;    // sustained evidence, reached within ~20 presents
constexpr uint32_t kMinSbsWidth = 2560;   // only treat wide targets as the side-by-side frame
// The lateness test is GONE. v26's log showed it was the entire problem: the threshold is a
// fraction of the largest frame ever seen, that maximum kept climbing (19,315 -> 20,349), and it
// dragged the bar up through the HUD's own mean order (~9,900-10,250). The HUD was selected at
// 120 and 240 presents and then DE-SELECTED at 360 -- which is exactly the observed "fine at
// first, then back to one UI".
//
// It was never needed. The eye-0 fraction separates the two populations with an enormous margin,
// stable across the whole session:
//     every scene pipeline   eye0Frac ~= 0.50 - 0.54   (drawn for both eyes)
//     the HUD pipelines      eye0Frac  = 1.000         (0xF84AC780, 0xCF2909FC0)
// Combined with full-width-on-the-SBS-target and a per-frame volume floor, that alone selects
// exactly the HUD and nothing else.
constexpr double kMinEye0Fraction = 0.95;   // HUD is exactly 1.000; scene work never exceeds ~0.54
constexpr uint64_t kMinDrawsPerFrame = 20;  // ANCHOR tier: excludes full-screen post passes
// v27 fixed the field HUD but left battle UI untouched. Battle draws its pieces through extra
// pipelines that issue far fewer calls per frame than the main HUD, so the volume floor that
// (correctly) keeps full-screen post passes out was keeping battle UI out too.
//
// Lowering the floor outright is unsafe: a tone-map or composite pass is also eye-0-only and
// full-width, and squeezing one into each half would wreck the image. The real discriminator is
// WHEN it happens -- post runs before the overlay, battle UI runs alongside it. Selection is
// therefore two-tier: anchors are found by volume as before, and companions may have low volume
// provided they draw no earlier than the anchors. The reference is the anchors' own measured
// position, NOT a fraction of the largest frame ever seen; that was v26's mistake and it is not
// repeated here.
constexpr uint64_t kCompanionMinDrawsPerFrame = 3;
constexpr double kCompanionOrderMargin = 0.90; // may begin slightly before the earliest anchor

struct Gate
{
    uintptr_t jeRva;
    uintptr_t fallRva;
    const char *name;
};

constexpr Gate kGates[] = {
    {0x0165F34B, 0x0165F34D, "static"},
    {0x0165C40B, 0x0165C411, "bucket16"},
    {0x016767A3, 0x016767A9, "bucket21_22"},
};
constexpr size_t kGateCount = sizeof(kGates) / sizeof(kGates[0]);

enum Mode : LONG { kDuplicate = 0, kHide = 1, kNormal = 2, kModeCount = 3 };
const char *kModeName[kModeCount] = {"DUPLICATE", "HIDE", "NORMAL"};

struct CmdState
{
    uint64_t pipeline = 0;
    float x = 0, y = 0, width = 0, height = 0;
    bool viewportSet = false;
    uint64_t rtResource = 0;   // which texture this draw writes into
    uint32_t rtWidth = 0, rtHeight = 0;
};

struct LearnStat
{
    uint64_t eye0 = 0, eye1 = 0;
    uint64_t orderSum = 0, count = 0;
    uint64_t framesSeen = 0;      // frames this pipeline drew in, so per-frame volume is honest
    LONG lastPresent = -1;        // dedup for framesSeen
    uint32_t rtWidth = 0;
    uint32_t maxElements = 0;     // largest single draw this pipeline has issued
    uint64_t vsHash = 0;          // VERTEX+PIXEL pair identity, from init_pipeline
    uint64_t rtResource = 0;      // the texture this pipeline draws into
    bool fullWidth = false;
};

// Statistical UI detection worked for the main HUD and then failed steadily on the minimap, the
// battle UI, and menus, because those elements do not share the HUD's statistical profile. What
// they DO share is the UI shader. v36 keys off shader identity instead of behaviour:
//
//   * init_pipeline records each pipeline's vertex-shader hash.
//   * A pipeline is a UI-SHADER SOURCE if it draws eye-0-only on the full-width SBS target and has
//     never issued a large mesh -- i.e. a genuine 2D overlay pipeline. Its vertex shader is a UI
//     shader.
//   * A draw is UI if its pipeline's vertex shader is one of those UI shaders and the draw itself
//     is small. This generalises to any 2D element sharing the shader (minimap, battle UI) while
//     excluding 3D meshes, which never use a UI shader, however they are batched.
constexpr uint32_t kMaxUiDrawElements = 512;      // per draw: above this it is geometry, not UI
constexpr uint32_t kMaxUiPipelineElements = 4096; // a pipeline drawing meshes this big is not UI
constexpr size_t kMaxSelectedPipelines = 24;      // hard cap: a bigger set means the criteria are wrong

CRITICAL_SECTION g_logLock;
wchar_t g_logPath[MAX_PATH] = {};
wchar_t g_crashPath[MAX_PATH] = {};
HANDLE g_crashFile = INVALID_HANDLE_VALUE;
PVOID g_veh = nullptr;
uintptr_t g_imageBase = 0;
volatile LONG g_enabled = 0;
volatile LONG g_workerLive = 0;
volatile LONG g_learning = 0;
volatile LONG g_active = 0;
volatile LONG g_mode = kDuplicate;
volatile LONG g_separated = 0;
volatile LONG g_initializerHits = 0;
volatile LONG g_gateOpened[kGateCount] = {};
volatile LONG g_crashCaptured = 0;
volatile LONG g_presents = 0;
volatile LONG g_duplicated = 0;
volatile LONG g_hidden = 0;
volatile LONG g_newPipelineSeen = 0;   // set when a pipeline is observed for the first time
volatile LONG g_lastSelectPresent = 0; // rate-limits event-driven re-evaluation
std::atomic<uint64_t> g_frameDrawIndex{0};
std::atomic<uint64_t> g_maxOrderSeen{0};
std::shared_mutex g_cmdLock;
std::unordered_map<command_list *, CmdState> g_cmdStates;
std::mutex g_learnLock;
std::unordered_map<uint64_t, LearnStat> g_learnStats;
std::shared_mutex g_uiLock;
std::unordered_set<uint64_t> g_uiPipelines;   // pipelines whose draws are duplicated
std::shared_mutex g_shaderLock;
std::unordered_map<uint64_t, uint64_t> g_pipelineVs; // pipeline handle -> vertex shader hash
std::unordered_set<uint64_t> g_uiShaders;       // shader pairs belonging to UI
// The frame capture settled how to reach the minimap and the stragglers without classifying them.
// Scene work for both eyes finishes at a definite point -- the last draw made while the engine eye
// index is 1. After that, on the ONE texture the high-volume HUD draws into, the capture found 479
// draws with a maximum element count of 60: no composite, no mesh, nothing but small overlay
// primitives. 468 were already duplicated; the remaining 11 are the minimap and its neighbours.
//
// So the rule is physical rather than statistical: a small draw, into the HUD's own texture, after
// the scene has finished, during the eye-0 phase. v40 failed because it accepted FIVE anchor
// targets including incidental ones; only the busiest matters.
std::atomic<uint64_t> g_primaryUiTarget{0};   // texture of the highest-volume HUD pipeline
std::atomic<uint64_t> g_sceneEndOrder{0};     // last eye-1 draw of the PREVIOUS frame
std::atomic<uint64_t> g_frameMaxEye1{0};      // running max for the current frame
alignas(16) float g_originalMatrices[32] = {};
bool g_originalMatricesValid = false;

void Log(const char *text)
{
    EnterCriticalSection(&g_logLock);
    FILE *file = nullptr;
    if (_wfopen_s(&file, g_logPath, L"a") == 0 && file != nullptr)
    {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        fprintf(file, "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds, text);
        fclose(file);
    }
    LeaveCriticalSection(&g_logLock);
}

void CrashWrite(const char *text)
{
    if (g_crashFile == INVALID_HANDLE_VALUE || text == nullptr) return;
    DWORD written = 0;
    WriteFile(g_crashFile, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    FlushFileBuffers(g_crashFile);
}

bool ReadMem(uintptr_t address, void *out, size_t size)
{
    SIZE_T count = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), out,
                             size, &count) != FALSE && count == size;
}

bool FastReadU32(uintptr_t address, uint32_t &value)
{
    __try
    {
        value = *reinterpret_cast<volatile const uint32_t *>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        value = 0xFFFFFFFFu;
        return false;
    }
}

bool WriteMem(uintptr_t address, const void *data, size_t size)
{
    SIZE_T count = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), data,
                              size, &count) != FALSE && count == size;
}

void BuildEyeMatrices(bool separated, float (&matrices)[32])
{
    memset(matrices, 0, sizeof(matrices));
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        float *matrix = matrices + eye * 16;
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    }
    if (separated)
    {
        matrices[12] = +kHalfIpd;
        matrices[16 + 12] = -kHalfIpd;
    }
}

bool ApplyEyeMatrices(bool separated)
{
    float matrices[32] = {};
    BuildEyeMatrices(separated, matrices);
    return WriteMem(g_imageBase + kEyeMatricesRva, matrices, sizeof(matrices));
}

void RestoreNativeState()
{
    const uint32_t zero = 0;
    WriteMem(g_imageBase + kActivationRva, &zero, sizeof(zero));
    if (g_originalMatricesValid)
        WriteMem(g_imageBase + kEyeMatricesRva, g_originalMatrices, sizeof(g_originalMatrices));
}

bool IsOurBreakpoint(const CONTEXT *cpu, uintptr_t target)
{
    const uintptr_t slots[4] = {
        static_cast<uintptr_t>(cpu->Dr0), static_cast<uintptr_t>(cpu->Dr1),
        static_cast<uintptr_t>(cpu->Dr2), static_cast<uintptr_t>(cpu->Dr3)};
    for (unsigned slot = 0; slot < 4; ++slot)
        if ((cpu->Dr6 & (1ull << slot)) != 0 && slots[slot] == target) return true;
    return false;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS *exception)
{
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    EXCEPTION_RECORD *record = exception->ExceptionRecord;
    CONTEXT *cpu = exception->ContextRecord;
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP &&
        InterlockedCompareExchange(&g_enabled, 0, 0) != 0)
    {
        const uintptr_t rip = static_cast<uintptr_t>(cpu->Rip);
        if (rip == g_imageBase + kInitializerRva && IsOurBreakpoint(cpu, rip))
        {
            const uint32_t mode = 1;
            WriteMem(g_imageBase + kActivationRva, &mode, sizeof(mode));
            InterlockedIncrement(&g_initializerHits);
            cpu->Dr6 = 0;
            cpu->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        for (size_t index = 0; index < kGateCount; ++index)
        {
            if (rip != g_imageBase + kGates[index].jeRva || !IsOurBreakpoint(cpu, rip)) continue;
            uint32_t identity = 0xFFFFFFFFu;
            FastReadU32(static_cast<uintptr_t>(cpu->Rax), identity);
            if (identity == 2)
            {
                InterlockedIncrement(&g_gateOpened[index]);
                cpu->Rip = g_imageBase + kGates[index].fallRva;
            }
            cpu->Dr6 = 0;
            cpu->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        InterlockedCompareExchange(&g_enabled, 0, 0) != 0 &&
        InterlockedCompareExchange(&g_crashCaptured, 1, 0) == 0)
    {
        char line[448] = {};
        sprintf_s(line,
                  "[CRASH] code=0x%08lX rip=0x%llX rva=0x%llX tid=%lu mode=%ld duplicated=%ld\r\n",
                  record->ExceptionCode, static_cast<unsigned long long>(cpu->Rip),
                  static_cast<unsigned long long>(
                      static_cast<uintptr_t>(cpu->Rip) >= g_imageBase
                          ? static_cast<uintptr_t>(cpu->Rip) - g_imageBase : 0),
                  GetCurrentThreadId(), InterlockedCompareExchange(&g_mode, 0, 0),
                  InterlockedCompareExchange(&g_duplicated, 0, 0));
        CrashWrite(line);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool SetThreadBreakpoint(DWORD tid, DWORD helperTid, bool enable)
{
    if (tid == helperTid) return false;
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
        uintptr_t targets[1 + kGateCount] = {g_imageBase + kInitializerRva};
        for (size_t index = 0; index < kGateCount; ++index)
            targets[1 + index] = g_imageBase + kGates[index].jeRva;
        DWORD64 *slots[4] = {&cpu.Dr0, &cpu.Dr1, &cpu.Dr2, &cpu.Dr3};
        bool changed = false;
        for (uintptr_t target : targets)
        {
            int presentSlot = -1;
            for (unsigned slot = 0; slot < 4; ++slot)
                if (((cpu.Dr7 >> (slot * 2)) & 0x3ull) != 0 &&
                    static_cast<uintptr_t>(*slots[slot]) == target)
                    presentSlot = static_cast<int>(slot);
            if (enable && presentSlot < 0)
            {
                for (unsigned slot = 0; slot < 4; ++slot)
                {
                    if (((cpu.Dr7 >> (slot * 2)) & 0x3ull) != 0) continue;
                    *slots[slot] = target;
                    cpu.Dr7 &= ~(0xFull << (16 + slot * 4));
                    cpu.Dr7 |= (1ull << (slot * 2));
                    changed = true;
                    break;
                }
            }
            else if (!enable && presentSlot >= 0)
            {
                const unsigned slot = static_cast<unsigned>(presentSlot);
                cpu.Dr7 &= ~(0x3ull << (slot * 2));
                cpu.Dr7 &= ~(0xFull << (16 + slot * 4));
                *slots[slot] = 0;
                changed = true;
            }
        }
        if (changed)
        {
            cpu.Dr6 = 0;
            ok = SetThreadContext(thread, &cpu) != FALSE;
        }
        else ok = true;
    }
    ResumeThread(thread);
    CloseHandle(thread);
    return ok;
}

void ApplyAllThreads(DWORD helperTid, bool enable, uint32_t &seen, uint32_t &applied)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    seen = applied = 0;
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() ||
                entry.th32ThreadID == helperTid) continue;
            ++seen;
            if (SetThreadBreakpoint(entry.th32ThreadID, helperTid, enable)) ++applied;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

CmdState StateCopy(command_list *cmd)
{
    std::shared_lock lock(g_cmdLock);
    const auto found = g_cmdStates.find(cmd);
    return found != g_cmdStates.end() ? found->second : CmdState{};
}

void OnInitCommandList(command_list *cmd)
{
    std::unique_lock lock(g_cmdLock);
    g_cmdStates.try_emplace(cmd);
}

void OnResetCommandList(command_list *cmd)
{
    std::unique_lock lock(g_cmdLock);
    g_cmdStates[cmd] = CmdState{};
}

void OnDestroyCommandList(command_list *cmd)
{
    std::unique_lock lock(g_cmdLock);
    g_cmdStates.erase(cmd);
}

uint64_t HashBytes(const void *data, size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

// Record each pipeline's shader identity as it is created. The identity is the VERTEX+PIXEL
// pair: v36 keyed on the vertex shader alone and eventually tiled the screen, because the game
// shares one generic full-screen-quad vertex shader between UI sprites and composite passes.
// Once a busy menu pipeline seeded that shared vertex shader, the composite inherited UI status
// and was squeezed into each half. The pixel shaders never collide -- a composite samples the
// scene, a sprite samples an atlas -- so the pair is a true identity.
void OnInitPipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects,
                    pipeline pipe)
{
    if (subobjects == nullptr || pipe.handle == 0) return;
    uint64_t vsHash = 0, psHash = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (subobjects[i].data == nullptr) continue;
        if (subobjects[i].type != pipeline_subobject_type::vertex_shader &&
            subobjects[i].type != pipeline_subobject_type::pixel_shader)
            continue;
        const auto *desc = static_cast<const shader_desc *>(subobjects[i].data);
        if (desc->code == nullptr || desc->code_size == 0) continue;
        const uint64_t hash = HashBytes(desc->code, desc->code_size);
        if (subobjects[i].type == pipeline_subobject_type::vertex_shader) vsHash = hash;
        else psHash = hash;
    }
    if (vsHash == 0 && psHash == 0) return;
    // Combine into one pair identity. Distinct constants keep (vs,ps) and (ps,vs) apart.
    const uint64_t pair = (vsHash * 1099511628211ull) ^ (psHash + 1469598103934665603ull);
    std::unique_lock lock(g_shaderLock);
    g_pipelineVs[pipe.handle] = pair;
}

uint64_t VsHashOf(uint64_t pipeline)
{
    std::shared_lock lock(g_shaderLock);
    const auto found = g_pipelineVs.find(pipeline);
    return found != g_pipelineVs.end() ? found->second : 0;
}

void OnBindPipeline(command_list *cmd, pipeline_stage stages, pipeline pipe)
{
    if ((static_cast<uint32_t>(stages) &
         static_cast<uint32_t>(pipeline_stage::vertex_shader)) == 0) return;
    std::unique_lock lock(g_cmdLock);
    g_cmdStates[cmd].pipeline = pipe.handle;
}

void OnBindViewports(command_list *cmd, uint32_t first, uint32_t count, const viewport *viewports)
{
    if (first != 0 || count == 0 || viewports == nullptr) return;
    std::unique_lock lock(g_cmdLock);
    CmdState &state = g_cmdStates[cmd];
    state.x = viewports[0].x;
    state.y = viewports[0].y;
    state.width = viewports[0].width;
    state.height = viewports[0].height;
    state.viewportSet = true;
}

void SetPrimaryRenderTarget(command_list *cmd, resource_view rtv)
{
    uint32_t width = 0, height = 0;
    uint64_t resourceValue = 0;
    if (rtv.handle != 0)
    {
        device *dev = cmd->get_device();
        const resource resourceHandle = dev->get_resource_from_view(rtv);
        resourceValue = resourceHandle.handle;
        if (resourceHandle.handle != 0)
        {
            const resource_desc desc = dev->get_resource_desc(resourceHandle);
            if (desc.type == resource_type::texture_2d)
            {
                width = static_cast<uint32_t>(desc.texture.width);
                height = desc.texture.height;
            }
        }
    }
    std::unique_lock lock(g_cmdLock);
    CmdState &state = g_cmdStates[cmd];
    state.rtResource = resourceValue;
    state.rtWidth = width;
    state.rtHeight = height;
}

void OnBindRenderTargets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view)
{
    SetPrimaryRenderTarget(cmd, count != 0 && rtvs != nullptr ? rtvs[0] : resource_view{});
}

void OnBeginRenderPass(command_list *cmd, uint32_t count,
                       const render_pass_render_target_desc *targets,
                       const render_pass_depth_stencil_desc *)
{
    SetPrimaryRenderTarget(cmd, count != 0 && targets != nullptr ? targets[0].view : resource_view{});
}

bool IsSbsFullWidth(const CmdState &state)
{
    return state.viewportSet && state.rtWidth >= kMinSbsWidth &&
           state.width >= static_cast<float>(state.rtWidth) * 0.9f &&
           state.height >= static_cast<float>(state.rtHeight) * 0.9f;
}

bool IsUiPipeline(uint64_t pipeline)
{
    if (InterlockedCompareExchange(&g_active, 0, 0) == 0) return false;
    std::shared_lock lock(g_uiLock);
    return g_uiPipelines.find(pipeline) != g_uiPipelines.end();
}

// Re-issue one HUD draw into each half of the side-by-side frame. The UI's vertices already
// resolve to NDC spanning the full screen, so a half-width viewport maps the whole HUD into
// that half; doing it twice puts a complete HUD in both eyes.
template <typename Issue>
bool DuplicateAcrossHalves(command_list *cmd, const CmdState &state, Issue &&issue)
{
    const float fullW = static_cast<float>(state.rtWidth);
    const float fullH = static_cast<float>(state.rtHeight);
    const float halfW = fullW * 0.5f;

    viewport half = {};
    half.y = 0.0f;
    half.width = halfW;
    half.height = fullH;
    half.min_depth = 0.0f;
    half.max_depth = 1.0f;

    for (unsigned side = 0; side < 2; ++side)
    {
        half.x = side == 0 ? 0.0f : halfW;
        const rect scissor = {
            static_cast<int32_t>(half.x), 0,
            static_cast<int32_t>(half.x + halfW), static_cast<int32_t>(fullH)};
        cmd->bind_viewports(0, 1, &half);
        cmd->bind_scissor_rects(0, 1, &scissor);
        issue();
    }

    // Restore what the game had bound so subsequent draws are untouched.
    viewport restore = {};
    restore.x = state.x;
    restore.y = state.y;
    restore.width = state.width;
    restore.height = state.height;
    restore.min_depth = 0.0f;
    restore.max_depth = 1.0f;
    const rect fullScissor = {0, 0, static_cast<int32_t>(fullW), static_cast<int32_t>(fullH)};
    cmd->bind_viewports(0, 1, &restore);
    cmd->bind_scissor_rects(0, 1, &fullScissor);
    InterlockedIncrement(&g_duplicated);
    return true;
}

void LearnDraw(const CmdState &state, uint64_t order, uint32_t eye, LONG present, uint32_t elements)
{
    std::lock_guard lock(g_learnLock);
    const bool firstSighting = g_learnStats.find(state.pipeline) == g_learnStats.end();
    LearnStat &stat = g_learnStats[state.pipeline];
    // A pipeline never seen before means the context just changed (battle started, a menu
    // opened). Flag it so the next present re-evaluates immediately rather than waiting for the
    // scheduled interval -- that wait is what made the UI take a moment to come good.
    if (firstSighting && IsSbsFullWidth(state)) InterlockedExchange(&g_newPipelineSeen, 1);
    if (eye == 0) ++stat.eye0; else if (eye == 1) ++stat.eye1;
    stat.orderSum += order;
    ++stat.count;
    if (stat.lastPresent != present)
    {
        stat.lastPresent = present;
        ++stat.framesSeen;
    }
    stat.rtWidth = state.rtWidth;
    if (elements > stat.maxElements) stat.maxElements = elements;
    if (stat.vsHash == 0) stat.vsHash = VsHashOf(state.pipeline);
    if (state.rtResource != 0) stat.rtResource = state.rtResource;
    stat.fullWidth = stat.fullWidth || IsSbsFullWidth(state);
}

bool HandleDraw(command_list *cmd, bool indexed, uint32_t a, uint32_t b, uint32_t c,
                int32_t d, uint32_t e)
{
    if (InterlockedCompareExchange(&g_enabled, 0, 0) == 0) return false;
    const CmdState state = StateCopy(cmd);
    const uint64_t order = g_frameDrawIndex.fetch_add(1, std::memory_order_relaxed);
    uint64_t previousMax = g_maxOrderSeen.load(std::memory_order_relaxed);
    while (order > previousMax &&
           !g_maxOrderSeen.compare_exchange_weak(previousMax, order, std::memory_order_relaxed))
    {
    }

    // Learning and acting now happen in the same frame: the set keeps growing as new contexts
    // (battle, menus, minigames) introduce their own UI pipelines.
    if (InterlockedCompareExchange(&g_learning, 0, 0) != 0)
    {
        uint32_t eye = 0xFFFFFFFFu;
        FastReadU32(g_imageBase + kEyeIndexRva, eye);
        LearnDraw(state, order, eye, InterlockedCompareExchange(&g_presents, 0, 0), a);
    }

    // Track where the scene ends: the last draw issued while the engine eye index is 1. Read the
    // eye only when it can matter -- a draw on the SBS-sized target -- so this costs nothing on
    // the thousands of shadow and auxiliary draws.
    bool overlayPhase = false;
    if (state.rtWidth >= kMinSbsWidth)
    {
        uint32_t eye = 0xFFFFFFFFu;
        FastReadU32(g_imageBase + kEyeIndexRva, eye);
        if (eye == 1)
        {
            uint64_t previous = g_frameMaxEye1.load(std::memory_order_relaxed);
            while (order > previous &&
                   !g_frameMaxEye1.compare_exchange_weak(previous, order, std::memory_order_relaxed))
            {
            }
        }
        else if (eye == 0)
        {
            // OVERLAY ROUTE: a small draw, into the HUD's own texture, after the scene finished.
            // The frame capture found nothing else there -- 479 draws, largest 60 elements -- so
            // this needs no shader or volume test and picks up the minimap and the menu elements
            // that every per-pipeline rule missed.
            const uint64_t sceneEnd = g_sceneEndOrder.load(std::memory_order_relaxed);
            const uint64_t primary = g_primaryUiTarget.load(std::memory_order_relaxed);
            overlayPhase = primary != 0 && state.rtResource == primary && sceneEnd != 0 &&
                           order > sceneEnd;
        }
    }

    const bool pipelineUi = IsUiPipeline(state.pipeline) && IsSbsFullWidth(state);
    if ((!pipelineUi && !overlayPhase) || a > kMaxUiDrawElements) return false;

    const LONG mode = InterlockedCompareExchange(&g_mode, 0, 0);
    if (mode == kNormal) return false;
    if (mode == kHide)
    {
        InterlockedIncrement(&g_hidden);
        return true;
    }
    if (indexed)
        return DuplicateAcrossHalves(cmd, state,
            [&] { cmd->draw_indexed(a, b, c, d, e); });
    return DuplicateAcrossHalves(cmd, state, [&] { cmd->draw(a, b, c, e); });
}

bool OnDraw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t firstVertex,
            uint32_t firstInstance)
{
    return HandleDraw(cmd, false, vertices, instances, firstVertex, 0, firstInstance);
}

bool OnDrawIndexed(command_list *cmd, uint32_t indices, uint32_t instances, uint32_t firstIndex,
                   int32_t vertexOffset, uint32_t firstInstance)
{
    return HandleDraw(cmd, true, indices, instances, firstIndex, vertexOffset, firstInstance);
}

void SelectUiPipelines()
{
    const uint64_t maxOrder = g_maxOrderSeen.load(std::memory_order_relaxed);

    struct Candidate
    {
        uint64_t pipeline = 0;
        LearnStat stat;
        double eye0Fraction = 0.0;
        uint64_t perFrame = 0;
        uint64_t meanOrder = 0;
        bool selected = false;
        bool uiLike = false;   // eye-0-only and full-width: a UI-shaped draw, whatever its volume
        const char *reject = "";
    };

    std::vector<Candidate> candidates;
    size_t newShaders = 0;
    uint64_t bestAnchorCount = 0;
    {
        std::lock_guard learn(g_learnLock);
        std::unique_lock ui(g_uiLock);
        std::unique_lock shaders(g_shaderLock);

        // Pass 1 -- harvest UI SHADERS. A pipeline is a UI-shader source if it draws only on the
        // full-width SBS target, only during the eye-0 phase (eye0Frac >= threshold), has been seen
        // across enough frames, and has NEVER issued a large mesh. Its vertex shader is a UI shader.
        // This is the seed: it needs a pipeline whose UI-ness is unambiguous, which the main HUD
        // always satisfies. Shaders are sticky -- a UI shader stays a UI shader.
        for (const auto &[pipeline, stat] : g_learnStats)
        {
            if (stat.count == 0 || stat.vsHash == 0) continue;
            const uint64_t eyeSamples = stat.eye0 + stat.eye1;
            const double eye0Fraction = eyeSamples != 0
                ? static_cast<double>(stat.eye0) / static_cast<double>(eyeSamples) : 0.0;
            // The seed MUST require high volume. v36's first build omitted it and a full-screen
            // composite pass -- a 6-element quad, drawn once, eye-0-only, on the SBS target --
            // qualified as a UI-shader source. Its shader was then marked UI and duplicated,
            // tiling the whole screen. Only an unmistakable HUD pipeline may seed: hundreds of
            // small primitives per frame is something no composite or post pass ever does.
            const uint64_t perFrame = stat.framesSeen != 0 ? stat.count / stat.framesSeen : 0;
            const bool source = stat.fullWidth && stat.rtWidth >= kMinSbsWidth &&
                                stat.framesSeen >= kMinFramesSeen &&
                                eye0Fraction >= kMinEye0Fraction &&
                                stat.maxElements <= kMaxUiPipelineElements &&
                                perFrame >= kMinDrawsPerFrame;
            if (source)
            {
                if (g_uiShaders.insert(stat.vsHash).second) ++newShaders;
                // The single BUSIEST seeded pipeline names the UI texture. v40 accepted every
                // seeded pipeline's target -- five of them, including incidental ones -- and a
                // composite came in through one of the others and tiled the screen.
                if (stat.count > bestAnchorCount && stat.rtResource != 0)
                {
                    bestAnchorCount = stat.count;
                    g_primaryUiTarget.store(stat.rtResource, std::memory_order_relaxed);
                }
            }
        }

        // Pass 2 -- a pipeline is UI if its vertex shader is a UI shader and it has never drawn a
        // large mesh. This catches the minimap and battle UI (same shader, lower volume) and
        // excludes 3D meshes (different shader) no matter how they are batched. Sticky.
        for (const auto &[pipeline, stat] : g_learnStats)
        {
            if (stat.count == 0) continue;
            Candidate candidate;
            candidate.pipeline = pipeline;
            candidate.stat = stat;
            const uint64_t eyeSamples = stat.eye0 + stat.eye1;
            candidate.eye0Fraction = eyeSamples != 0
                ? static_cast<double>(stat.eye0) / static_cast<double>(eyeSamples) : 0.0;
            candidate.perFrame = stat.framesSeen != 0 ? stat.count / stat.framesSeen : 0;
            candidate.meanOrder = stat.orderSum / stat.count;
            candidate.uiLike = stat.vsHash != 0 && g_uiShaders.count(stat.vsHash) != 0;

            const bool alreadySelected = g_uiPipelines.find(pipeline) != g_uiPipelines.end();
            if (alreadySelected) candidate.selected = true;
            else if (stat.vsHash == 0) candidate.reject = "noShaderHash";
            else if (g_uiShaders.count(stat.vsHash) == 0) candidate.reject = "notAUiShader";
            else if (stat.maxElements > kMaxUiPipelineElements) candidate.reject = "drawsMeshes_notUi";
            else if (stat.rtWidth < kMinSbsWidth) candidate.reject = "notSbsTarget";
            else
            {
                candidate.selected = true;
                g_uiPipelines.insert(pipeline);
            }
            candidates.push_back(candidate);
        }

        // Hard cap. Whatever the criteria believe, a set this large means they are wrong, and
        // tiling the screen is the worst possible response to being wrong. Keep the busiest.
        if (g_uiPipelines.size() > kMaxSelectedPipelines)
        {
            std::vector<std::pair<uint64_t, uint64_t>> ranked; // {draw count, pipeline}
            for (const Candidate &candidate : candidates)
                if (g_uiPipelines.find(candidate.pipeline) != g_uiPipelines.end())
                    ranked.push_back({candidate.stat.count, candidate.pipeline});
            std::sort(ranked.begin(), ranked.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
            g_uiPipelines.clear();
            for (size_t i = 0; i < ranked.size() && i < kMaxSelectedPipelines; ++i)
                g_uiPipelines.insert(ranked[i].second);
            for (Candidate &candidate : candidates)
                if (candidate.selected &&
                    g_uiPipelines.find(candidate.pipeline) == g_uiPipelines.end())
                {
                    candidate.selected = false;
                    candidate.reject = "trimmed_setTooLarge";
                }
            char warn[224] = {};
            sprintf_s(warn, "[GUARD] selection exceeded %zu pipelines; trimmed to the busiest %zu",
                      kMaxSelectedPipelines, g_uiPipelines.size());
            Log(warn);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.stat.count > b.stat.count;
    });
    size_t selectedCount = 0;
    for (const Candidate &candidate : candidates) if (candidate.selected) ++selectedCount;

    // Selection now runs every kReselectInterval presents, so only report when the set actually
    // changes -- otherwise the log fills with identical rows.
    size_t uiShaderCount = 0;
    { std::shared_lock shaders(g_shaderLock); uiShaderCount = g_uiShaders.size(); }

    static size_t lastSelectedCount = SIZE_MAX;
    static size_t lastShaderCount = SIZE_MAX;
    const bool changed = selectedCount != lastSelectedCount || uiShaderCount != lastShaderCount;
    lastSelectedCount = selectedCount;
    lastShaderCount = uiShaderCount;
    if (!changed && newShaders == 0)
    {
        InterlockedExchange(&g_active, 1);
        return;
    }

    char line[320] = {};
    sprintf_s(line,
              "[LEARNED] presents=%ld maxOrder=%llu candidates=%zu uiShaders=%zu selected=%zu uiTarget=0x%llX sceneEnd=%llu",
              InterlockedCompareExchange(&g_presents, 0, 0),
              static_cast<unsigned long long>(maxOrder), candidates.size(), uiShaderCount,
              selectedCount,
              static_cast<unsigned long long>(g_primaryUiTarget.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(g_sceneEndOrder.load(std::memory_order_relaxed)));
    Log(line);

    // v27 logged only the 18 busiest of 411 candidates, so battle UI -- which draws far less than
    // the field HUD -- never appeared and could not be diagnosed. Log every UI-SHAPED candidate
    // (eye-0-only, full-width) and every selected pipeline regardless of rank; everything else is
    // still capped so the log stays readable.
    size_t loggedBulk = 0;
    for (size_t index = 0; index < candidates.size(); ++index)
    {
        const Candidate &candidate = candidates[index];
        const bool alwaysLog = candidate.selected || candidate.uiLike;
        if (!alwaysLog)
        {
            if (loggedBulk >= 12) continue;
            ++loggedBulk;
        }
        char row[420] = {};
        sprintf_s(row,
                  "[CAND] rank=%zu pipeline=0x%llX vs=0x%llX %s%s draws=%llu perFrame=%llu eye0Frac=%.3f maxEl=%u meanOrder=%llu rtWidth=%u%s%s",
                  index, static_cast<unsigned long long>(candidate.pipeline),
                  static_cast<unsigned long long>(candidate.stat.vsHash),
                  candidate.selected ? "SELECTED" : "rejected",
                  candidate.uiLike ? "(uiShader)" : "",
                  static_cast<unsigned long long>(candidate.stat.count),
                  static_cast<unsigned long long>(candidate.perFrame),
                  candidate.eye0Fraction, candidate.stat.maxElements,
                  static_cast<unsigned long long>(candidate.meanOrder),
                  candidate.stat.rtWidth,
                  candidate.selected ? "" : " reason=", candidate.reject);
        Log(row);
    }
    if (selectedCount == 0)
        Log("[WARN] no UI pipelines identified; nothing will be duplicated (see [CAND] rows above)");
    InterlockedExchange(&g_active, 1);
}

void OnPresent(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
    g_sceneEndOrder.store(g_frameMaxEye1.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_frameMaxEye1.store(0, std::memory_order_relaxed);
    g_frameDrawIndex.store(0, std::memory_order_relaxed);
    if (InterlockedCompareExchange(&g_enabled, 0, 0) == 0) return;
    const LONG presents = InterlockedIncrement(&g_presents);

    // Learn densely up front so the HUD is caught quickly, then keep sampling one frame in four
    // forever so new contexts are still discovered.
    const bool learnThisFrame =
        presents <= kLearnPresents || (presents % kLearnSampleEvery) == 0;
    InterlockedExchange(&g_learning, learnThisFrame ? 1 : 0);

    // Re-evaluate as soon as an unfamiliar pipeline shows up, rather than on a fixed cadence.
    // Entering battle now costs a handful of frames instead of up to a second and a half. The
    // scheduled interval stays as a backstop, and a minimum gap keeps this from thrashing while
    // a busy scene streams in many new pipelines at once.
    const LONG sinceLastSelect = presents - InterlockedCompareExchange(&g_lastSelectPresent, 0, 0);
    const bool newPipeline = InterlockedCompareExchange(&g_newPipelineSeen, 0, 0) != 0;
    const bool dueForBackstop = presents > kLearnPresents &&
                                (presents % kReselectInterval) == 0;
    if (presents == kLearnPresents ||
        (presents > kLearnPresents && sinceLastSelect >= kReselectMinGap &&
         (newPipeline || dueForBackstop)))
    {
        InterlockedExchange(&g_newPipelineSeen, 0);
        InterlockedExchange(&g_lastSelectPresent, presents);
        SelectUiPipelines();
    }

    if (presents % 300 != 0) return;
    char line[320] = {};
    sprintf_s(line, "[FRAME] presents=%ld mode=%s duplicated=%ld hidden=%ld initializerHits=%ld",
              presents, kModeName[InterlockedCompareExchange(&g_mode, 0, 0) % kModeCount],
              InterlockedCompareExchange(&g_duplicated, 0, 0),
              InterlockedCompareExchange(&g_hidden, 0, 0),
              InterlockedCompareExchange(&g_initializerHits, 0, 0));
    Log(line);
}

DWORD WINAPI Worker(LPVOID)
{
    const DWORD helperTid = GetCurrentThreadId();
    uint32_t seen = 0, applied = 0;
    while (InterlockedCompareExchange(&g_enabled, 0, 0) != 0)
    {
        ApplyAllThreads(helperTid, true, seen, applied);
        Sleep(kRefreshMilliseconds);
    }
    ApplyAllThreads(helperTid, false, seen, applied);
    RestoreNativeState();
    InterlockedExchange(&g_workerLive, 0);
    return 0;
}

void OnEffects(effect_runtime *runtime, command_list *, resource_view, resource_view)
{
    if (runtime->is_key_pressed(VK_F11))
    {
        const LONG next = (InterlockedCompareExchange(&g_mode, 0, 0) + 1) % kModeCount;
        InterlockedExchange(&g_mode, next);
        char line[128] = {};
        sprintf_s(line, "[MODE] %s", kModeName[next]);
        Log(line);
    }
    if (runtime->is_key_pressed(VK_F10) &&
        InterlockedCompareExchange(&g_enabled, 0, 0) != 0)
    {
        const LONG next = InterlockedCompareExchange(&g_separated, 0, 0) == 0 ? 1 : 0;
        if (ApplyEyeMatrices(next != 0))
        {
            InterlockedExchange(&g_separated, next);
            Log(next ? "[EYES] separation ON" : "[EYES] separation OFF");
        }
    }
    if (!runtime->is_key_pressed(VK_F9)) return;
    if (InterlockedCompareExchange(&g_enabled, 0, 0) != 0)
    {
        Log("[IGNORED] already armed; exit the game normally when finished");
        return;
    }
    if (InterlockedCompareExchange(&g_workerLive, 1, 0) != 0) return;
    g_originalMatricesValid = ReadMem(g_imageBase + kEyeMatricesRva,
                                      g_originalMatrices, sizeof(g_originalMatrices));
    if (!g_originalMatricesValid || !ApplyEyeMatrices(false))
    {
        InterlockedExchange(&g_workerLive, 0);
        Log("[ERROR] eye matrix initialization failed");
        return;
    }
    {
        std::lock_guard learn(g_learnLock);
        g_learnStats.clear();
    }
    {
        std::unique_lock ui(g_uiLock);
        g_uiPipelines.clear();
    }
    {
        std::unique_lock shaders(g_shaderLock);
        g_uiShaders.clear();
        g_primaryUiTarget.store(0, std::memory_order_relaxed);
        g_sceneEndOrder.store(0, std::memory_order_relaxed);
        g_frameMaxEye1.store(0, std::memory_order_relaxed);
        // g_pipelineVs is NOT cleared: pipelines are created once at load, so their shader hashes
        // must survive a re-arm.
    }
    g_maxOrderSeen.store(0, std::memory_order_relaxed);
    InterlockedExchange(&g_separated, 0);
    InterlockedExchange(&g_initializerHits, 0);
    for (size_t index = 0; index < kGateCount; ++index)
        InterlockedExchange(&g_gateOpened[index], 0);
    InterlockedExchange(&g_crashCaptured, 0);
    InterlockedExchange(&g_presents, 0);
    InterlockedExchange(&g_duplicated, 0);
    InterlockedExchange(&g_newPipelineSeen, 0);
    InterlockedExchange(&g_lastSelectPresent, 0);
    InterlockedExchange(&g_hidden, 0);
    InterlockedExchange(&g_active, 0);
    InterlockedExchange(&g_learning, 1);
    InterlockedExchange(&g_enabled, 1);
    Log("[ON] stereo + gates armed; learning the HUD continuously; DUPLICATE mode after 120 presents");
    HANDLE worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (worker != nullptr) CloseHandle(worker);
    else
    {
        InterlockedExchange(&g_enabled, 0);
        InterlockedExchange(&g_learning, 0);
        InterlockedExchange(&g_workerLive, 0);
        RestoreNativeState();
        Log("[ERROR] breakpoint worker creation failed");
    }
}
} // namespace

extern "C" __declspec(dllexport) const char *NAME = "LAD8 UI overlay v41";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "F9 arms native stereo with the v21 gates and draws the HUD into both halves; F11 cycles mode.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        if (!reshade::register_addon(module)) return FALSE;
        InitializeCriticalSection(&g_logLock);
        wchar_t local[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        CreateDirectoryW((std::wstring(local) + L"\\LAD8VR").c_str(), nullptr);
        swprintf_s(g_logPath, L"%s\\LAD8VR\\LAD8VR_UiOverlayV41.txt", local);
        swprintf_s(g_crashPath, L"%s\\LAD8VR\\LAD8VR_UiOverlayV41_Crash.txt", local);
        DeleteFileW(g_logPath);
        g_crashFile = CreateFileW(g_crashPath, GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        g_imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        g_veh = AddVectoredExceptionHandler(1, Handler);
        reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
        reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
        reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
        reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
        reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
        reshade::register_event<reshade::addon_event::bind_viewports>(OnBindViewports);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
        reshade::register_event<reshade::addon_event::begin_render_pass>(OnBeginRenderPass);
        reshade::register_event<reshade::addon_event::draw>(OnDraw);
        reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnEffects);
        Log(g_veh != nullptr
                ? "[BOOT] v41 ready; UI phase on the HUD texture; F9 arms stereo + HUD duplication, F11 cycles mode"
                : "[BOOT] ERROR: VEH registration failed");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        InterlockedExchange(&g_enabled, 0);
        RestoreNativeState();
        if (g_veh != nullptr) RemoveVectoredExceptionHandler(g_veh);
        reshade::unregister_addon(module);
        if (g_crashFile != INVALID_HANDLE_VALUE) CloseHandle(g_crashFile);
        DeleteCriticalSection(&g_logLock);
    }
    return TRUE;
}
