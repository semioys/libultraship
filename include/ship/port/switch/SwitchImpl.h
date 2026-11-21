#pragma once

#ifdef __SWITCH__

#include <cstdint>

// Nintendo Switch platform layer.
//
// This header deliberately does not include <switch.h>: libnx typedefs u64/s64
// as uint64_t/int64_t (unsigned long on aarch64), which conflicts with
// libultra's unsigned long long. Everything that needs libnx lives in
// SwitchImpl.cpp.
//
// Process-level setup (nxlink stdio, romfs, the SD card app directory) happens
// in libnx's userAppInit()/userAppExit() hooks, before main() and after exit().

namespace Ship {
namespace Switch {

// Links in the crash reporter (SwitchCrash.cpp) and records the code region.
void InstallCrashHandler();

// Read-only files packed into the NRO.
inline constexpr const char* kRomfsPath = "romfs:";
// User data on the SD card: config, saves, mods, logs, generated archives.
inline constexpr const char* kAppDirectoryPath = "sdmc:/switch/PaperBoat";

// True when addr points into code or static data of a loaded module (the NRO
// itself), the Horizon equivalent of dladdr() succeeding.
bool IsAddressInLoadedModule(uintptr_t addr);

// True when stdout/stderr are redirected to an nxlink host on the PC.
bool IsNxlinkConnected();

// True when launched as a library applet (e.g. from the Album) instead of
// title override: far less memory, the game may not fit.
bool IsAppletMode();

// Focus / sleep state from the applet hook (HOME menu, sleep, overlays).
bool IsInFocus();
// Incremented every time the console wakes from sleep; audio uses it to reopen
// its device, which does not survive a sleep cycle.
uint32_t GetResumeCount();

// Native output size for the current operation mode: 1920x1080 docked,
// 1280x720 handheld.
void GetDisplaySize(int* width, int* height);

// True while docked (AppletOperationMode_Console).
bool IsDocked();

// SDL's Switch joystick backend maps buttons by position (Xbox layout: the
// bottom face button is "A"). Remap every connected controller so SDL's A/B/X/Y
// follow the printed Nintendo labels (A right, B bottom, X top, Y left).
// Idempotent per controller GUID; call before a controller is opened.
void ApplyNintendoButtonLayout();

// HD Rumble: one pulse on every connected Switch controller (handheld,
// Pro Controller, Joy-Con pair or single Joy-Con). Amplitudes 0..1, stopped
// automatically after durationMs (checked in OnFrame()).
void PlayHaptic(float ampLow, float freqLow, float ampHigh, float freqHigh, uint32_t durationMs);
void StopHaptic();

// Opens the system controller applet (pair / reorder controllers, switch
// between Joy-Con pair and single sideways Joy-Con). Blocks until closed.
void ShowControllerApplet();

// Call once per presented frame. Every 5 s logs average FPS, the worst frame
// and the number of hitches (frames over 50 ms) through Diag().
void OnFrame();

// Stock CPU boost (appletSetCpuBoostMode FastLoad: CPU 1785 MHz, GPU at
// minimum) for loading screens. Not an overclock. Calls nest; boost stays on
// until the last one is released.
void SetCpuBoost(bool enable);

// Diagnostic line for hardware testing: always reaches stdout (nxlink) with a
// "[switch] " prefix, regardless of the spdlog level, and is mirrored to the
// log file.
void Diag(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Logs process memory usage (used / total) with a tag.
void LogMemoryUsage(const char* tag);

// RAII helper around SetCpuBoost.
class ScopedCpuBoost {
  public:
    ScopedCpuBoost() {
        SetCpuBoost(true);
    }
    ~ScopedCpuBoost() {
        SetCpuBoost(false);
    }
    ScopedCpuBoost(const ScopedCpuBoost&) = delete;
    ScopedCpuBoost& operator=(const ScopedCpuBoost&) = delete;
};

} // namespace Switch
} // namespace Ship

#endif
