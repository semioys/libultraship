#ifdef __SWITCH__

#include "ship/port/switch/SwitchImpl.h"

#include <atomic>
#include <malloc.h>
#include <cerrno>
#include <cstdarg>
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
