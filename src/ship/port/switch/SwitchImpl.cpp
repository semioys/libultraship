#ifdef __SWITCH__

#include "ship/port/switch/SwitchImpl.h"

#include <atomic>
#include <cerrno>
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

    printf("[switch] PaperBoat starting: romfs=%s nxlink=%s applet_type=%d\n", sRomfsMounted ? "ok" : "FAILED",
           sNxlinkSocket >= 0 ? "connected" : "off", (int)appletGetAppletType());
}

// Called by libnx after main() returns or exit() is called.
void userAppExit(void) {
    printf("[switch] PaperBoat exiting\n");
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

static std::atomic<int> sCpuBoostRefs { 0 };

void SetCpuBoost(bool enable) {
    if (enable) {
        if (sCpuBoostRefs.fetch_add(1) == 0) {
            Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
            SPDLOG_INFO("[switch] CPU boost on (rc=0x{:x})", rc);
        }
    } else {
        int prev = sCpuBoostRefs.fetch_sub(1);
        if (prev == 1) {
            Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
            SPDLOG_INFO("[switch] CPU boost off (rc=0x{:x})", rc);
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
    SPDLOG_INFO("[switch] memory {}: used {} MiB / total {} MiB", tag, used >> 20, total >> 20);
}

} // namespace Switch
} // namespace Ship

#endif
