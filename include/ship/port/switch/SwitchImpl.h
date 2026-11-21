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

// Read-only files packed into the NRO.
inline constexpr const char* kRomfsPath = "romfs:";
// User data on the SD card: config, saves, mods, logs, generated archives.
inline constexpr const char* kAppDirectoryPath = "sdmc:/switch/PaperBoat";

// True when addr points into code or static data of a loaded module (the NRO
// itself), the Horizon equivalent of dladdr() succeeding.
bool IsAddressInLoadedModule(uintptr_t addr);

// True when stdout/stderr are redirected to an nxlink host on the PC.
bool IsNxlinkConnected();

} // namespace Switch
} // namespace Ship

#endif
