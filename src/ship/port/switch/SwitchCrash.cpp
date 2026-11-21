#ifdef __SWITCH__

// Crash reporter for Nintendo Switch.
//
// libnx calls __libnx_exception_handler() on its own stack when a thread
// faults. We write a report to sdmc:/switch/PaperBoat/logs/crash-*.txt and to
// stdout (nxlink), then return so the system shows its usual error screen.
//
// Every code address is printed both raw and as an offset from the start of
// the NRO's code ("+0x..."), which maps directly onto the ELF:
//   aarch64-none-elf-addr2line -f -C -e Paperboat.elf 0x<offset>
//
// The handler avoids the heap and spdlog: the faulting thread may hold their
// locks or have corrupted them.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "ship/port/switch/SwitchImpl.h"

extern "C" {
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
}

namespace {

int sReportFd = -1;
u64 sCodeBase = 0;
u64 sCodeEnd = 0;

void Emit(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Emit(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (len <= 0) {
        return;
    }
    if (len > (int)sizeof(line) - 1) {
        len = sizeof(line) - 1;
    }
    write(STDOUT_FILENO, line, len);
    if (sReportFd >= 0) {
        write(sReportFd, line, len);
    }
}

const char* DescribeException(u32 desc) {
    switch (desc) {
        case ThreadExceptionDesc_InstructionAbort:
            return "Instruction abort";
        case ThreadExceptionDesc_MisalignedPC:
            return "Misaligned PC";
        case ThreadExceptionDesc_MisalignedSP:
            return "Misaligned SP";
        case ThreadExceptionDesc_SError:
            return "SError";
        case ThreadExceptionDesc_BadSVC:
            return "Bad SVC";
        case ThreadExceptionDesc_Trap:
            return "Trap (illegal instruction / abort())";
        case ThreadExceptionDesc_Other:
            return "Data abort or other";
        default:
            return "Unknown";
    }
}

// Code region of the NRO: the mapping that contains this very function.
void FindCodeRegion() {
    MemoryInfo info;
    u32 pageInfo;
    if (R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, (u64)&FindCodeRegion))) {
        sCodeBase = info.addr;
        sCodeEnd = info.addr + info.size;
    }
}

bool InCode(u64 addr) {
    return addr >= sCodeBase && addr < sCodeEnd;
}

bool IsReadable(u64 addr, size_t size) {
    MemoryInfo info;
    u32 pageInfo;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, addr))) {
        return false;
    }
    if ((info.type & MemState_Type) == MemType_Unmapped || (info.perm & Perm_R) == 0) {
        return false;
    }
    return addr + size <= info.addr + info.size;
}

void EmitAddress(const char* label, u64 addr) {
    if (InCode(addr)) {
        Emit("  %-4s 0x%016lx  (+0x%lx)\n", label, (unsigned long)addr, (unsigned long)(addr - sCodeBase));
    } else {
        Emit("  %-4s 0x%016lx\n", label, (unsigned long)addr);
    }
}

void OpenReportFile() {
    const std::string dir = std::string(Ship::Switch::kAppDirectoryPath) + "/logs";
    mkdir(dir.c_str(), 0777);
    char path[160];
    const time_t now = time(nullptr);
    struct tm tmNow;
    if (localtime_r(&now, &tmNow) != nullptr) {
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmNow);
        snprintf(path, sizeof(path), "%s/crash-%s.txt", dir.c_str(), stamp);
    } else {
        snprintf(path, sizeof(path), "%s/crash-%lu.txt", dir.c_str(), (unsigned long)armGetSystemTick());
    }
    sReportFd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (sReportFd >= 0) {
        Emit("[switch] crash report: %s\n", path);
    }
}

} // namespace

namespace Ship {
namespace Switch {
// Referenced from userAppInit() so the linker pulls this object (and with it
// the strong exception handler/stack definitions) out of the static library.
void InstallCrashHandler() {
    FindCodeRegion();
}
} // namespace Switch
} // namespace Ship

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    static bool sInHandler = false;
    if (sInHandler) {
        return; // Faulted while reporting: let the system take over.
    }
    sInHandler = true;

    FindCodeRegion();
    OpenReportFile();

    Emit("\n================ PaperBoat crashed ================\n");
    Emit("exception: %s (0x%x)\n", DescribeException(ctx->error_desc), ctx->error_desc);
    Emit("code region: 0x%016lx - 0x%016lx\n", (unsigned long)sCodeBase, (unsigned long)sCodeEnd);
    Emit("mode: %s, applet type %d\n", Ship::Switch::IsDocked() ? "docked" : "handheld", (int)appletGetAppletType());
    Emit("esr 0x%08x  afsr0 0x%08x  afsr1 0x%08x  pstate 0x%08x\n", ctx->esr, ctx->afsr0, ctx->afsr1, ctx->pstate);

    Emit("\nregisters:\n");
    EmitAddress("pc", ctx->pc.x);
    EmitAddress("lr", ctx->lr.x);
    EmitAddress("sp", ctx->sp.x);
    EmitAddress("fp", ctx->fp.x);
    EmitAddress("far", ctx->far.x);
    for (int i = 0; i < 29; i++) {
        char label[8];
        snprintf(label, sizeof(label), "x%d", i);
        EmitAddress(label, ctx->cpu_gprs[i].x);
    }

    // Frame-pointer chain. May be short if frames were built without fp.
    Emit("\nbacktrace (frame pointers):\n");
    EmitAddress("#0", ctx->pc.x);
    EmitAddress("#1", ctx->lr.x);
    u64 fp = ctx->fp.x;
    for (int depth = 2; depth < 40 && fp != 0 && (fp & 0xF) == 0 && IsReadable(fp, 16); depth++) {
        const u64 nextFp = ((const u64*)fp)[0];
        const u64 ret = ((const u64*)fp)[1];
        if (ret == 0) {
            break;
        }
        char label[8];
        snprintf(label, sizeof(label), "#%d", depth);
        EmitAddress(label, ret);
        if (nextFp <= fp) {
            break; // The stack grows down, so callers' frames are higher.
        }
        fp = nextFp;
    }

    // Heuristic: code addresses found on the stack (return addresses, mostly).
    Emit("\nstack scan (code addresses near sp):\n");
    const u64 sp = ctx->sp.x & ~7ULL;
    int found = 0;
    for (int i = 0; i < 1024 && found < 32; i++) {
        const u64 slot = sp + (u64)i * 8;
        if ((slot & 0xFFF) == 0 && !IsReadable(slot, 8)) {
            break;
        }
        if (i == 0 && !IsReadable(slot, 8)) {
            break;
        }
        const u64 value = *(const u64*)slot;
        if (InCode(value)) {
            Emit("  [sp+0x%03x] 0x%016lx  (+0x%lx)\n", i * 8, (unsigned long)value, (unsigned long)(value - sCodeBase));
            found++;
        }
    }

    Emit("\nResolve offsets with:\n  aarch64-none-elf-addr2line -f -C -e Paperboat.elf 0x<offset>\n");
    Emit("===================================================\n");

    if (sReportFd >= 0) {
        fsync(sReportFd);
        close(sReportFd);
        sReportFd = -1;
    }
    // Returning hands the exception back to the system (error report screen).
}

#endif
