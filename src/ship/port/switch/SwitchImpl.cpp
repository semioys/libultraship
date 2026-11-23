#ifdef __SWITCH__

#include "ship/port/switch/SwitchImpl.h"

#include <atomic>
#include <malloc.h>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>

#include <set>
#include <string>
#include <switch.h>
#include <SDL2/SDL.h>

#include "spdlog/spdlog.h"

namespace {

int sNxlinkSocket = -1;
u64 sHapticStopTick = 0; // armGetSystemTick() deadline for the current HD Rumble pulse.
bool sRomfsMounted = false;
bool sSocketsInitialized = false;

AppletHookCookie sAppletHookCookie;
std::atomic<bool> sInFocus { true };
std::atomic<uint32_t> sResumeCount { 0 };

// Runs from appletMainLoop(), which SDL calls while pumping events.
void AppletHookCallback(AppletHookType hook, void* param) {
    (void)param;
    switch (hook) {
        case AppletHookType_OnFocusState: {
            const bool focused = appletGetFocusState() == AppletFocusState_InFocus;
            sInFocus = focused;
            Ship::Switch::Diag("focus %s", focused ? "gained" : "lost");
            break;
        }
        case AppletHookType_OnResume:
            sResumeCount++;
            Ship::Switch::Diag("resumed from sleep");
            break;
        case AppletHookType_OnOperationMode:
            Ship::Switch::Diag("operation mode -> %s", Ship::Switch::IsDocked() ? "docked" : "handheld");
            break;
        case AppletHookType_OnExitRequest:
            Ship::Switch::Diag("exit requested by the system");
            break;
        default:
            break;
    }
}

// mkdir -p for sdmc: paths.
void MakeDirectories(const char* path) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char* p = buf; *p != '\0'; p++) {
        // Skip the "sdmc:/" device prefix.
        if (*p == '/' && p != buf && *(p - 1) != ':') {
            *p = '\0';
            mkdir(buf, 0777);
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}

} // namespace

extern "C" {

// Called by libnx before main(). Keep it to things that cannot fail fatally:
// the game must still get to main() to show an error on screen.
void userAppInit(void) {
    // stdout/stderr -> PC terminal, only when launched with `nxlink -s`.
    if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault())) {
        sSocketsInitialized = true;
        sNxlinkSocket = nxlinkStdio();
        if (sNxlinkSocket < 0) {
            socketExit();
            sSocketsInitialized = false;
        }
    }
    // A socket-backed stdout is fully buffered by default; a crash would eat
    // the last lines, which are the ones we need.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    sRomfsMounted = R_SUCCEEDED(romfsInit());
    Ship::Switch::InstallCrashHandler();

    // Get told about HOME/sleep (to pause audio) while still being suspended
    // by the system as usual; allow the Capture button to take screenshots.
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
    appletHook(&sAppletHookCookie, AppletHookCallback, nullptr);
    appletSetScreenShotPermission(AppletScreenShotPermission_Enable);

    // A single Joy-Con is held sideways (one-player "mini controller").
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);

    // Handheld touchscreen drives the ImGui menu as a touch "mouse".
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");

    MakeDirectories(Ship::Switch::kAppDirectoryPath);
    // Relative paths (./mods, logs, ...) resolve against the app directory.
    chdir(Ship::Switch::kAppDirectoryPath);

    Ship::Switch::Diag("PaperBoat starting: romfs=%s nxlink=%s applet_type=%d", sRomfsMounted ? "ok" : "FAILED",
                       sNxlinkSocket >= 0 ? "connected" : "off", (int)appletGetAppletType());
}

// Called by libnx after main() returns or exit() is called.
void userAppExit(void) {
    Ship::Switch::Diag("PaperBoat exiting");
    appletUnhook(&sAppletHookCookie);
    fflush(stdout);
    fflush(stderr);

    if (sRomfsMounted) {
        romfsExit();
        sRomfsMounted = false;
    }
    if (sNxlinkSocket >= 0) {
        close(sNxlinkSocket);
        sNxlinkSocket = -1;
    }
    if (sSocketsInitialized) {
        socketExit();
        sSocketsInitialized = false;
    }
}

} // extern "C"

namespace Ship {
namespace Switch {

bool IsAddressInLoadedModule(uintptr_t addr) {
    MemoryInfo info;
    u32 pageInfo;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, addr))) {
        return false;
    }
    switch (info.type & MemState_Type) {
        case MemType_CodeStatic:
        case MemType_CodeMutable:
        case MemType_ModuleCodeStatic:
        case MemType_ModuleCodeMutable:
            return true;
        default:
            return false;
    }
}

bool IsNxlinkConnected() {
    return sNxlinkSocket >= 0;
}

void PinCurrentThread(int core, int priority) {
    const Handle self = threadGetCurHandle();
    svcSetThreadCoreMask(self, core, 1U << core);
    svcSetThreadPriority(self, priority);
}

bool IsAppletMode() {
    const AppletType type = appletGetAppletType();
    return type != AppletType_Application && type != AppletType_SystemApplication;
}

bool IsInFocus() {
    return sInFocus;
}

uint32_t GetResumeCount() {
    return sResumeCount;
}

void Diag(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // Straight to fd 1 (the nxlink socket when connected). newlib keeps a
    // separate stdout FILE per thread, and output from worker threads'
    // FILEs did not reach nxlink in testing.
    char line[540];
    int len = snprintf(line, sizeof(line), "[switch] %s\n", buf);
    if (len > 0) {
        if (len > (int)sizeof(line) - 1) {
            len = sizeof(line) - 1;
        }
        if (write(STDOUT_FILENO, line, len) < 0) {
            SPDLOG_WARN("[switch] {}", buf);
        }
    }
    SPDLOG_INFO("[switch] {}", buf);
}

bool IsDocked() {
    return appletGetOperationMode() == AppletOperationMode_Console;
}

void GetDisplaySize(int* width, int* height) {
    if (IsDocked()) {
        *width = 1920;
        *height = 1080;
    } else {
        *width = 1280;
        *height = 720;
    }
}

// ---- Graphics recovery -----------------------------------------------------
static std::atomic<bool> sGraphicsResetRequested { false };

void RequestGraphicsReset(const char* reason) {
    if (!sGraphicsResetRequested.exchange(true)) {
        Diag("graphics reset requested: %s", reason != nullptr ? reason : "?");
    }
}

bool ConsumeGraphicsReset() {
    return sGraphicsResetRequested.exchange(false);
}

void GetHeapStats(uint64_t* inUse, uint64_t* freeBytes) {
    const struct mallinfo info = mallinfo();
    *inUse = info.uordblks;
    *freeBytes = info.fordblks;
}

// ---- Adaptive frame rate ----------------------------------------------------
// While the game targets 60 FPS, watch a sliding window of frame times. When
// too many frames miss the 60 FPS budget, hold 30 FPS (steady instead of
// stuttering between 40 and 60) and try 60 again later, waiting longer each
// time a heavy scene keeps failing.
static bool sHighTarget = false;
static bool sHolding = false;
static u64 sHoldUntilNs = 0;
static u64 sHoldLengthNs = 4000000000ULL;
static u64 sLastReleaseNs = 0;
// Last second of frame times (microseconds), only while aiming for 60.
static uint32_t sFrameRing[60];
static int sFrameRingPos = 0;
static int sFrameRingFill = 0;

void SetHighFrameRateTarget(bool high) {
    sHighTarget = high;
    if (!high) {
        sHolding = false;
    }
}

bool ShouldHoldThirtyFps() {
    return sHolding;
}

static void TrackAdaptiveFps(u64 now, u64 frameNs) {
    if (!sHighTarget) {
        return;
    }
    if (sHolding) {
        if (now >= sHoldUntilNs) {
            sHolding = false;
            sLastReleaseNs = now;
            sFrameRingPos = 0;
            sFrameRingFill = 0;
            Diag("adaptive fps: trying 60 again");
        }
        return;
    }
    const int ringSize = (int)(sizeof(sFrameRing) / sizeof(sFrameRing[0]));
    sFrameRing[sFrameRingPos] = (uint32_t)(frameNs / 1000);
    sFrameRingPos = (sFrameRingPos + 1) % ringSize;
    if (sFrameRingFill < ringSize) {
        sFrameRingFill++;
        return; // need a full second of history
    }
    // Only a sustained drop counts: the last second averaged below 52 FPS, or
    // at least 10 frames took longer than 24 ms (a 1.5x miss). Isolated spikes
    // and frames that just miss vsync by a few ms keep 60.
    uint64_t totalUs = 0;
    int veryslow = 0;
    for (int i = 0; i < ringSize; i++) {
        totalUs += sFrameRing[i];
        veryslow += sFrameRing[i] > 24000 ? 1 : 0;
    }
    const double avgFps = ringSize * 1e6 / (double)totalUs;
    if (avgFps < 52.0 || veryslow >= 10) {
        const int sSlowCount = veryslow;
        // Failing again right after a release: back off longer (up to 60 s).
        if (sLastReleaseNs != 0 && now - sLastReleaseNs < 3000000000ULL) {
            sHoldLengthNs = sHoldLengthNs * 2 > 60000000000ULL ? 60000000000ULL : sHoldLengthNs * 2;
        } else {
            sHoldLengthNs = 4000000000ULL;
        }
        sHolding = true;
        sHoldUntilNs = now + sHoldLengthNs;
        Diag("adaptive fps: holding 30 FPS for %.0f s (last second %.1f fps, %d frames over 24 ms)",
             sHoldLengthNs / 1e9, avgFps, sSlowCount);
    }
}

// ---- Spike profiler -----------------------------------------------------------
static uint64_t sCostNs[(int)FrameCost::Count];
static uint32_t sCostCount[(int)FrameCost::Count];

void AddFrameCost(FrameCost kind, uint64_t ns) {
    sCostNs[(int)kind] += ns;
    sCostCount[(int)kind]++;
}

ScopedFrameCost::ScopedFrameCost(FrameCost kind) : mKind(kind), mStart(armTicksToNs(armGetSystemTick())) {
}

ScopedFrameCost::~ScopedFrameCost() {
    AddFrameCost(mKind, armTicksToNs(armGetSystemTick()) - mStart);
}

// Called with the length of a frame that took far longer than the target:
// prints what the frame spent its time on, then starts the next frame clean.
static void ReportFrameCosts(u64 now, u64 frameNs) {
    // A spike is a frame much longer than the current target: 30 ms while
    // aiming for 60 FPS, 60 ms at 30 FPS (where every frame is ~33 ms).
    const u64 threshold = (sHighTarget && !sHolding) ? 30000000ULL : 60000000ULL;
    if (frameNs > threshold) {
        // At most two reports per second so a heavy stretch can't flood the log.
        static u64 sWindowStartNs = 0;
        static int sInWindow = 0;
        if (now - sWindowStartNs > 1000000000ULL) {
            sWindowStartNs = now;
            sInWindow = 0;
        }
        if (sInWindow < 2) {
            sInWindow++;
            const auto ms = [](FrameCost k) { return sCostNs[(int)k] / 1e6; };
            const double known = ms(FrameCost::Render) + ms(FrameCost::Shader) + ms(FrameCost::Texture) +
                                 ms(FrameCost::Resource);
            Diag("spike %.1f ms: render %.1f ms (textures %u in %.1f ms, shaders %u in %.1f ms), "
                 "resource loads %u waited %.1f ms, other %.1f ms",
                 frameNs / 1e6, ms(FrameCost::Render), sCostCount[(int)FrameCost::Texture], ms(FrameCost::Texture),
                 sCostCount[(int)FrameCost::Shader], ms(FrameCost::Shader), sCostCount[(int)FrameCost::Resource],
                 ms(FrameCost::Resource), frameNs / 1e6 - known > 0 ? frameNs / 1e6 - known : 0.0);
        }
    }
    memset(sCostNs, 0, sizeof(sCostNs));
    memset(sCostCount, 0, sizeof(sCostCount));
}

void OnFrame() {
    static u64 sWindowStart = 0;
    static u64 sLastFrame = 0;
    static u32 sFrames = 0;
    static u32 sHitches = 0;
    static u64 sWorstNs = 0;

    if (sHapticStopTick != 0 && armGetSystemTick() >= sHapticStopTick) {
        StopHaptic();
    }

    const u64 now = armTicksToNs(armGetSystemTick());
    if (sLastFrame != 0) {
        const u64 frameNs = now - sLastFrame;
        TrackAdaptiveFps(now, frameNs);
        ReportFrameCosts(now, frameNs);
        sWorstNs = frameNs > sWorstNs ? frameNs : sWorstNs;
        if (frameNs > 50000000ULL) {
            sHitches++;
        }
        sFrames++;
    } else {
        sWindowStart = now;
    }
    sLastFrame = now;

    const u64 windowNs = now - sWindowStart;
    if (windowNs >= 5000000000ULL && sFrames > 0) {
        Diag("frames: %.1f fps avg, worst %.1f ms, %u hitches >50ms (%s)", sFrames * 1e9 / (double)windowNs,
             sWorstNs / 1e6, (unsigned)sHitches, IsDocked() ? "docked" : "handheld");
        sWindowStart = now;
        sFrames = 0;
        sHitches = 0;
        sWorstNs = 0;
    }
}

// Swaps the values of two "name:binding" fields in an SDL mapping string.
static void SwapMappingFields(std::string& mapping, const char* first, const char* second) {
    auto find = [&](const char* key) -> std::pair<size_t, size_t> {
        const std::string needle = std::string(",") + key + ":";
        size_t start = mapping.find(needle);
        if (start == std::string::npos) {
            return { std::string::npos, 0 };
        }
        start += needle.size();
        size_t end = mapping.find(',', start);
        if (end == std::string::npos) {
            end = mapping.size();
        }
        return { start, end - start };
    };
    auto [firstPos, firstLen] = find(first);
    auto [secondPos, secondLen] = find(second);
    if (firstPos == std::string::npos || secondPos == std::string::npos) {
        return;
    }
    const std::string firstValue = mapping.substr(firstPos, firstLen);
    const std::string secondValue = mapping.substr(secondPos, secondLen);
    // Replace the later field first so the earlier offset stays valid.
    if (firstPos > secondPos) {
        mapping.replace(firstPos, firstLen, secondValue);
        mapping.replace(secondPos, secondLen, firstValue);
    } else {
        mapping.replace(secondPos, secondLen, firstValue);
        mapping.replace(firstPos, firstLen, secondValue);
    }
}

void ApplyNintendoButtonLayout() {
    static std::set<std::string> sRemapped;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        char guidStr[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guidStr, sizeof(guidStr));
        if (sRemapped.count(guidStr) != 0) {
            continue;
        }
        char* raw = SDL_GameControllerMappingForGUID(SDL_JoystickGetDeviceGUID(i));
        if (raw == nullptr) {
            continue;
        }
        std::string mapping = raw;
        SDL_free(raw);
        SwapMappingFields(mapping, "a", "b");
        SwapMappingFields(mapping, "x", "y");
        if (SDL_GameControllerAddMapping(mapping.c_str()) >= 0) {
            sRemapped.insert(guidStr);
            Diag("Nintendo button layout applied to %s (%s)", guidStr, SDL_JoystickNameForIndex(i));
        }
    }
}

// ---- HD Rumble -------------------------------------------------------------
namespace {
struct VibrationTarget {
    HidNpadIdType id;
    HidNpadStyleTag style;
    s32 count;
    HidVibrationDeviceHandle handles[2];
    bool ready;
};

VibrationTarget sVibrationTargets[] = {
    { HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 2, {}, false },
    { HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey, 2, {}, false },
    { HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, 2, {}, false },
    { HidNpadIdType_No1, HidNpadStyleTag_NpadJoyLeft, 1, {}, false },
    { HidNpadIdType_No1, HidNpadStyleTag_NpadJoyRight, 1, {}, false },
};
bool sVibrationInitialized = false;

void InitVibration() {
    if (sVibrationInitialized) {
        return;
    }
    sVibrationInitialized = true;
    for (auto& target : sVibrationTargets) {
        target.ready =
            R_SUCCEEDED(hidInitializeVibrationDevices(target.handles, target.count, target.id, target.style));
    }
}

void SendVibration(const HidVibrationValue& value) {
    InitVibration();
    HidVibrationValue values[2] = { value, value };
    for (auto& target : sVibrationTargets) {
        if (!target.ready || (hidGetNpadStyleSet(target.id) & target.style) == 0) {
            continue;
        }
        hidSendVibrationValues(target.handles, values, target.count);
    }
}
} // namespace

void PlayHaptic(float ampLow, float freqLow, float ampHigh, float freqHigh, uint32_t durationMs) {
    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const HidVibrationValue value = { clamp01(ampLow), freqLow, clamp01(ampHigh), freqHigh };
    SendVibration(value);
    sHapticStopTick = armGetSystemTick() + armNsToTicks((u64)durationMs * 1000000ULL);
}

void StopHaptic() {
    sHapticStopTick = 0;
    const HidVibrationValue off = { 0.0f, 160.0f, 0.0f, 320.0f };
    SendVibration(off);
}

void ShowControllerApplet() {
    HidLaControllerSupportArg arg;
    hidLaCreateControllerSupportArg(&arg);
    arg.hdr.player_count_min = 1;
    arg.hdr.player_count_max = 1;
    arg.hdr.enable_single_mode = 1; // Allows handheld, a Joy-Con pair, or one sideways Joy-Con.
    HidLaControllerSupportResultInfo info;
    const Result rc = hidLaShowControllerSupport(&info, &arg);
    Diag("controller applet closed (rc=0x%x, players=%d)", rc, (int)info.player_count);
}

static std::atomic<int> sCpuBoostRefs { 0 };

void SetCpuBoost(bool enable) {
    if (enable) {
        if (sCpuBoostRefs.fetch_add(1) == 0) {
            Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
            Diag("CPU boost on (rc=0x%x)", rc);
        }
    } else {
        int prev = sCpuBoostRefs.fetch_sub(1);
        if (prev == 1) {
            Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
            Diag("CPU boost off (rc=0x%x)", rc);
        } else if (prev <= 0) {
            sCpuBoostRefs = 0;
        }
    }
}

void LogMemoryUsage(const char* tag) {
    // libnx hands (almost) all process memory to the heap up front, so the
    // kernel's "used" figure is always ~total. mallinfo() shows what is
    // actually allocated inside that heap.
    u64 total = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    const struct mallinfo info = mallinfo();
    Diag("memory %s: heap in use %lu MiB, heap arena %lu MiB, process total %lu MiB", tag,
         (unsigned long)(info.uordblks >> 20), (unsigned long)(info.arena >> 20), (unsigned long)(total >> 20));
}

} // namespace Switch
} // namespace Ship

#endif
