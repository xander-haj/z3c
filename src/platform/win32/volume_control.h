// =================================================================================
// volume_control.h — Public API for Win32 per-application volume control
//
// Exposes three functions that allow the zelda3 game engine to read and adjust
// the application's volume level through the Windows Audio Session API (WASAPI).
// This enables volume changes that only affect zelda3 without altering the
// system-wide volume — the same behavior as the Windows Volume Mixer slider.
//
// On non-Win32 platforms, this header is not included; SDL2's audio API is used
// directly instead. The SYSTEM_VOLUME_MIXER_AVAILABLE flag lets the game engine
// conditionally compile volume mixer UI elements only when this API is present.
// =================================================================================

#ifndef ZELDA3_PLATFORM_WIN32_VOLUME_CONTROL_H_
#define ZELDA3_PLATFORM_WIN32_VOLUME_CONTROL_H_

#include <stdbool.h>

// Feature flag indicating that native OS volume mixer integration is available.
// When set to 1, the game can show a volume control in its UI that directly
// adjusts the per-application volume in the Windows Volume Mixer.
// Defaults to 1 on Win32; other platforms should define this as 0.
#ifndef SYSTEM_VOLUME_MIXER_AVAILABLE
#define SYSTEM_VOLUME_MIXER_AVAILABLE 1
#endif  // SYSTEM_VOLUME_MIXER_AVAILABLE

// Returns the current application volume as an integer 0-100, or 0 on failure
int GetApplicationVolume();
// Sets the application volume to the given percentage (0-100). Returns true on success.
bool SetApplicationVolume(int volume_level);
// Mutes (true) or unmutes (false) the application audio without changing volume level
bool SetApplicationMuted(bool muted);

#endif // ZELDA3_PLATFORM_WIN32_VOLUME_CONTROL_H_
