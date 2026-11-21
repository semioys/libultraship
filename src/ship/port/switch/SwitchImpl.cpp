#ifdef __SWITCH__

#include "ship/port/switch/SwitchImpl.h"

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "spdlog/spdlog.h"

namespace {

int sNxlinkSocket = -1;
bool sRomfsMounted = false;
bool sSocketsInitialized = false;

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

    MakeDirectories(Ship::Switch::kAppDirectoryPath);
    // Relative paths (./mods, logs, ...) resolve against the app directory.
    chdir(Ship::Switch::kAppDirectoryPath);

    Ship::Switch::Diag("PaperBoat starting: romfs=%s nxlink=%s applet_type=%d", sRomfsMounted ? "ok" : "FAILED",
                       sNxlinkSocket >= 0 ? "connected" : "off", (int)appletGetAppletType());
}

// Called by libnx after main() returns or exit() is called.
void userAppExit(void) {
    Ship::Switch::Diag("PaperBoat exiting");
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
    u64 total = 0;
    u64 used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    Diag("memory %s: used %lu MiB / total %lu MiB", tag, (unsigned long)(used >> 20), (unsigned long)(total >> 20));
}

} // namespace Switch
} // namespace Ship

#endif
