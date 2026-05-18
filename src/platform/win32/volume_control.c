// =================================================================================
// volume_control.c — Win32 per-application volume control via WASAPI COM interfaces
//
// Provides functions to get/set the application's audio volume and mute state on
// Windows, independent of the system-wide volume. This uses the Windows Audio
// Session API (WASAPI) which is available on Windows Vista and later.
//
// The implementation acquires a COM ISimpleAudioVolume interface for the current
// audio session by walking the device enumeration chain:
//   MMDeviceEnumerator -> default audio endpoint -> AudioSessionManager ->
//   SimpleAudioVolume
//
// Each public function creates a fresh ISimpleAudioVolume instance, performs its
// operation, and releases it immediately. This avoids holding COM references across
// frames, which could cause issues if the audio device changes during gameplay.
//
// This file is only compiled on Win32 builds. Other platforms handle volume through
// SDL2's audio API directly.
// =================================================================================

#include "volume_control.h"

// COBJMACROS enables C-style macro wrappers for COM interface method calls
// (e.g., IMMDeviceEnumerator_GetDefaultAudioEndpoint instead of vtable->method)
#define COBJMACROS
// CINTERFACE tells the Windows headers to generate C-compatible COM struct layouts
// instead of C++ class definitions with virtual function tables
#define CINTERFACE

// Prevents redefinition errors for Interlocked* functions when compiling as C
#define MICROSOFT_WINDOWS_WINBASE_H_DEFINE_INTERLOCKED_CPLUSPLUS_OVERLOADS 0

// Windows audio API headers
#include <initguid.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

// Forward declaration — defined at the bottom of this file
static ISimpleAudioVolume *GetSimpleAudioVolume();

// COM interface and class GUIDs required to instantiate the WASAPI device chain.
// These are Microsoft-defined identifiers that never change across Windows versions.
// IMMDeviceEnumerator: interface for enumerating audio endpoints (speakers, headphones)
DEFINE_GUID(IID_IMMDeviceEnumerator, 0XA95664D2, 0X9614, 0X4F35, 0XA7, 0X46, 0XDE, 0X8D, 0XB6, 0X36, 0X17, 0XE6);
// CLSID_MMDeviceEnumerator: COM class that implements IMMDeviceEnumerator
DEFINE_GUID(CLSID_MMDeviceEnumerator, 0XBCDE0395, 0XE52F, 0X467C, 0X8E, 0X3D, 0XC4, 0X57, 0X92, 0X91, 0X69, 0X2E);
// IAudioSessionManager: interface to access per-session volume/mute controls
DEFINE_GUID(IID_IAudioSessionManager, 0X77AA99A0, 0X1BD6, 0X484F, 0X8B, 0XC7, 0X2C, 0X65, 0X4C, 0X9A, 0X9B, 0X6F);

// Ensures COM is initialized exactly once per process lifetime.
// CoInitialize must be called before any COM interface can be used.
// Uses single-threaded apartment (STA) mode since zelda3 audio runs on one thread.
static void InitializeCom() {
  static bool com_initialized;
  if (!com_initialized)
    com_initialized = SUCCEEDED(CoInitialize(NULL));
}

// Retrieves the current application volume as an integer percentage (0-100).
// Returns 0 if the audio session cannot be accessed (e.g., no audio device present).
// The WASAPI volume is a float 0.0-1.0; this converts to integer percentage for
// compatibility with the game's volume slider which operates in whole-number steps.
int GetApplicationVolume() {
  ISimpleAudioVolume *simple_audio_volume = GetSimpleAudioVolume();
  if (!simple_audio_volume)
    return false;
  float volume_level = -1;
  HRESULT result = ISimpleAudioVolume_GetMasterVolume(simple_audio_volume, &volume_level);
  ISimpleAudioVolume_Release(simple_audio_volume);
  // Scale from 0.0-1.0 float to 0-100 integer
  return (int)(volume_level * 100);
}

// Sets the application volume to a percentage (0-100).
// Returns true if the volume was successfully applied, false on failure.
// The NULL GUID parameter means "do not associate this change with a specific event
// context" — Windows will not fire a volume-change notification back to us.
bool SetApplicationVolume(int volume_level) {
  ISimpleAudioVolume *simple_audio_volume = GetSimpleAudioVolume();
  if (!simple_audio_volume)
    return false;
  // Convert integer percentage to 0.0-1.0 float range expected by WASAPI
  HRESULT result = ISimpleAudioVolume_SetMasterVolume(simple_audio_volume, (float)(volume_level / 100.0), NULL);
  ISimpleAudioVolume_Release(simple_audio_volume);
  return SUCCEEDED(result);
}

// Mutes or unmutes the application's audio session without changing the volume level.
// The mute state is independent of volume — unmuting restores the previous volume.
// Returns true on success, false if the audio session could not be accessed.
bool SetApplicationMuted(bool muted) {
  ISimpleAudioVolume *simple_audio_volume = GetSimpleAudioVolume();
  if (!simple_audio_volume)
    return false;
  HRESULT result = ISimpleAudioVolume_SetMute(simple_audio_volume, muted, NULL);
  ISimpleAudioVolume_Release(simple_audio_volume);
  return SUCCEEDED(result);
}

// Walks the WASAPI COM object chain to obtain an ISimpleAudioVolume interface
// for this application's audio session. The chain is:
//   1. Create MMDeviceEnumerator (entry point to the audio device graph)
//   2. Get the default audio render endpoint (speakers/headphones)
//   3. Activate an AudioSessionManager on that endpoint
//   4. Request the SimpleAudioVolume for the default session (GUID_NULL)
//
// Returns NULL if any step fails (e.g., no audio device, COM not initialized).
// The caller is responsible for releasing the returned interface.
// Intermediate COM objects (enumerator, device, session manager) are released
// before returning to avoid leaking COM references.
static ISimpleAudioVolume *GetSimpleAudioVolume() {
  HRESULT result;
  IMMDeviceEnumerator *device_enumerator = NULL;
  IMMDevice *device = NULL;
  IAudioSessionManager *audio_session_manager = NULL;
  ISimpleAudioVolume *simple_audio_volume = NULL;

  InitializeCom();

  // Step 1: Create the device enumerator — the root of the WASAPI device tree
  result = CoCreateInstance(&CLSID_MMDeviceEnumerator,
      NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, &device_enumerator);
  if (FAILED(result) || device_enumerator == NULL)
    goto done;

  // Step 2: Get the default audio output device (eRender = playback, eConsole = default role)
  result = IMMDeviceEnumerator_GetDefaultAudioEndpoint(device_enumerator, eRender, eConsole, &device);
  if (FAILED(result) || device == NULL)
    goto done;

  // Step 3: Activate the session manager on the device to access per-app volume
  result = IMMDevice_Activate(device, &IID_IAudioSessionManager, CLSCTX_INPROC_SERVER, NULL, &audio_session_manager);
  if (FAILED(result) || audio_session_manager == NULL)
    goto done;
  // Step 4: Get the volume interface for the default audio session (GUID_NULL).
  // The second parameter (0) means "do not cross-process" — only this app's session.
  result = IAudioSessionManager_GetSimpleAudioVolume(audio_session_manager, &GUID_NULL, 0, &simple_audio_volume);

// Cleanup: release all intermediate COM objects regardless of success/failure.
// Only simple_audio_volume is returned to the caller (may be NULL on failure).
done:
  if (device_enumerator) IMMDeviceEnumerator_Release(device_enumerator);
  if (device) IMMDevice_Release(device);
  if (audio_session_manager) IAudioSessionManager_Release(audio_session_manager);

  return simple_audio_volume;
}
