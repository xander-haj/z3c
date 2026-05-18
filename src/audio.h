/*
 * audio.h - Audio playback and MSU-1 enhanced soundtrack interface
 *
 * Declares the public API for the zelda3 audio subsystem. The original SNES
 * game used the SPC700 coprocessor for all music and sound effects; this
 * reimplementation adds optional MSU-1 support, which replaces the SPC700
 * chiptune tracks with CD-quality PCM audio streamed from external files.
 *
 * The functions here serve two purposes:
 *   1. MSU-1 track control -- query, play, and enable/disable MSU-1 tracks.
 *   2. Audio rendering -- mix the final audio output (SPC700 or MSU-1) into
 *      a platform-provided sample buffer each frame, and manage music state
 *      around save/load and reset operations.
 *
 * Concurrency note: functions suffixed with _Locked are called while the APU
 * mutex is already held; they must not re-acquire it.
 */
#ifndef ZELDA3_AUDIO_H_
#define ZELDA3_AUDIO_H_

#include "types.h"

/* ---------------------------------------------------------------------------
 * MSU-1 track query and control
 *
 * MSU-1 is a custom SNES coprocessor (originally a bsnes enhancement chip)
 * that allows streaming high-quality audio from external PCM data packs.
 * These functions let the game engine check whether MSU-1 audio is active
 * and trigger track changes in response to in-game music cues.
 * --------------------------------------------------------------------------- */

// Things for msu

// Returns true if the given SPC700 track ID is currently playing via MSU-1.
bool ZeldaIsPlayingMusicTrack(uint8 track);

// Same check as above, but preserves a known bug from the original SNES code
// where certain track comparisons use an off-by-one index. Kept for accuracy.
bool ZeldaIsPlayingMusicTrackWithBug(uint8 track);

// Begins playback of the specified music track through the MSU-1 audio path.
// If MSU-1 is disabled or the track file is missing, this is a no-op.
void ZeldaPlayMsuAudioTrack(uint8 track);

// Returns true if any music track (SPC700 or MSU-1) is currently playing.
bool ZeldaIsMusicPlaying();

// Enables (1) or disables (0) the MSU-1 enhanced audio subsystem.
// When disabled, all music falls back to the SPC700 emulation path.
void ZeldaEnableMsu(uint8 enable);

/* ---------------------------------------------------------------------------
 * Audio rendering and state management
 *
 * These functions are called by the platform layer (main.c) each frame to
 * produce audio output and to synchronize music state across save/load
 * boundaries.
 * --------------------------------------------------------------------------- */

// Renders |samples| audio frames into |audio_buffer| with the given channel
// count. This mixes both SPC700 emulated output and MSU-1 PCM, applying
// volume scaling. Called by the platform audio callback each frame.
void ZeldaRenderAudio(int16 *audio_buffer, int samples, int channels);

// Drops any audio samples that have been generated but not yet consumed.
// Used when the platform audio system falls behind or after a pause.
void ZeldaDiscardUnusedAudioFrames();

// Restores the music state after loading a save or performing a reset.
// Must be called while the APU lock is already held (hence _Locked suffix).
// |is_reset| distinguishes a full game reset from a save-file load.
void ZeldaRestoreMusicAfterLoad_Locked(bool is_reset);

// Snapshots the current SPC700 music state into SRAM so it persists across
// save operations. Must be called while the APU lock is already held.
void ZeldaSaveMusicStateToRam_Locked();

// Pushes the current APU register state to the SPC700 emulator, synchronizing
// any pending writes that the game engine has queued via zelda_apu_write().
void ZeldaPushApuState();

#endif  // ZELDA3_AUDIO_H_
